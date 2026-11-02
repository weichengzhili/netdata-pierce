//
// Created by User on 2026/10/28.
// Copyright (c) 2026年 User. All rights reserved.
//  内核↔用户态共享 ABI（只含 POD、枚举、宏）
//

#ifndef ND_UAPI_H
#define ND_UAPI_H
/* 内核↔用户态共享：per-PHY 统计（POD，固定宽度） */
struct nd_uapi_phy_stats {
    __u64 link_up_events;
    __u64 link_down_events;
    __u64 an_restart_events;
    __u64 downshift_events;
    __u64 mdio_errors;
    __u64 eee_cleared_events;
    __u64 reg_dump_events;
    __u16 last_bmsr;
    __u16 last_bmcr;
    __u8  last_speed;
    __u8  last_duplex;
    __u8  eee_disabled;
    __u8  _pad[5];
} __attribute__((packed));

#endif //ND_UAPI_H
