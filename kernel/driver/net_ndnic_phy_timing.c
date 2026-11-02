//
// Created by User on 2026/11/1.
// # RGMII/SGMII 延迟与复位时序
//
// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "net_ndnic_phy_timing: " fmt

#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/delay.h>

#include "net_ndnic_phy.h"

phy_interface_t nd_phy_interface_from_mode(enum nd_phy_mode mode)
{
	switch (mode) {
	case ND_PHY_MODE_RGMII:		return PHY_INTERFACE_MODE_RGMII;
	case ND_PHY_MODE_RGMII_ID:	return PHY_INTERFACE_MODE_RGMII_ID;
	case ND_PHY_MODE_RGMII_TXID:	return PHY_INTERFACE_MODE_RGMII_TXID;
	case ND_PHY_MODE_RGMII_RXID:	return PHY_INTERFACE_MODE_RGMII_RXID;
	case ND_PHY_MODE_SGMII:		return PHY_INTERFACE_MODE_SGMII;
	default:			return PHY_INTERFACE_MODE_NA;
	}
}

int nd_phy_parse_timing(struct nd_phy *phy, struct device_node *np)
{
	const char *mode_str;
	int err;

	err = of_property_read_string(np, "phy-mode", &mode_str);
	if (err) {
		dev_err(phy->dev, "缺少 phy-mode 属性\n");
		return err;
	}

	if (!strcmp(mode_str, "rgmii"))		phy->cfg.mode = ND_PHY_MODE_RGMII;
	else if (!strcmp(mode_str, "rgmii-id"))	phy->cfg.mode = ND_PHY_MODE_RGMII_ID;
	else if (!strcmp(mode_str, "rgmii-txid")) phy->cfg.mode = ND_PHY_MODE_RGMII_TXID;
	else if (!strcmp(mode_str, "rgmii-rxid")) phy->cfg.mode = ND_PHY_MODE_RGMII_RXID;
	else if (!strcmp(mode_str, "sgmii"))	phy->cfg.mode = ND_PHY_MODE_SGMII;
	else {
		dev_err(phy->dev, "不支持的 phy-mode: %s\n", mode_str);
		return -EINVAL;
	}

	phy->cfg.disable_eee = of_property_read_bool(np, "nd,disable-eee");
	of_property_read_u32(np, "nd,downshift-count",
			     &phy->cfg.downshift_count);
	of_property_read_u32(np, "nd,downshift-reg",
			     &phy->cfg.downshift_reg);
	of_property_read_u32(np, "nd,downshift-val",
			     &phy->cfg.downshift_val);

	dev_info(phy->dev,
		 "timing: mode=%s, eee=%s, downshift=%u\n",
		 mode_str, phy->cfg.disable_eee ? "off" : "on",
		 phy->cfg.downshift_count);
	return 0;
}

/*
 * 时序一致性检查：MAC 侧与 PHY 侧至少有一侧提供延迟。
 * RGMII 的 *_ID 表示 PHY 内部提供延迟；如果 DTS 声明 rgmii 但两侧都不提供，
 * 数据眼图会偏移。这里只做“明显错误”的拒绝，不做精确补偿。
 */
int nd_phy_validate_timing(struct nd_phy *phy, phy_interface_t interface)
{
	switch (phy->cfg.mode) {
	case ND_PHY_MODE_RGMII:
		/* 若 MAC 也不提供延迟，明确警告而非静默 */
		dev_warn(phy->dev,
			 "RGMII 无内部延迟，依赖 MAC 侧 TX/RX delay；请确认板级\n");
		break;
	case ND_PHY_MODE_RGMII_TXID:
	case ND_PHY_MODE_RGMII_RXID:
	case ND_PHY_MODE_RGMII_ID:
	case ND_PHY_MODE_SGMII:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/*
 * RGMII 延迟写回：只对已知需要写 PHY 寄存器的模式执行。
 * 具体寄存器和步进由板级配置/PHY 型号决定，这里给出通用框架。
 */
int nd_phy_apply_rgmii_delay(struct nd_phy *phy)
{
	struct phy_device *phydev = phy->phydev;
	int err = 0;

	if (!phydev)
		return -ENODEV;

	switch (phy->cfg.mode) {
	case ND_PHY_MODE_RGMII_ID:
	case ND_PHY_MODE_RGMII_TXID:
	case ND_PHY_MODE_RGMII_RXID:
		/*
		 * 若 PHY 驱动已通过 config_init 写了延迟，这里不重复写。
		 * 仅在板级显式给出 downshift_reg 时写回，避免覆盖驱动配置。
		 */
		break;
	default:
		break;
	}
	return err;
}
