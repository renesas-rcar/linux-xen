// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Renesas R-Car V3U and Gen4 Series SoCs
 *  Copyright (C) 2022 Renesas Electronics Corporation
 *
 * Author: Hoang Vo <hoang.vo.eb@renesas.com>
 */

#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/gpio/consumer.h>

#include "../../pci.h"
#include "pcie-designware.h"

/* PCI Express capability */
#define EXPCAP(x)		(0x0070 + (x))
/* Link Capabilities - Maximum Link Width */
#define	PCI_EXP_LNKCAP_MLW_X1	BIT(4)
#define	PCI_EXP_LNKCAP_MLW_X2	BIT(5)
#define	PCI_EXP_LNKCAP_MLW_X4	BIT(6)

/* PCIEC PHY */
#define	REFCLKCTRLP0		0x0B8
#define	REFCLKCTRLP1		0x2B8
#define	PHY_REF_CLKDET_EN	BIT(10)
#define	PHY_REF_REPEAT_CLK_EN	BIT(9)
#define	PHY_REF_USE_PAD		BIT(2)

/* ASPM L1 PM Substates */
#define L1PSCAP(x)		(0x01BC + (x))

/* Renesas-specific */
#define	PCIEMSR0		0x0000
#define	BIFUR_MOD_SET_ON	(0x1 << 0)
#define	DEVICE_TYPE_RC		(0x4 << 2)
#define	APP_SRIS_MODE		BIT(6)

#define	PCIERSTCTRL1		0x0014
#define	APP_HOLD_PHY_RST	BIT(16)
#define	APP_LTSSM_ENABLE	BIT(0)

#define	MSICAP0F0		0x0050
#define	MSIE			BIT(16)
#define PCIEINTSTS0		0x0084
#define PCIEINTSTS0EN		0x0310
#define	MSI_CTRL_INT		BIT(26)
#define	SMLH_LINK_UP		BIT(7)
#define	RDLH_LINK_UP		BIT(6)

/* Max Payload Size */
#define	MPS_256			BIT(5)

/*Power Management*/
#define	PCIEPWRMNGCTRL		0x0070
#define CLK_REG			BIT(11)
#define CLK_PM			BIT(10)
#define	PMCAP1F0		0x0044
#define	PMEE_EN			BIT(8)

/* Error Status Clear */
#define	PCIEERRSTS0CLR		0x033C
#define	PCIEERRSTS1CLR		0x035C
#define	PCIEERRSTS2CLR		0x0360
#define	ERRSTS0_EN		GENMASK(10, 6)
#define	ERRSTS1_EN		GENMASK(29, 0)
#define	ERRSTS2_EN		GENMASK(5, 0)

/* PORT LOGIC */
#define	PRTLGC2			0x0708
#define	DO_DESKEW_FOR_SRIS	BIT(23)
#define	PRTLGC5			0x0714
#define	LANE_CONFIG		BIT(6)

#define PCIEERRSTS0EN           0x030C
#define CFG_SYS_ERR_RC          GENMASK(10, 9)
#define CFG_SAFETY_UNCORR_CORR  GENMASK(5, 4)

/* PCI Shadow offset */
#define SHADOW_REG(x)		(0x2000 + (x))
/* BAR Mask registers */
#define BAR0_MASK		0x0010
#define BAR1_MASK		0x0014

#define DWC_VERSION		0x520A

#define	MAX_RETRIES		10

#define to_renesas_pcie(x)	dev_get_drvdata((x)->dev)

struct renesas_pcie {
	struct dw_pcie			*pci;
	void __iomem			*base;
	void __iomem			*phy_base;
	struct clk			*bus_clk;
	struct reset_control		*rst;
	struct gpio_desc		*clkreq;
	void __iomem			*base_shared;
	struct clk			*clk_shared;
	u32				msi_irq_en[MAX_MSI_CTRLS];
	u32				msi_irq_mask[MAX_MSI_CTRLS];
};

static const struct of_device_id renesas_pcie_of_match[];

static u32 renesas_pcie_readl(struct renesas_pcie *pcie, u32 reg)
{
	return readl(pcie->base + reg);
}

static void renesas_pcie_writel(struct renesas_pcie *pcie, u32 reg, u32 val)
{
	writel(val, pcie->base + reg);
}

static u32 renesas_pcie_phy_readl(struct renesas_pcie *pcie, u32 reg)
{
	return readl(pcie->phy_base + reg);
}

static void renesas_pcie_phy_writel(struct renesas_pcie *pcie, u32 reg, u32 val)
{
	writel(val, pcie->phy_base + reg);
}

static u32 renesas_pcie_shared_readl(struct renesas_pcie *pcie, u32 reg)
{
	return readl(pcie->base_shared + reg);
}

static void renesas_pcie_shared_writel(struct renesas_pcie *pcie, u32 reg, u32 val)
{
	writel(val, pcie->base_shared + reg);
}

static void renesas_pcie_ltssm_enable(struct renesas_pcie *pcie,
				      bool enable)
{
	u32 val;

	val = renesas_pcie_readl(pcie, PCIERSTCTRL1);
	if (enable) {
		val |= APP_LTSSM_ENABLE;
		val &= ~APP_HOLD_PHY_RST;
	} else {
		val &= ~APP_LTSSM_ENABLE;
		val |= APP_HOLD_PHY_RST;
	}
	renesas_pcie_writel(pcie, PCIERSTCTRL1, val);
}

static void renesas_pcie_retrain_link(struct dw_pcie *pci)
{
	u32 val, lnksta, retries;

	val = dw_pcie_readl_dbi(pci, EXPCAP(PCI_EXP_LNKCTL));
	val |= PCI_EXP_LNKCTL_RL;
	dw_pcie_writel_dbi(pci, EXPCAP(PCI_EXP_LNKCTL), val);

	/* Wait for link retrain */
	for (retries = 0; retries <= MAX_RETRIES; retries++) {
		lnksta = dw_pcie_readw_dbi(pci, EXPCAP(PCI_EXP_LNKSTA));

		/* Check retrain flag */
		if (!(lnksta & PCI_EXP_LNKSTA_LT))
			break;
		mdelay(1);
	}
}

static void renesas_pcie_check_speed(struct dw_pcie *pci)
{
	u32 lnkcap, lnksta;

	lnkcap = dw_pcie_readl_dbi(pci, EXPCAP(PCI_EXP_LNKCAP));
	lnksta = dw_pcie_readw_dbi(pci, EXPCAP(PCI_EXP_LNKSTA));

	if ((lnksta & PCI_EXP_LNKSTA_CLS) != (lnkcap & PCI_EXP_LNKCAP_SLS))
		renesas_pcie_retrain_link(pci);
}

static int renesas_pcie_link_up(struct dw_pcie *pci)
{
	struct renesas_pcie *pcie = to_renesas_pcie(pci);
	u32 val, mask;

	val = renesas_pcie_readl(pcie, PCIEINTSTS0);
	mask = RDLH_LINK_UP | SMLH_LINK_UP;

	renesas_pcie_check_speed(pci);

	return (val & mask) == mask;
}

static int renesas_pcie_start_link(struct dw_pcie *pci)
{
	struct renesas_pcie *pcie = to_renesas_pcie(pci);

	renesas_pcie_ltssm_enable(pcie, true);

	return 0;
}

static void renesas_pcie_stop_link(struct dw_pcie *pci)
{
	struct renesas_pcie *pcie = to_renesas_pcie(pci);

	renesas_pcie_ltssm_enable(pcie, false);
}

static const struct dw_pcie_ops dw_pcie_ops = {
	.start_link = renesas_pcie_start_link,
	.stop_link = renesas_pcie_stop_link,
	.link_up = renesas_pcie_link_up,
};

static int renesas_pcie_host_init(struct pcie_port *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	u32 val;
	int ret;

	dw_pcie_setup_rc(pp);

	dw_pcie_dbi_ro_wr_en(pci);

	/* Set Max Link Width */
	val = dw_pcie_readl_dbi(pci, EXPCAP(PCI_EXP_LNKCAP));
	val &= ~PCI_EXP_LNKCAP_MLW;
	switch (pci->num_lanes) {
	case 1:
		val |= PCI_EXP_LNKCAP_MLW_X1;
		break;
	case 2:
		val |= PCI_EXP_LNKCAP_MLW_X2;
		break;
	case 4:
		val |= PCI_EXP_LNKCAP_MLW_X4;
		break;
	default:
		dev_err(pci->dev, "num-lanes %u: invalid value\n", pci->num_lanes);
		return -EINVAL;
	}
	dw_pcie_writel_dbi(pci, EXPCAP(PCI_EXP_LNKCAP), val);

	dw_pcie_dbi_ro_wr_dis(pci);

	if (!dw_pcie_link_up(pci)) {
		ret = renesas_pcie_start_link(pci);
		if (ret)
			return ret;
	}

	/* Ignore errors, the link may come up later */
	if (dw_pcie_wait_for_link(pci))
		dev_info(pci->dev, "PCIe link down\n");

	dw_pcie_msi_init(pp);

	return 0;
}

static void renesas_pcie_set_num_vectors(struct pcie_port *pp)
{
	pp->num_vectors = MAX_MSI_IRQS;
}

static const struct dw_pcie_host_ops renesas_pcie_host_ops = {
	.host_init = renesas_pcie_host_init,
	.set_num_vectors = renesas_pcie_set_num_vectors,
};

static int renesas_add_pcie_port(struct renesas_pcie *pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = pcie->pci;
	struct pcie_port *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	u32 val;
	int ret;

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		pp->msi_irq = platform_get_irq_byname(pdev, "msi");
		if (pp->msi_irq < 0)
			return pp->msi_irq;

		/* Enable MSI interrupt signal */
		val = renesas_pcie_readl(pcie, PCIEINTSTS0EN);
		val |= MSI_CTRL_INT;
		renesas_pcie_writel(pcie, PCIEINTSTS0EN, val);
	}

	pp->ops = &renesas_pcie_host_ops;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host\n");
		return ret;
	}

	return 0;
}

static void renesas_pcie_init_rc(struct renesas_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	int val;

	/* Device type selection - Root Complex */
	val = renesas_pcie_readl(pcie, PCIEMSR0);
	val |= DEVICE_TYPE_RC;
	renesas_pcie_writel(pcie, PCIEMSR0, val);

	if (pcie->base_shared) {
		clk_prepare_enable(pcie->clk_shared);
		val = renesas_pcie_shared_readl(pcie, PCIEMSR0);
		val |= BIFUR_MOD_SET_ON;
		renesas_pcie_shared_writel(pcie, PCIEMSR0, val);
		clk_disable_unprepare(pcie->clk_shared);
	}

	/* Enable SRIS mode */
	val = renesas_pcie_readl(pcie, PCIEMSR0);
	val |= APP_SRIS_MODE;
	renesas_pcie_writel(pcie, PCIEMSR0, val);

	/* Error Status Enable */
	val = renesas_pcie_readl(pcie, PCIEERRSTS0EN);
	val |= CFG_SYS_ERR_RC | CFG_SAFETY_UNCORR_CORR;
	renesas_pcie_writel(pcie, PCIEERRSTS0EN, val);

	/* Error Status Clear */
	val = renesas_pcie_readl(pcie, PCIEERRSTS0CLR);
	val |= ERRSTS0_EN;
	renesas_pcie_writel(pcie, PCIEERRSTS0CLR, val);

	val = renesas_pcie_readl(pcie, PCIEERRSTS1CLR);
	val |= ERRSTS1_EN;
	renesas_pcie_writel(pcie, PCIEERRSTS1CLR, val);

	val = renesas_pcie_readl(pcie, PCIEERRSTS2CLR);
	val |= ERRSTS2_EN;
	renesas_pcie_writel(pcie, PCIEERRSTS2CLR, val);

	/* Power Management */
	val = renesas_pcie_readl(pcie, PCIEPWRMNGCTRL);
	val |= CLK_REG | CLK_PM;
	renesas_pcie_writel(pcie, PCIEPWRMNGCTRL, val);

	/* Enable DBI read-only registers for writing */
	dw_pcie_dbi_ro_wr_en(pci);

	val = dw_pcie_readl_dbi(pci, MSICAP0F0);
	val |= MSIE;
	dw_pcie_writel_dbi(pci, MSICAP0F0, val);

	/* Enable L1 Substates */
	val = dw_pcie_readl_dbi(pci, L1PSCAP(PCI_L1SS_CTL1));
	val &= ~PCI_L1SS_CTL1_L1SS_MASK;
	val |= PCI_L1SS_CTL1_PCIPM_L1_2 | PCI_L1SS_CTL1_PCIPM_L1_1 |
		PCI_L1SS_CTL1_ASPM_L1_2 | PCI_L1SS_CTL1_ASPM_L1_1;
	dw_pcie_writel_dbi(pci, L1PSCAP(PCI_L1SS_CTL1), val);

	/* Disable BARs */
	dw_pcie_writel_dbi(pci, SHADOW_REG(BAR0_MASK), 0x0);
	dw_pcie_writel_dbi(pci, SHADOW_REG(BAR1_MASK), 0x0);

	/* Set Max Payload Size */
	val = dw_pcie_readl_dbi(pci, EXPCAP(PCI_EXP_DEVCTL));
	val &= ~PCI_EXP_DEVCTL_PAYLOAD;
	val |= MPS_256;
	dw_pcie_writel_dbi(pci, EXPCAP(PCI_EXP_DEVCTL), val);

	/* Set Root Control */
	val = dw_pcie_readl_dbi(pci, EXPCAP(PCI_EXP_RTCTL));
	val |= PCI_EXP_RTCTL_SECEE | PCI_EXP_RTCTL_SENFEE |
		PCI_EXP_RTCTL_SEFEE | PCI_EXP_RTCTL_PMEIE |
		PCI_EXP_RTCTL_CRSSVE;
	dw_pcie_writel_dbi(pci, EXPCAP(PCI_EXP_RTCTL), val);

	/* Enable SERR */
	val = dw_pcie_readb_dbi(pci, PCI_BRIDGE_CONTROL);
	val |= PCI_BRIDGE_CTL_SERR;
	dw_pcie_writeb_dbi(pci, PCI_BRIDGE_CONTROL, val);

	/* Device control */
	val = dw_pcie_readl_dbi(pci, EXPCAP(PCI_EXP_DEVCTL));
	val |= PCI_EXP_DEVCTL_CERE | PCI_EXP_DEVCTL_NFERE |
		PCI_EXP_DEVCTL_FERE | PCI_EXP_DEVCTL_URRE;
	dw_pcie_writel_dbi(pci, EXPCAP(PCI_EXP_DEVCTL), val);

	/* Enable SRIS mode */
	val = dw_pcie_readl_dbi(pci, PRTLGC2);
	val |= DO_DESKEW_FOR_SRIS;
	dw_pcie_writel_dbi(pci, PRTLGC2, val);

	/* Enable PME */
	val = dw_pcie_readl_dbi(pci, PMCAP1F0);
	val |= PMEE_EN;
	dw_pcie_writel_dbi(pci, PMCAP1F0, val);

	val = dw_pcie_readl_dbi(pci, PRTLGC5);
	val |= LANE_CONFIG;
	dw_pcie_writel_dbi(pci, PRTLGC5, val);

	dw_pcie_dbi_ro_wr_dis(pci);

	/* PCIe PHY setting */
	val = renesas_pcie_phy_readl(pcie, REFCLKCTRLP0);
	val |= PHY_REF_CLKDET_EN | PHY_REF_REPEAT_CLK_EN;
	renesas_pcie_phy_writel(pcie, REFCLKCTRLP0, val);

	val = renesas_pcie_phy_readl(pcie, REFCLKCTRLP1);
	val &= ~PHY_REF_USE_PAD;
	val |= PHY_REF_CLKDET_EN | PHY_REF_REPEAT_CLK_EN;
	renesas_pcie_phy_writel(pcie, REFCLKCTRLP1, val);
}

static int renesas_pcie_host_enable(struct renesas_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	int ret;


	if (pcie->clkreq)
		gpiod_set_value(pcie->clkreq, 1);

	ret = clk_prepare_enable(pcie->bus_clk);
	if (ret) {
		dev_err(pci->dev, "failed to enable bus clock: %d\n", ret);
		goto err_clkreq_off;
	}

	ret = reset_control_deassert(pcie->rst);
	if (ret)
		goto err_clk_disable;

	renesas_pcie_init_rc(pcie);

	return 0;

err_clk_disable:
	clk_disable_unprepare(pcie->bus_clk);

err_clkreq_off:
	if (pcie->clkreq)
		gpiod_set_value(pcie->clkreq, 0);

	return ret;
}

static int renesas_pcie_get_resources(struct renesas_pcie *pcie,
				      struct platform_device *pdev)
{
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	pci->dbi_base = devm_pci_remap_cfg_resource(dev, res);
	if (IS_ERR(pci->dbi_base))
		return PTR_ERR(pci->dbi_base);

	/* Renesas-specific registers */
	pcie->base = devm_platform_ioremap_resource_byname(pdev, "app");
	if (IS_ERR(pcie->base))
		return PTR_ERR(pcie->base);

	pcie->phy_base = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(pcie->phy_base))
		return PTR_ERR(pcie->phy_base);

	pcie->bus_clk = devm_clk_get(dev, "pcie_bus");
	if (IS_ERR(pcie->bus_clk)) {
		dev_err(dev, "cannot get pcie bus clock\n");
		return PTR_ERR(pcie->bus_clk);
	}

	pcie->rst = devm_reset_control_get(dev, NULL);
	if (IS_ERR(pcie->rst)) {
		dev_err(dev, "failed to get Cold-reset\n");
		return PTR_ERR(pcie->rst);
	}

	pcie->clkreq =  devm_gpiod_get(dev, "clkreq", GPIOD_OUT_LOW);
	if (IS_ERR(pcie->clkreq))
		pcie->clkreq = NULL;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "shared");
	if (res) {
		pcie->base_shared = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(pcie->base_shared))
			pcie->base_shared = NULL;
	}

	pcie->clk_shared = devm_clk_get(dev, "shared");
	if (IS_ERR(pcie->clk_shared))
		pcie->clk_shared = NULL;

	return 0;
}

static int renesas_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci;
	struct renesas_pcie *pcie;
	int err;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;
	pci->ops = &dw_pcie_ops;
	pci->version = DWC_VERSION;

	pcie->pci = pci;

	pm_runtime_enable(pci->dev);
	err = pm_runtime_get_sync(pci->dev);
	if (err < 0) {
		dev_err(pci->dev, "pm_runtime_get_sync failed\n");
		goto err_pm_put;
	}

	err = renesas_pcie_get_resources(pcie, pdev);
	if (err < 0) {
		dev_err(dev, "failed to request resource: %d\n", err);
		goto err_pm_put;
	}

	platform_set_drvdata(pdev, pcie);

	err = renesas_pcie_host_enable(pcie);
	if (err < 0)
		goto err_pm_put;

	err = renesas_add_pcie_port(pcie, pdev);
	if (err < 0)
		goto err_host_disable;

	return 0;

err_host_disable:
	reset_control_assert(pcie->rst);
	clk_disable_unprepare(pcie->bus_clk);
	if (pcie->clk_shared)
		clk_disable_unprepare(pcie->clk_shared);
	if (pcie->clkreq)
		gpiod_set_value(pcie->clkreq, 0);

err_pm_put:
	pm_runtime_put(dev);
	pm_runtime_disable(dev);

	return err;
}

static void renesas_pcie_msi_save_restore(struct renesas_pcie *pcie,
					  bool restore)
{
	struct dw_pcie *pci = pcie->pci;
	struct pcie_port *pp = &pci->pp;
	u32 num_ctrl, ctrl;

	num_ctrl = pp->num_vectors / MAX_MSI_IRQS_PER_CTRL;
	dw_pcie_dbi_ro_wr_en(pci);

	if (restore) {
		/* Restore MSI state in resume */
		for (ctrl = 0; ctrl < num_ctrl; ctrl++) {
			dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_ENABLE +
					   (ctrl * MSI_REG_CTRL_BLOCK_SIZE),
					   pcie->msi_irq_en[ctrl]);

			dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK +
					   (ctrl * MSI_REG_CTRL_BLOCK_SIZE),
					   pcie->msi_irq_mask[ctrl]);
		}
	} else {
		/* Save MSI state in suspend  */
		for (ctrl = 0; ctrl < num_ctrl; ctrl++) {
			pcie->msi_irq_en[ctrl] =
			dw_pcie_readl_dbi(pci, PCIE_MSI_INTR0_ENABLE +
					  (ctrl * MSI_REG_CTRL_BLOCK_SIZE));

			pcie->msi_irq_mask[ctrl] =
			dw_pcie_readl_dbi(pci, PCIE_MSI_INTR0_MASK +
					  (ctrl * MSI_REG_CTRL_BLOCK_SIZE));
		}
	}

	dw_pcie_dbi_ro_wr_dis(pci);
}

static int renesas_pcie_suspend_noirq(struct device *dev)
{
	struct renesas_pcie *pcie = dev_get_drvdata(dev);

	renesas_pcie_msi_save_restore(pcie, false);

	return 0;
}

static int renesas_pcie_resume_noirq(struct device *dev)
{
	struct renesas_pcie *pcie = dev_get_drvdata(dev);
	struct dw_pcie *pci = pcie->pci;
	struct pcie_port *pp = &pci->pp;
	u32 err, val, ret;

	if (pcie->clkreq)
		gpiod_set_value(pcie->clkreq, 1);

	ret = clk_prepare_enable(pcie->bus_clk);
	if (ret) {
		dev_err(pci->dev,
			"failed to enable bus clock: %d\n", ret);
	}

	err = reset_control_deassert(pcie->rst);
	if (err)
		return err;

	/* Re-initialize Root Complex */
	renesas_pcie_init_rc(pcie);

	/* Re-enable MSI interrupt signal */
	renesas_pcie_msi_save_restore(pcie, true);

	/* Skip resetting MSI in framework */
	pci_no_msi();

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		val = renesas_pcie_readl(pcie, PCIEINTSTS0EN);
		val |= MSI_CTRL_INT;
		renesas_pcie_writel(pcie, PCIEINTSTS0EN, val);
	}

	dw_pcie_setup_rc(pp);

	/* Reset MSI flags */
	pci_has_msi();

	if (!dw_pcie_link_up(pci)) {
		ret = renesas_pcie_start_link(pci);
		if (ret)
			return ret;
	}

	dw_pcie_msi_init(pp);

	return 0;
}

static int renesas_pcie_resume(struct device *dev)
{
	struct renesas_pcie *pcie = dev_get_drvdata(dev);
	struct dw_pcie *pci = pcie->pci;

	if (dw_pcie_wait_for_link(pci))
		return 0;

	return 0;
}

static const struct dev_pm_ops renesas_pcie_pm_ops = {
	.suspend_noirq = renesas_pcie_suspend_noirq,
	.resume_noirq = renesas_pcie_resume_noirq,
	.resume    = renesas_pcie_resume,
};

static const struct of_device_id renesas_pcie_of_match[] = {
	{ .compatible = "renesas,r8a779a0-pcie", },
	{ .compatible = "renesas,r8a779f0-pcie", },
	{ .compatible = "renesas,r8a779g0-pcie", },
	{},
};

static struct platform_driver renesas_pcie_driver = {
	.driver = {
		.name = "pcie-renesas",
		.of_match_table = renesas_pcie_of_match,
		.pm     = &renesas_pcie_pm_ops,
	},
	.probe = renesas_pcie_probe,
};
builtin_platform_driver(renesas_pcie_driver);
