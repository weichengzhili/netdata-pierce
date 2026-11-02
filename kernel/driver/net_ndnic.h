//
// Created by User on 2026/10/28.
//

#ifndef _NET_NDNIC_H
#define _NET_NDNIC_H

/* ===== 三方（内核）依赖 ===== */
#include <linux/version.h>        // LINUX_VERSION_CODE, KERNEL_VERSION
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/pci.h>            // pci_dev, pci_driver, BAR, DMA mask
#include <linux/netdevice.h>      // net_device, netdev_ops, napi_struct
#include <linux/etherdevice.h>    // eth_hw_addr_random, is_zero_ether_addr
#include <linux/interrupt.h>      // request_irq, MSI-X, IRQ affinity
#include <linux/ethtool.h>        // ethtool_ops, ethtool_ringparam
#include <linux/if_link.h>        // XDP attach flags
#include <linux/bpf.h>            // bpf_prog, XDP 返回码
#include <linux/filter.h>         // bpf_prog 辅助接口
#include <linux/vmalloc.h>        // 大块 Ring 分配
#include <linux/dma-mapping.h>    // dma_alloc_coherent, dma_set_mask
#include <linux/workqueue.h>      // struct work_struct (reset_task)
#include <linux/timer.h>          // struct timer_list (service_timer)
#include <net/xdp.h>              // xdp_buff, xdp_frame
#include <net/xdp_sock_drv.h>     // AF_XDP 零拷贝辅助（如驱动原生支持）

#include "net_ndnic_compat.h"
#include "net_ndnic_phy.h"

#define ND_DRV_NAME        "net_ndnic"
#define ND_DRV_VERSION     "0.1.0"

#define ND_MAX_Q_VECTORS   64
#define ND_DEFAULT_RX_RING 1024
#define ND_DEFAULT_TX_RING 1024
#define ND_NAPI_WEIGHT     64
#define ND_TX_CLEAN_BUDGET 256
#define ND_MIN_MTU         68
#define ND_MAX_MTU         9000

struct nd_adapter;
struct nd_ring;

/* 环形缓冲区描述符（硬件相关，示例用简单结构） */
struct nd_rx_desc {
	__le64	buf_addr;
	__le32	status;
	__le16	len;
	__le16	vlan;
} __attribute__((packed));

struct nd_tx_desc {
	__le64	buf_addr;
	__le32	status;
	__le16	len;
	__le16	pad;
} __attribute__((packed));

struct nd_ring {
	struct nd_adapter	*adapter;
	void			*desc;		/* 描述符环（DMA 一致性） */
	dma_addr_t		desc_dma;
	u32			count;
	u32			next_to_use;
	u32			next_to_clean;
	void			**bufs;		/* 数据缓冲区指针数组 */
	dma_addr_t		*buf_dma;	/* 数据缓冲区 DMA 地址 */
	struct sk_buff		**skbs;		/* 已填充的 SKB（RX） */
};

struct nd_q_vector {
	struct nd_adapter	*adapter;
	struct napi_struct	napi;
	int			v_idx;
	char			name[IFNAMSIZ + 8];
	cpumask_t		affinity_mask;
	struct nd_ring		rx;
	struct nd_ring		tx;
	u64			packets;
	u64			bytes;
};

struct nd_stats {
	u64	rx_packets;
	u64	rx_bytes;
	u64	tx_packets;
	u64	tx_bytes;
	u64	rx_dropped;
	u64	tx_dropped;
	u64	rx_errors;
	u64	tx_errors;
	u64	rx_crc_errors;
	u64	rx_length_errors;
	u64	rx_fifo_errors;
	u64	tx_fifo_errors;
	u64	link_down_events;
	u64	resets;
};

struct nd_adapter {
	struct pci_dev		*pdev;
	struct net_device	*netdev;
	void __iomem		*hw_addr;
	void __iomem		*io_addr;

	u32			num_q_vectors;
	struct nd_q_vector	*q_vector;

	u32			rx_ring_count;
	u32			tx_ring_count;
	u8			hw_rev;
	u32			msg_enable;

	struct nd_phy		phy;
	struct nd_stats	stats;

	struct bpf_prog		*xdp_prog;

	struct work_struct	reset_task;
	struct timer_list	service_timer;
	unsigned long		state;
};

/* pci.c */
int  nd_pci_check_hw(struct nd_adapter *adapter);
int  nd_pci_reset(struct nd_adapter *adapter);
int  nd_pci_init_adapter(struct nd_adapter *adapter);
void nd_pci_release_adapter(struct nd_adapter *adapter);
int  nd_read_mac(struct nd_adapter *adapter, u8 *mac);

/* irq.c */
int  nd_irq_alloc(struct nd_adapter *adapter);
void nd_irq_free(struct nd_adapter *adapter);
void nd_irq_enable(struct nd_adapter *adapter);
void nd_irq_disable(struct nd_adapter *adapter);
void nd_irq_schedule_all(struct nd_adapter *adapter);

/* napi.c */
int  nd_napi_add_all(struct nd_adapter *adapter);
void nd_napi_del_all(struct nd_adapter *adapter);
int  nd_poll(struct napi_struct *napi, int budget);

/* ring.c */
int  nd_ring_alloc_all(struct nd_adapter *adapter);
void nd_ring_free_all(struct nd_adapter *adapter);
void nd_ring_enable_all(struct nd_adapter *adapter);
void nd_ring_disable_all(struct nd_adapter *adapter);
netdev_tx_t nd_xmit(struct sk_buff *skb, struct net_device *ndev);

/* xdp.c */
int  nd_xdp_init(struct nd_adapter *adapter);
void nd_xdp_deinit(struct nd_adapter *adapter);
int  nd_xdp(struct net_device *ndev, struct netdev_bpf *bpf);
int  nd_xdp_xmit(struct net_device *ndev, int num, struct xdp_frame **frames,
		 u32 flags);

/* ethtool.c */
extern const struct ethtool_ops nd_ethtool_ops;

/* stats.c */
void nd_update_stats(struct nd_adapter *adapter);
void nd_aggregate_stats(struct nd_adapter *adapter);

#endif /* _NET_NDNIC_H */
