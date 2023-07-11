// SPDX-License-Identifier: GPL-2.0
/* Renesas Ethernet Switch Para-Virtualized driver
 *
 * Copyright (C) 2022 EPAM Systems
 */

#include "linux/err.h"
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <xen/interface/grant_table.h>
#include <xen/grant_table.h>
#include <xen/xenbus.h>
#include <xen/events.h>
#include <xen/page.h>
#include "rswitch.h"

/* TODO: get this from rswitch.c */
#define RSWITCH_BACK_BASE_INDEX		3

struct rswitch_vmq_back_info {
	char name[32];
	struct xenbus_device *dev;
	struct rswitch_device *rdev;

	/* This is the state that will be reflected in xenstore when any
	 * active hotplug script completes.
	 */
	enum xenbus_state state;
	enum xenbus_state frontend_state;
	struct rswitch_gwca_chain *tx_chain;
	struct rswitch_gwca_chain *rx_chain;
	struct rswitch_private *rswitch_priv;
	evtchn_port_t tx_evtchn;
	evtchn_port_t rx_evtchn;
	int tx_irq;
	int rx_irq;

	uint32_t osid;
	uint32_t if_num;
};

static struct rswitch_device*
rswitch_vmq_back_ndev_register(struct rswitch_private *priv, int index)
{
	struct platform_device *pdev = priv->pdev;
	struct net_device *ndev;
	struct rswitch_device *rdev;
	int err;

	ndev = alloc_etherdev_mqs(sizeof(struct rswitch_device), 1, 1);
	if (!ndev)
		return ERR_PTR(-ENOMEM);

	SET_NETDEV_DEV(ndev, &pdev->dev);
	ether_setup(ndev);

	rdev = netdev_priv(ndev);
	rdev->ndev = ndev;
	rdev->priv = priv;
	priv->rdev[RSWITCH_BACK_BASE_INDEX + index] = rdev;
	rdev->port = 3;
	rdev->etha = NULL;
	rdev->remote_chain = -1;

	rdev->addr = priv->addr;

	spin_lock_init(&rdev->lock);

	ndev->features = NETIF_F_RXCSUM;
	ndev->hw_features = NETIF_F_RXCSUM;
	ndev->base_addr = (unsigned long)rdev->addr;
	snprintf(ndev->name, IFNAMSIZ, "vmq%d", index);
	ndev->netdev_ops = &rswitch_netdev_ops;

	netif_napi_add(ndev, &rdev->napi, rswitch_poll, 64);

	eth_hw_addr_random(ndev);

	/* Network device register */
	err = register_netdev(ndev);
	if (err)
		goto out_reg_netdev;

	err = rswitch_rxdmac_init(ndev, priv, -1);
	if (err < 0)
		goto out_rxdmac;

	err = rswitch_txdmac_init(ndev, priv, -1);
	if (err < 0)
		goto out_txdmac;

	/* Print device information */
	netdev_info(ndev, "MAC address %pMn", ndev->dev_addr);

	return rdev;

out_txdmac:
	rswitch_rxdmac_free(ndev, priv);

out_rxdmac:
	unregister_netdev(ndev);

out_reg_netdev:
	netif_napi_del(&rdev->napi);
	free_netdev(ndev);

	return ERR_PTR(err);
}

static void rswitch_vmq_back_disconnect(struct xenbus_device *dev)
{
	struct rswitch_vmq_back_info *be = dev_get_drvdata(&dev->dev);

	if (be->rx_irq)
		unbind_from_irqhandler(be->rx_irq, be);
	if (be->tx_irq)
		unbind_from_irqhandler(be->tx_irq, be);
	be->tx_irq = 0;
	be->rx_irq = 0;
}

static int rswitch_vmq_back_remove(struct xenbus_device *dev)
{
	struct rswitch_vmq_back_info *be = dev_get_drvdata(&dev->dev);

	if (be->rdev) {
		rswitch_vmq_back_disconnect(dev);
		rswitch_ndev_unregister(be->rswitch_priv,
					RSWITCH_BACK_BASE_INDEX + be->if_num);
		be->rdev = NULL;
	}

	if (be->rx_chain)
		rswitch_gwca_put(be->rswitch_priv, be->rx_chain);
	if (be->tx_chain)
		rswitch_gwca_put(be->rswitch_priv, be->tx_chain);

	kfree(be);

	dev_set_drvdata(&dev->dev, NULL);

	return 0;
}

/**
 * Entry point to this code when a new device is created.  Allocate the basic
 * structures and switch to InitWait.
 */
static int rswitch_vmq_back_probe(struct xenbus_device *dev,
				  const struct xenbus_device_id *id)
{
	int err = 0;
	struct xenbus_transaction xbt;

	struct rswitch_vmq_back_info *be = kzalloc(sizeof(*be), GFP_KERNEL);

	if (!be) {
		xenbus_dev_fatal(dev, -ENOMEM,
				 "allocating backend structure");
		return -ENOMEM;
	}

	be->dev = dev;
	be->rswitch_priv = rswitch_find_priv();
	if (!be->rswitch_priv) {
		xenbus_dev_fatal(dev, -ENODEV, "Failed to get rswitch priv data");
		return -ENODEV;
	}
	be->tx_chain = rswitch_gwca_get(be->rswitch_priv);
	be->rx_chain = rswitch_gwca_get(be->rswitch_priv);
	if (!be->rx_chain || !be->tx_chain) {
		err = -ENODEV;
		goto fail;
	}

	be->tx_chain->back_info = be;
	be->rx_chain->back_info = be;
	be->tx_chain->dir_tx = true;
	be->rx_chain->dir_tx = false;
	dev_set_drvdata(&dev->dev, be);

	be->osid = xenbus_read_unsigned(dev->otherend, "osid", 255);
	be->tx_chain->osid = be->osid;
	be->rx_chain->osid = be->osid;

	snprintf(be->name, sizeof(be->name) - 1, "rswitch-vmq-osid%d", be->osid);

	be->if_num = xenbus_read_unsigned(dev->otherend, "if-num", 255);

	be->rdev = rswitch_vmq_back_ndev_register(be->rswitch_priv, be->if_num);
	if (IS_ERR(be->rdev)) {
		err = PTR_ERR(be->rdev);
		xenbus_dev_fatal(dev, err, "Failed to allocate local rdev: %d ", err);
		return err;
	}

	do {
		err = xenbus_transaction_start(&xbt);
		if (err)
			goto fail;

		err = xenbus_printf(xbt, dev->nodename, "tx-chain-id", "%d",
				    be->tx_chain->index);
		if (err)
			goto abort_transaction;
		err = xenbus_printf(xbt, dev->nodename, "rx-chain-id", "%d",
				    be->rx_chain->index);
		if (err)
			goto abort_transaction;

		err = xenbus_printf(xbt, dev->nodename, "remote-chain-id", "%d",
				    be->rdev->rx_chain->index);
		if (err)
			goto abort_transaction;

		err = xenbus_transaction_end(xbt, 0);
	} while (err == -EAGAIN);

	if (err) {
		xenbus_dev_fatal(dev, err, "completing transaction");
		goto fail;
	}

	xenbus_switch_state(dev, XenbusStateInitWait);

	return 0;

abort_transaction:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(dev, err, "Failed to write xenstore info\n");
fail:
	if (be->rx_chain)
		rswitch_gwca_put(be->rswitch_priv, be->rx_chain);
	if (be->tx_chain)
		rswitch_gwca_put(be->rswitch_priv, be->tx_chain);

	return err;
}

void rswitch_vmq_back_data_irq(struct rswitch_gwca_chain *c)
{
	struct rswitch_vmq_back_info *be = c->back_info;

	notify_remote_via_evtchn(be->rx_evtchn);
	notify_remote_via_evtchn(be->tx_evtchn);
}

static irqreturn_t rswitch_vmq_back_rx_interrupt(int irq, void *dev_id)
{
	struct rswitch_vmq_back_info *be = dev_id;

	rswitch_enadis_rdev_irqs(be->rdev, true);

	xen_irq_lateeoi(irq, 0);

	return IRQ_HANDLED;
}

static irqreturn_t rswitch_vmq_back_tx_interrupt(int irq, void *dev_id)
{
	struct rswitch_vmq_back_info *be = dev_id;

	rswitch_trigger_chain(be->rswitch_priv, be->tx_chain);

	xen_irq_lateeoi(irq, 0);

	return IRQ_HANDLED;
}


static int rswitch_vmq_back_connect(struct xenbus_device *dev)
{
	struct rswitch_vmq_back_info *be = dev_get_drvdata(&dev->dev);
	unsigned int tx_evt;
	unsigned int rx_evt;
	int err;

	err = xenbus_gather(XBT_NIL, dev->otherend,
			    "tx-evtch", "%u", &tx_evt,
			    "rx-evtch", "%u", &rx_evt,
			    NULL);
	if (err) {
		xenbus_dev_fatal(dev, err, "Failed to read front-end info: %d", err);
		return err;
	}

	be->tx_evtchn = tx_evt;
	be->rx_evtchn = rx_evt;

	err = bind_interdomain_evtchn_to_irqhandler_lateeoi(dev->otherend_id,
		tx_evt, rswitch_vmq_back_tx_interrupt, 0,
		be->name, be);
	if (err < 0) {
		xenbus_dev_fatal(dev, err, "Failed to bind tx_evt IRQ: %d", err);
		return err;
	}
	be->tx_irq = err;

	err = bind_interdomain_evtchn_to_irqhandler_lateeoi(dev->otherend_id,
	        rx_evt, rswitch_vmq_back_rx_interrupt, 0,
		be->name, be);
	if (err < 0) {
		xenbus_dev_fatal(dev, err, "Failed to bind rx_evt IRQ: %d", err);
		return err;
	}
	be->rx_irq = err;

	rswitch_gwca_chain_register(be->rswitch_priv, be->tx_chain, false);
	rswitch_gwca_chain_register(be->rswitch_priv, be->rx_chain, true);

	notify_remote_via_evtchn(tx_evt);
	notify_remote_via_evtchn(rx_evt);

	be->rdev->remote_chain = be->rx_chain->index;

	return 0;
}

static void set_backend_state(struct xenbus_device *dev,
			      enum xenbus_state state)
{
	while (dev->state != state) {
		switch (dev->state) {
		case XenbusStateClosed:
			switch (state) {
			case XenbusStateInitWait:
			case XenbusStateConnected:
				xenbus_switch_state(dev, XenbusStateInitWait);
				break;
			case XenbusStateClosing:
				xenbus_switch_state(dev, XenbusStateClosing);
				break;
			default:
				WARN_ON(1);
			}
			break;
		case XenbusStateInitWait:
		case XenbusStateInitialised:
			switch (state) {
			case XenbusStateConnected:
				if (rswitch_vmq_back_connect(dev))
					return;
				xenbus_switch_state(dev, XenbusStateConnected);
				break;
			case XenbusStateClosing:
			case XenbusStateClosed:
				xenbus_switch_state(dev, XenbusStateClosing);
				break;
			default:
				WARN_ON(1);
			}
			break;
		case XenbusStateConnected:
			switch (state) {
			case XenbusStateInitWait:
			case XenbusStateClosing:
			case XenbusStateClosed:
				rswitch_vmq_back_disconnect(dev);
				xenbus_switch_state(dev, XenbusStateClosing);
				break;
			default:
				WARN_ON(1);
			}
			break;
		case XenbusStateClosing:
			switch (state) {
			case XenbusStateInitWait:
			case XenbusStateConnected:
			case XenbusStateClosed:
				xenbus_switch_state(dev, XenbusStateClosed);
				break;
			default:
				WARN_ON(1);
			}
			break;
		default:
			WARN_ON(1);
		}
	}
}

/**
 * Callback received when the frontend's state changes.
 */
static void rswitch_vmq_frontend_changed(struct xenbus_device *dev,
					 enum xenbus_state frontend_state)
{
	struct rswitch_vmq_back_info *be = dev_get_drvdata(&dev->dev);

	be->frontend_state = frontend_state;

	switch (frontend_state) {
	case XenbusStateInitialising:
		set_backend_state(dev, XenbusStateInitWait);
		break;

	case XenbusStateInitialised:
	case XenbusStateConnected:
		set_backend_state(dev, XenbusStateConnected);
		break;

	case XenbusStateReconfiguring:
		xenbus_switch_state(dev, XenbusStateReconfigured);
		break;

	case XenbusStateClosing:
		set_backend_state(dev, XenbusStateClosing);
		break;

	case XenbusStateClosed:
		set_backend_state(dev, XenbusStateClosed);
		if (xenbus_dev_is_online(dev))
			break;
		fallthrough;	/* if not online */
	case XenbusStateUnknown:
		set_backend_state(dev, XenbusStateClosed);
		device_unregister(&dev->dev);
		break;

	default:
		xenbus_dev_fatal(dev, -EINVAL, "saw state %d at frontend",
				 frontend_state);
		break;
	}
}

static const struct xenbus_device_id rswitch_vmq_ids[] = {
	{ "renesas_vmq" },
	{ "" }
};

static struct xenbus_driver rswitch_vmq_driver = {
	.ids = rswitch_vmq_ids,
	.probe = rswitch_vmq_back_probe,
	.remove = rswitch_vmq_back_remove,
	.otherend_changed = rswitch_vmq_frontend_changed,
	.allow_rebind = false,
};

static int __init rswitch_vmq_back_init(void)
{
	if (!xen_domain())
		return -ENODEV;

	return xenbus_register_backend(&rswitch_vmq_driver);
}

module_init(rswitch_vmq_back_init);

static void rswitch_vmq_back_exit(void)
{
	return xenbus_unregister_driver(&rswitch_vmq_driver);
}

module_exit(rswitch_vmq_back_exit);
