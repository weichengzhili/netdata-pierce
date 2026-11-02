//
// Created by User on 2026/11/1.
// # 链路状态机、自协商、downshift、EEE 策略
//
// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "net_ndnic_phy_link: " fmt

#include <linux/phy.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/workqueue.h>
#include <linux/delay.h>

#include "net_ndnic.h"
#include "net_ndnic_compat.h"
#include "net_ndnic_phy.h"

int nd_phy_configure_downshift(struct nd_phy *phy)
{
	struct phy_device *phydev = phy->phydev;
	int err;

	if (!phydev)
		return -ENODEV;

	/* 优先走标准接口 */
	err = nd_phy_set_downshift(phydev, phy->cfg.downshift_count);
	if (err != -EOPNOTSUPP)
		return err;

	/* 回退：板级给出寄存器地址时直接写 */
	if (!phy->cfg.downshift_reg)
		return -EOPNOTSUPP;

	err = phy_write(phydev, phy->cfg.downshift_reg,
			phy->cfg.downshift_val);
	if (err)
		nd_phy_inc_mdio_error(phy);
	return err;
}

int nd_phy_link_configure(struct nd_phy *phy)
{
	struct phy_device *phydev = phy->phydev;

	if (!phydev)
		return -ENODEV;

	if (phy->cfg.disable_eee && !phy->eee_disabled) {
		nd_phy_disable_eee_internal(phydev);
		phy->eee_disabled = true;
		atomic64_inc(&phy->stats.eee_cleared_events);
	}

	if (phy->cfg.downshift_count) {
		int err = nd_phy_configure_downshift(phy);
		if (err)
			dev_warn(phy->dev, "downshift 配置失败: %d\n", err);
	}
	return 0;
}

/*
 * phylib 状态机回调。运行在 phylib 状态机线程中，不能阻塞。
 */
void nd_phy_link_adjust(struct net_device *ndev)
{
	struct nd_adapter *adapter = netdev_priv(ndev);
	struct nd_phy *phy = &adapter->phy;
	struct phy_device *phydev = phy->phydev;
	u8 state;

	if (!phydev)
		return;

	state = phydev->state;

	/* 记录状态变化事件 */
	if (phydev->link) {
		if (phy->last_phy_state != PHY_UP)
			atomic64_inc(&phy->stats.link_up_events);
	} else {
		if (phy->last_phy_state == PHY_UP)
			atomic64_inc(&phy->stats.link_down_events);
	}

	/* 降速检测：协商速率低于最大支持速率 */
	if (phydev->link && phydev->speed < phydev->supported_speeds_max) {
		atomic64_inc(&phy->stats.downshift_events);
		dev_warn(phy->dev,
			 "PHY 降速: %uMbps (max %uMbps)\n",
			 phydev->speed, phydev->supported_speeds_max);
	}

	phy->last_phy_state = (phydev->link ? PHY_UP : PHY_DOWN);
	phy->last_state_change = jiffies;
	phy->stats.last_speed = phydev->speed;
	phy->stats.last_duplex = phydev->duplex;

	phy_print_status(phydev);
}

/*
 * 看门狗：检测链路状态长时间不更新，执行恢复。
 */
void nd_phy_link_watchdog(struct work_struct *work)
{
	struct nd_phy *phy = container_of(to_delayed_work(work),
					  struct nd_phy, watchdog);
	struct phy_device *phydev = phy->phydev;

	if (!phydev || !netif_running(phy->phydev->attached_dev)) {
		schedule_delayed_work(&phy->watchdog, msecs_to_jiffies(2000));
		return;
	}

	/* 超过 30 秒无状态更新，尝试恢复 */
	if (time_after(jiffies, phy->last_state_change + 30 * HZ)) {
		dev_warn(phy->dev, "链路状态 30s 未更新，尝试恢复\n");
		nd_phy_link_recover(phy);
	}

	schedule_delayed_work(&phy->watchdog, msecs_to_jiffies(2000));
}

int nd_phy_link_recover(struct nd_phy *phy)
{
	struct phy_device *phydev = phy->phydev;
	int err;

	if (!phydev)
		return -ENODEV;

	/* 重新触发一次自协商，并读取状态 */
	err = phy_read(phydev, MII_BMSR);
	if (err < 0) {
		nd_phy_inc_mdio_error(phy);
		dev_err(phy->dev, "恢复时读取 BMSR 失败: %d\n", err);
		return err;
	}

	atomic64_inc(&phy->stats.an_restart_events);
	phy_start_aneg(phydev);
	return 0;
}
