//
// Created by User on 2026/11/1.
//  # PHY 私有配置、时序参数、状态结构
//

#ifndef _NET_NDNIC_PHY_H
#define _NET_NDNIC_PHY_H

#include <linux/phy.h>
#include <linux/gpio/consumer.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
/* 板级 PHY 接口模式 */
struct _nd_phy_mode
{
      ND_PHY_MODE_UNKNOWN = 0,
      ND_PHY_MODE_RGMII,
      ND_PHY_MODE_RGMII_ID,
      ND_PHY_MODE_RGMII_TXID,
      ND_PHY_MODE_RGMII_RXID,
      ND_PHY_MODE_SGMII,
};
/* 复位时序 */
struct nd_phy_reset_seq {
      u32 assert_us;
      u32 deassert_us;
};
/* 板级覆盖配置（DTS 解析结果） */
struct nd_phy_board_cfg {
      bool		disable_eee;
      u32		downshift_count;	/* 0 = 不启用 */
      u32		downshift_reg;		/* 0 = 使用 PHY 默认 */
      u32		downshift_val;
      enum nd_phy_mode mode;
      struct nd_phy_reset_seq reset;
};

/* per-PHY 统计（控制面聚合） */
struct nd_phy_stats {
      atomic64_t	link_up_events;
      atomic64_t	link_down_events;
      atomic64_t	an_restart_events;
      atomic64_t	downshift_events;
      atomic64_t	mdio_errors;
      atomic64_t	eee_cleared_events;
      atomic64_t	reg_dump_events;
      u16		last_bmsr;
      u16		last_bmcr;
      u8		last_speed;
      u8		last_duplex;
};

/* PHY 私有状态 */
struct nd_phy {
      struct phy_device	*phydev;
      struct device		*dev;
      struct gpio_desc	*reset_gpio;
      struct nd_phy_board_cfg	cfg;
      struct nd_phy_stats	stats;
      bool			eee_disabled;
      bool			timing_validated;
      struct delayed_work	watchdog;
      unsigned long		last_state_change;
      u8			last_phy_state;
};

struct nd_adapter;

/* 生命周期 */
int  nd_phy_init(struct nd_adapter *adapter, struct device_node *np);
void nd_phy_deinit(struct nd_adapter *adapter);
int  nd_phy_start(struct nd_adapter *adapter);
void nd_phy_stop(struct nd_adapter *adapter);

/* timing 子模块 */
int nd_phy_parse_timing(struct nd_phy *phy, struct device_node *np);
int nd_phy_validate_timing(struct nd_phy *phy, phy_interface_t interface);
int nd_phy_reset_sequence(struct nd_phy *phy);
int nd_phy_apply_rgmii_delay(struct nd_phy *phy);

/* link 子模块 */
int  nd_phy_link_configure(struct nd_phy *phy);
void nd_phy_link_adjust(struct net_device *ndev);
void nd_phy_link_watchdog(struct work_struct *work);
int  nd_phy_link_recover(struct nd_phy *phy);

/* debug 子模块 */
void nd_phy_dump_regs(struct nd_phy *phy, const char *reason);
void nd_phy_inc_mdio_error(struct nd_phy *phy);
void nd_phy_get_stats(struct nd_phy *phy, struct nd_phy_stats *out);
#endif //NET_NDNIC_PHY_H
