//
// Created by User on 2026/11/1.
//  # 寄存器 dump、统计、ethtool PHY 钩子
//
// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "net_ndnic_phy_debug: " fmt

#include <linux/phy.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>

#include "net_ndnic_phy.h"

void nd_phy_inc_mdio_error(struct nd_phy *phy)
{
    atomic64_inc(&phy->stats.mdio_errors);
    dev_warn_ratelimited(phy->dev, "MDIO 访问失败，累计 %lld 次\n",
                 (long long)atomic64_read(&phy->stats.mdio_errors));
}

static const struct {
    u16 reg;
    const char *name;
} nd_phy_key_regs[] = {
    { MII_BMCR,  "BMCR" },
    { MII_BMSR,  "BMSR" },
    { MII_PHYSID1, "PHYSID1" },
    { MII_PHYSID2, "PHYSID2" },
    { MII_ADVERTISE, "ADVERTISE" },
    { MII_LPA,   "LPA" },
    { MII_STAT1000, "STAT1000" },
};

/*
 * 链路异常或 probe 失败时调用，打印关键寄存器。
 * 不依赖用户手动触发，避免现场只能靠猜。
 */
void nd_phy_dump_regs(struct nd_phy *phy, const char *reason)
{
    struct phy_device *phydev = phy->phydev;
    int i;

    if (!phydev) {
        dev_err(phy->dev, "寄存器 dump 失败：PHY 未连接（%s）\n",
            reason);
        return;
    }

    dev_err(phy->dev, "PHY 寄存器 dump: %s\n", reason);
    for (i = 0; i < ARRAY_SIZE(nd_phy_key_regs); i++) {
        int val = phy_read(phydev, nd_phy_key_regs[i].reg);
        if (val < 0) {
            nd_phy_inc_mdio_error(phy);
            dev_err(phy->dev, "  %-10s = <读取失败 %d>\n",
                nd_phy_key_regs[i].name, val);
        } else {
            dev_err(phy->dev, "  %-10s = 0x%04x\n",
                nd_phy_key_regs[i].name, val);
        }
    }
    atomic64_inc(&phy->stats.reg_dump_events);
}

void nd_phy_get_stats(struct nd_phy *phy, struct nd_phy_stats *out)
{
    memcpy(out, &phy->stats, sizeof(*out));
}