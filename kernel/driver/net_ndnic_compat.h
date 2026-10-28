//
// Created by User on 2026/10/28.
// ★ 唯一版本兼容入口
//
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_NDNIC_COMPAT_H
#define _NET_NDNIC_COMPAT_H

#include <linux/version.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/bpf.h>
#include <net/xdp.h>

/*
 * ---------------------------------------------------------------------------
 * NAPI
 * ---------------------------------------------------------------------------
 * 5.18 起 netif_napi_add 签名稳定，6.8 起权重通过 netif_napi_add_weight 显式传入。
 * 我们统一封装为 nd_napi_add()，业务层不感知版本差异。
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
static inline void nd_napi_add(struct net_device *ndev,
                               struct napi_struct *napi,
                               int (*poll)(struct napi_struct *, int),
                               int weight)
{
    netif_napi_add_weight(ndev, napi, poll, weight);
}
#else
static inline void nd_napi_add(struct net_device *ndev,
                               struct napi_struct *napi,
                               int (*poll)(struct napi_struct *, int),
                               int weight)
{
    netif_napi_add(ndev, napi, poll, weight);
}
#endif

/*
 * ---------------------------------------------------------------------------
 * PCI IRQ 向量分配
 * ---------------------------------------------------------------------------
 * pci_alloc_irq_vectors_affinity() 在旧内核上不支持某些 flags。
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#define ND_PCI_IRQ_FLAGS (PCI_IRQ_MSIX | PCI_IRQ_AFFINITY)
#else
#define ND_PCI_IRQ_FLAGS (PCI_IRQ_MSIX)
#endif

static inline int nd_pci_alloc_irq_vectors(struct pci_dev *pdev,
                                           unsigned int min_vecs,
                                           unsigned int max_vecs)
{
    return pci_alloc_irq_vectors(pdev, min_vecs, max_vecs, ND_PCI_IRQ_FLAGS);
}

/*
 * ---------------------------------------------------------------------------
 * XDP attach/detach
 * ---------------------------------------------------------------------------
 * bpf_xdp_attach / bpf_xdp_detach 在 libbpf v0.7.0 起可用；内核侧
 * bpf_prog_attach 的 XDP 路径签名在 5.9 附近稳定。这里封装为驱动内接口。
 */
#if defined(CONFIG_XDP_SOCKETS) || IS_ENABLED(CONFIG_XDP_SOCKETS)
static inline int nd_xdp_attach(struct net_device *ndev, struct bpf_prog *prog)
{
    return dev_change_xdp_fd(ndev, NULL, prog ? prog->aux->id : -1,
                             XDP_FLAGS_DRV_MODE);
}
#else
static inline int nd_xdp_attach(struct net_device *ndev, struct bpf_prog *prog)
{
    return -EOPNOTSUPP;
}
#endif

/*
 * ---------------------------------------------------------------------------
 * AF_XDP 零拷贝
 * ---------------------------------------------------------------------------
 * xsk_buff_pool 相关接口在 5.4 引入，5.10 后稳定。旧内核直接返回不支持，
 * 让 probe 走通用 XDP 路径。
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) && \
    IS_ENABLED(CONFIG_XDP_SOCKETS)
static inline bool nd_afxdp_zc_supported(struct net_device *ndev)
{
    return ndev->netdev_ops->ndo_bpf && ndev->xdp_features & NETDEV_XDP_ACT_XSK_ZEROCOPY;
}
#else
static inline bool nd_afxdp_zc_supported(struct net_device *ndev)
{
    return false;
}
#endif

#endif /* _NET_NDNIC_COMPAT_H */