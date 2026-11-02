//
// Created by User on 2026/11/1.
// # PHY 设备发现、phy_connect、phy_driver 注册
//
// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "net_ndnic_phy: " fmt

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/phy.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>

#include "net_ndnic.h"
#include "net_ndnic_compat.h"
#include "net_ndnic_phy.h"

static int nd_phy_of_get_reset(struct nd_phy *phy, struct device_node *np)
{
	struct nd_phy_reset_seq *seq = &phy->cfg.reset;

	if (of_property_read_u32(np, "reset-assert-us", &seq->assert_us))
		seq->assert_us = 1000;
	if (of_property_read_u32(np, "reset-deassert-us", &seq->deassert_us))
		seq->deassert_us = 1000;

	phy->reset_gpio = devm_gpiod_get_optional(phy->dev, "reset",
						  GPIOD_OUT_HIGH);
	if (IS_ERR(phy->reset_gpio)) {
		int err = PTR_ERR(phy->reset_gpio);
		dev_err(phy->dev, "reset GPIO 获取失败: %d\n", err);
		return err;
	}
	return 0;
}

int nd_phy_reset_sequence(struct nd_phy *phy)
{
	if (!phy->reset_gpio)
		return 0;

	gpiod_set_value_cansleep(phy->reset_gpio, 0);
	if (phy->cfg.reset.assert_us)
		usleep_range(phy->cfg.reset.assert_us,
			     phy->cfg.reset.assert_us + 100);

	gpiod_set_value_cansleep(phy->reset_gpio, 1);
	if (phy->cfg.reset.deassert_us)
		usleep_range(phy->cfg.reset.deassert_us,
			     phy->cfg.reset.deassert_us + 200);

	dev_info(phy->dev, "PHY 复位完成 (assert=%uus, deassert=%uus)\n",
		 phy->cfg.reset.assert_us, phy->cfg.reset.deassert_us);
	return 0;
}

static int nd_phy_apply_board_cfg(struct nd_phy *phy)
{
	struct phy_device *phydev = phy->phydev;

	if (phy->cfg.disable_eee) {
		nd_phy_disable_eee_internal(phydev);
		phy->eee_disabled = true;
		atomic64_inc(&phy->stats.eee_cleared_events);
		dev_info(phy->dev, "PHY EEE 已禁用\n");
	}

	if (phy->cfg.downshift_count) {
		int err = nd_phy_set_downshift(phydev,
					       phy->cfg.downshift_count);
		if (err == -EOPNOTSUPP) {
			/* 旧内核走板级寄存器写回退 */
			err = nd_phy_configure_downshift(phy);
		}
		if (err)
			dev_warn(phy->dev, "downshift 配置失败: %d\n", err);
	}

	return 0;
}

/*
 * 主驱动 probe 中调用：解析 DTS、执行复位、连接 PHY、应用板级覆盖。
 * 返回 0 表示 PHY 已就绪，之后可以调用 nd_phy_start()。
 */
int nd_phy_init(struct nd_adapter *adapter, struct device_node *np)
{
	struct net_device *ndev = adapter->netdev;
	struct nd_phy *phy = &adapter->phy;
	struct phy_device *phydev;
	int err;

	memset(phy, 0, sizeof(*phy));
	phy->dev = &adapter->pdev->dev;
	atomic64_set(&phy->stats.link_up_events, 0);
	atomic64_set(&phy->stats.link_down_events, 0);
	atomic64_set(&phy->stats.an_restart_events, 0);
	atomic64_set(&phy->stats.downshift_events, 0);
	atomic64_set(&phy->stats.mdio_errors, 0);
	atomic64_set(&phy->stats.eee_cleared_events, 0);
	atomic64_set(&phy->stats.reg_dump_events, 0);

	struct net_device *ndev = adapter->netdev;
	struct nd_phy *phy = &adapter->phy;
	struct device_node *phy_np;
	struct phy_device *phydev;
	const char *mode_str;
	int err;

	/* 无 DTS（纯 x86 平台）时跳过 */
	if (!mac_np)
		return 0;

	/* 1. 从 MAC 节点读 phy-mode */
	err = of_property_read_string(mac_np, "phy-mode", &mode_str);
	if (err) {
		dev_err(phy->dev, "MAC 节点缺少 phy-mode\n");
		return err;
	}
	phy->cfg.mode = nd_phy_mode_from_string(mode_str);

	/* 2. 从 MAC 节点找 phy-handle */
	phy_np = of_parse_phandle(mac_np, "phy-handle", 0);
	if (!phy_np) {
		dev_err(phy->dev, "MAC 节点缺少 phy-handle\n");
		return -ENODEV;
	}

	/* 3. 从 PHY 节点读 reset 配置和板级覆盖 */
	nd_phy_of_get_reset(phy, phy_np);
	phy->cfg.disable_eee = of_property_read_bool(phy_np, "nd,disable-eee");
	of_property_read_u32(phy_np, "nd,downshift-count",
				 &phy->cfg.downshift_count);

	/* 4. 执行复位（在 MDIO 访问之前） */
	err = nd_phy_reset_sequence(phy);
	if (err)
		goto out_put_np;

	/* 5. 连接 PHY（此时 phy_np 才是 PHY 节点） */
	phydev = of_phy_connect(ndev, phy_np, nd_phy_link_adjust, 0,
				nd_phy_interface_from_mode(phy->cfg.mode));
	if (!phydev) {
		err = -ENODEV;
		goto out_put_np;
	}
	phy->phydev = phydev;

	/* PHY ID 完整性校验：全 0 或全 1 说明 MDIO 总线不可靠 */
	if (phydev->phy_id == 0 || phydev->phy_id == 0xffffffff) {
		dev_err(phy->dev, "PHY ID 无效: 0x%08x（MDIO 故障或地址错误）\n",
			phydev->phy_id);
		err = -EIO;
		goto err_disconnect;
	}

	err = nd_phy_validate_timing(phy, phydev->interface);
	if (err) {
		dev_err(phy->dev, "MAC/PHY 时序校验失败: %d\n", err);
		goto err_disconnect;
	}
	phy->timing_validated = true;

	err = nd_phy_apply_rgmii_delay(phy);
	if (err)
		dev_warn(phy->dev, "RGMII 延迟写入失败: %d（继续）\n", err);

	err = nd_phy_apply_board_cfg(phy);
	if (err)
		dev_warn(phy->dev, "板级 PHY 配置部分失败: %d\n", err);

	INIT_DELAYED_WORK(&phy->watchdog, nd_phy_link_watchdog);
	schedule_delayed_work(&phy->watchdog, msecs_to_jiffies(2000));

	dev_info(phy->dev,
		 "PHY 就绪: id=0x%08x, mode=%d, interface=%d, eee=%s\n",
		 phydev->phy_id, phy->cfg.mode, phydev->interface,
		 phy->eee_disabled ? "off" : "on");
	return 0;

err_disconnect:
	phy_disconnect(phydev);
	phy->phydev = NULL;
	return err;
}

void nd_phy_deinit(struct nd_adapter *adapter)
{
	struct nd_phy *phy = &adapter->phy;

	cancel_delayed_work_sync(&phy->watchdog);

	if (phy->phydev) {
		phy_disconnect(phy->phydev);
		phy->phydev = NULL;
	}
	/* reset_gpio 由 devm 管理，无需手动释放 */
	dev_info(phy->dev, "PHY 已释放\n");
}

int nd_phy_start(struct nd_adapter *adapter)
{
	struct nd_phy *phy = &adapter->phy;

	if (!phy->phydev)
		return -ENODEV;

	phy_start(phy->phydev);
	return 0;
}

void nd_phy_stop(struct nd_adapter *adapter)
{
	struct nd_phy *phy = &adapter->phy;

	if (phy->phydev)
		phy_stop(phy->phydev);
}
