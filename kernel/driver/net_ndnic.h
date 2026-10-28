//
// Created by User on 2026/10/28.
//

#ifndef NET_NDNIC_H
#define NET_NDNIC_H
// 驱动私有主头文件（net_device 私有结构体、宏、内联）
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
#include <net/xdp.h>              // xdp_buff, xdp_frame
#include <net/xdp_sock_drv.h>     // AF_XDP 零拷贝辅助（如果驱动原生支持）


#endif //NETDATA_INDUSTRIAL_NET_NDNIC_H
