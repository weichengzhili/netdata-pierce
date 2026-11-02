//
// Created by User on 2026/10/28.
// 模块入口/出口，pci_driver 注册，probe/remove 编排
//
// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "net_ndnic: " fmt

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/ethtool.h>
#include <linux/of.h>
#include <linux/aer.h>
#include <linux/if_link.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>

#include "net_ndnic.h"
#include "net_ndnic_compat.h"
#include "net_ndnic_phy.h"

#define ND_VENDOR_ID	0x1d0f		/* 示例厂商 ID，量产前替换 */
#define ND_DEVICE_ID	0x0001

static const struct pci_device_id nd_pci_tbl[] = {
	{ PCI_VDEVICE(ND_VENDOR, ND_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, nd_pci_tbl);

/* =========================================================================
 * ndo_change_mtu
 * ========================================================================= */
static int nd_change_mtu(struct net_device *ndev, int new_mtu)
{
	if (new_mtu < ND_MIN_MTU || new_mtu > ND_MAX_MTU)
		return -EINVAL;

	/* MTU 改变会导致缓冲区大小变化，运行中必须拒绝；
	 * 更安全的做法是 down/up 后才能改，避免正在使用的 DMA 缓冲区被释放。
	 */
	if (netif_running(ndev))
		return -EBUSY;

	ndev->mtu = new_mtu;
	return 0;
}

/* =========================================================================
 * ndo_set_rx_mode（多播/混杂模式）
 * ========================================================================= */
static void nd_set_rx_mode(struct net_device *ndev)
{
	struct nd_adapter *adapter = netdev_priv(ndev);
	bool promisc = !!(ndev->flags & IFF_PROMISC);
	bool allmulti = !!(ndev->flags & IFF_ALLMULTI);

	/* 实际写硬件寄存器的逻辑在 pci.c 中；
	 * 这里只记录状态，避免在多播列表更新时反复写 MMIO。
	 */
	dev_dbg(&adapter->pdev->dev,
		"rx_mode: promisc=%d allmulti=%d mcast_count=%d\n",
		promisc, allmulti, netdev_mc_count(ndev));
}

/* =========================================================================
 * ndo_tx_timeout（发送队列卡死时的恢复钩子）
 * ========================================================================= */
static void nd_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	struct nd_adapter *adapter = netdev_priv(ndev);

	netdev_warn(ndev, "TX 超时（queue=%u），触发复位\n", txqueue);
	adapter->stats.resets++;

	/* 简化处理：调度 reset_task 异步复位，避免在中断上下文中做重活 */
	schedule_work(&adapter->reset_task);
}

/* =========================================================================
 * ndo_get_stats64（64 位统计，避免 32 位回绕）
 * ========================================================================= */
static void nd_get_stats64(struct net_device *ndev,
			   struct rtnl_link_stats64 *stats)
{
	struct nd_adapter *adapter = netdev_priv(ndev);
	struct nd_stats *s = &adapter->stats;

	stats->rx_packets = s->rx_packets;
	stats->tx_packets = s->tx_packets;
	stats->rx_bytes   = s->rx_bytes;
	stats->tx_bytes   = s->tx_bytes;
	stats->rx_dropped = s->rx_dropped;
	stats->tx_dropped = s->tx_dropped;
	stats->rx_errors  = s->rx_errors;
	stats->tx_errors  = s->tx_errors;

	stats->rx_length_errors = s->rx_length_errors;
	stats->rx_crc_errors    = s->rx_crc_errors;
	stats->rx_fifo_errors   = s->rx_fifo_errors;
	stats->tx_fifo_errors   = s->tx_fifo_errors;
}

/* =========================================================================
 * netdev_ops
 * ========================================================================= */
static const struct net_device_ops nd_netdev_ops = {
	.ndo_open            = net_ndnic_open,
	.ndo_stop            = net_ndnic_close,
	.ndo_start_xmit      = nd_xmit,
	.ndo_set_rx_mode     = nd_set_rx_mode,
	.ndo_change_mtu      = nd_change_mtu,
	.ndo_tx_timeout      = nd_tx_timeout,
	.ndo_get_stats64     = nd_get_stats64,
	.ndo_bpf             = nd_xdp,
	.ndo_xdp_xmit        = nd_xdp_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr   = eth_validate_addr,
};

/* =========================================================================
 * ndo_open：分配 ring、申请中断、启动 PHY、使能硬件
 *
 * 工业级约束：
 *   - 顺序：ring → irq → PHY → 硬件 enable
 *   - 每一阶段失败都回滚前面已完成的资源
 *   - ring/irq 延迟到 open 而不是 probe，避免设备未被使用就占满 DMA 内存
 * ========================================================================= */
static int net_ndnic_open(struct net_device *ndev)
{
	struct nd_adapter *adapter = netdev_priv(ndev);
	int err;

	/* 1. 分配 RX/TX 描述符环和数据缓冲区 */
	err = nd_ring_alloc_all(adapter);
	if (err) {
		netdev_err(ndev, "ring 分配失败: %d\n", err);
		return err;
	}

	/* 2. 申请 MSI-X 中断并绑定 NAPI */
	err = nd_irq_alloc(adapter);
	if (err) {
		netdev_err(ndev, "中断申请失败: %d\n", err);
		goto err_ring;
	}

	/* 3. 启动 PHY（EEE 禁用、downshift 已在 probe 配置） */
	err = nd_phy_start(adapter);
	if (err) {
		netdev_err(ndev, "PHY start 失败: %d\n", err);
		goto err_irq;
	}

	/* 4. 使能硬件收发并开中断 */
	nd_ring_enable_all(adapter);
	nd_irq_enable(adapter);

	netif_carrier_off(ndev);
	netif_tx_start_all_queues(ndev);

	netdev_info(ndev, "设备已启动\n");
	return 0;

err_irq:
	nd_irq_free(adapter);
err_ring:
	nd_ring_free_all(adapter);
	return err;
}

/* =========================================================================
 * ndo_stop：与 ndo_open 严格对称
 * ========================================================================= */
static int net_ndnic_close(struct net_device *ndev)
{
	struct nd_adapter *adapter = netdev_priv(ndev);

	netif_tx_stop_all_queues(ndev);

	/* 停止顺序与 open 相反：硬件 → 中断 → PHY → ring */
	nd_ring_disable_all(adapter);
	nd_irq_disable(adapter);
	nd_phy_stop(adapter);
	nd_irq_free(adapter);
	nd_ring_free_all(adapter);

	netif_carrier_off(ndev);
	netdev_info(ndev, "设备已停止\n");
	return 0;
}

/* =========================================================================
 * reset_task：TX 超时后的异步复位
 * ========================================================================= */
static void nd_reset_task(struct work_struct *work)
{
	struct nd_adapter *adapter = container_of(work, struct nd_adapter,
						  reset_task);
	struct net_device *ndev = adapter->netdev;

	if (!netif_running(ndev))
		return;

	netdev_warn(ndev, "执行硬件复位\n");
	rtnl_lock();
	net_ndnic_close(ndev);
	nd_pci_reset(adapter);
	net_ndnic_open(ndev);
	rtnl_unlock();
}

/* =========================================================================
 * probe：严格的资源申请顺序 + 分阶段 goto 清理
 * ========================================================================= */
static int net_ndnic_probe(struct pci_dev *pdev,
			   const struct pci_device_id *id)
{
	struct net_device *ndev;
	struct nd_adapter *adapter;
	u8 mac[ETH_ALEN];
	int err;

	/* --- 1. 使能 PCI 设备（MEM 空间） --- */
	err = pci_enable_device_mem(pdev);
	if (err) {
		dev_err(&pdev->dev, "pci_enable_device_mem 失败: %d\n", err);
		return err;
	}

	/* --- 2. 设置 DMA mask，失败必须回退 --- */
	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		dev_warn(&pdev->dev, "64 位 DMA mask 失败，回退 32 位: %d\n", err);
		err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(&pdev->dev, "32 位 DMA mask 也失败: %d\n", err);
			goto err_pci_disable;
		}
	}

	/* --- 3. 总线主控（DMA 必需） --- */
	pci_set_master(pdev);

	/* --- 4. 请求 BAR 区域 --- */
	err = pci_request_mem_regions(pdev, ND_DRV_NAME);
	if (err) {
		dev_err(&pdev->dev, "BAR 区域申请失败: %d\n", err);
		goto err_pci_clear_master;
	}

	/* --- 5. 分配 net_device（附带 priv 空间 + 多队列） --- */
	ndev = alloc_etherdev_mqs(sizeof(struct nd_adapter),
				  ND_MAX_Q_VECTORS, ND_MAX_Q_VECTORS);
	if (!ndev) {
		err = -ENOMEM;
		goto err_release_regions;
	}

	SET_NETDEV_DEV(ndev, &pdev->dev);
	pci_set_drvdata(pdev, ndev);
	adapter = netdev_priv(ndev);
	adapter->pdev = pdev;
	adapter->netdev = ndev;
	adapter->msg_enable = NETIF_MSG_DRV | NETIF_MSG_PROBE;

	/* --- 6. 映射 BAR0（pcim_iomap 自动管理，remove 时无需手动 unmap） --- */
	adapter->hw_addr = pcim_iomap(pdev, 0, 0);
	if (!adapter->hw_addr) {
		dev_err(&pdev->dev, "BAR0 映射失败\n");
		err = -EIO;
		goto err_free_netdev;
	}

	/* --- 7. 硬件检查：复位、版本、能力 --- */
	err = nd_pci_check_hw(adapter);
	if (err) {
		dev_err(&pdev->dev, "硬件检查失败: %d\n", err);
		goto err_free_netdev;
	}

	/* --- 8. 分配 q_vector 和控制结构（不分配 DMA） --- */
	adapter->num_q_vectors = min_t(u32, ND_MAX_Q_VECTORS,
				       num_online_cpus());
	err = nd_pci_init_adapter(adapter);
	if (err) {
		dev_err(&pdev->dev, "adapter 初始化失败: %d\n", err);
		goto err_free_netdev;
	}

	/* --- 9. netdev 静态配置 --- */
	ndev->netdev_ops    = &nd_netdev_ops;
	ndev->ethtool_ops   = &nd_ethtool_ops;
	ndev->watchdog_timeo = 5 * HZ;
	ndev->min_mtu = ND_MIN_MTU;
	ndev->max_mtu = ND_MAX_MTU;
	ndev->features |= NETIF_F_RXCSUM | NETIF_F_HW_CSUM |
			  NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_CTAG_TX |
			  NETIF_F_SG | NETIF_F_TSO;
	ndev->hw_features = ndev->features;
	ndev->vlan_features = ndev->features;

	/* --- 10. MAC 地址：硬件有效则用硬件，否则随机 --- */
	if (nd_read_mac(adapter, mac) == 0 && is_valid_ether_addr(mac)) {
		nd_eth_hw_addr_set(ndev, mac);
	} else {
		dev_warn(&pdev->dev, "硬件 MAC 无效，使用随机地址\n");
		eth_hw_addr_random(ndev);
	}

	/* --- 11. PHY 初始化（关键步骤，失败必须回滚） ---
	 * pdev->dev.of_node 指向 MAC 节点（DTS 中的 ethernet 节点）；
	 * 无 DTS（纯 x86 平台）时返回 NULL，此时跳过外部 PHY 管理。
	 */
	err = nd_phy_init(adapter, pdev->dev.of_node);
	if (err) {
		dev_err(&pdev->dev, "PHY 初始化失败: %d\n", err);
		goto err_adapter_release;
	}

	/* --- 12. XDP 初始化 --- */
	err = nd_xdp_init(adapter);
	if (err) {
		dev_err(&pdev->dev, "XDP 初始化失败: %d\n", err);
		goto err_phy;
	}

	/* --- 13. 注册 netdev，此时设备对外可见 --- */
	err = register_netdev(ndev);
	if (err) {
		dev_err(&pdev->dev, "register_netdev 失败: %d\n", err);
		goto err_xdp;
	}

	/* --- 14. reset_task 初始化，AER 开启 --- */
	INIT_WORK(&adapter->reset_task, nd_reset_task);
	pci_enable_pcie_error_reporting(pdev);

	netdev_info(ndev, "probe 完成: %s, rev 0x%02x, %u 队列\n",
		    ndev->name, adapter->hw_rev, adapter->num_q_vectors);
	return 0;

	/* ---------- 分阶段错误回滚（倒序） ---------- */
err_xdp:
	nd_xdp_deinit(adapter);
err_phy:
	nd_phy_deinit(adapter);
err_adapter_release:
	nd_pci_release_adapter(adapter);
err_free_netdev:
	free_netdev(ndev);
	pci_set_drvdata(pdev, NULL);
err_release_regions:
	pci_release_mem_regions(pdev);
err_pci_clear_master:
	pci_clear_master(pdev);
err_pci_disable:
	pci_disable_device(pdev);
	return err;
}

/* =========================================================================
 * remove：与 probe 严格对称
 * ========================================================================= */
static void net_ndnic_remove(struct pci_dev *pdev)
{
	struct net_device *ndev = pci_get_drvdata(pdev);
	struct nd_adapter *adapter;

	if (!ndev)
		return;

	adapter = netdev_priv(ndev);

	/* 0. 关闭 AER 上报 */
	pci_disable_pcie_error_reporting(pdev);

	/* 1. 取消可能挂起的 reset_task，避免 remove 与 reset 竞争 */
	cancel_work_sync(&adapter->reset_task);

	/* 2. 注销 netdev（自动调用 ndo_stop，对称于 register_netdev） */
	unregister_netdev(ndev);

	/* 3. 释放 XDP */
	nd_xdp_deinit(adapter);

	/* 4. 释放 PHY */
	nd_phy_deinit(adapter);

	/* 5. 释放 q_vector 等控制结构 */
	nd_pci_release_adapter(adapter);

	/* 6. 取消 BAR 映射（pcim_iomap 自动管理，无需手动 iounmap） */

	/* 7. 释放 net_device */
	free_netdev(ndev);
	pci_set_drvdata(pdev, NULL);

	/* 8. 释放 BAR 区域 */
	pci_release_mem_regions(pdev);

	/* 9. 清理总线主控 */
	pci_clear_master(pdev);

	/* 10. 禁用 PCI 设备 */
	pci_disable_device(pdev);
}

/* =========================================================================
 * AER 错误处理（工业设备必须实现）
 *
 * 故障域：PCIe 链路错误、DMA 错误、热插拔
 * 恢复策略：
 *   - io_normal: 只记录，可恢复
 *   - io_frozen: 断开链路，通知内核需要 slot reset
 *   - io_perm_failure: 无法恢复，断开
 * ========================================================================= */
static pci_ers_result_t nd_pci_error_detected(struct pci_dev *pdev,
					      pci_channel_state_t state)
{
	struct net_device *ndev = pci_get_drvdata(pdev);
	struct nd_adapter *adapter;

	if (!ndev)
		return PCI_ERS_RESULT_DISCONNECT;

	adapter = netdev_priv(ndev);

	switch (state) {
	case pci_channel_io_normal:
		return PCI_ERS_RESULT_CAN_RECOVER;
	case pci_channel_io_frozen:
		netif_device_detach(ndev);
		nd_irq_disable(adapter);
		return PCI_ERS_RESULT_NEED_RESET;
	case pci_channel_io_perm_failure:
		return PCI_ERS_RESULT_DISCONNECT;
	default:
		return PCI_ERS_RESULT_NEED_RESET;
	}
}

static pci_ers_result_t nd_pci_error_slot_reset(struct pci_dev *pdev)
{
	struct net_device *ndev = pci_get_drvdata(pdev);
	struct nd_adapter *adapter;
	int err;

	if (!ndev)
		return PCI_ERS_RESULT_DISCONNECT;

	adapter = netdev_priv(ndev);

	if (pci_enable_device_mem(pdev)) {
		dev_err(&pdev->dev, "slot reset 后使能失败\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}

	pci_set_master(pdev);

	err = nd_pci_reset(adapter);
	if (err) {
		dev_err(&pdev->dev, "slot reset 后硬件复位失败: %d\n", err);
		return PCI_ERS_RESULT_DISCONNECT;
	}

	return PCI_ERS_RESULT_RECOVERED;
}

static void nd_pci_error_resume(struct pci_dev *pdev)
{
	struct net_device *ndev = pci_get_drvdata(pdev);
	struct nd_adapter *adapter;

	if (!ndev)
		return;

	adapter = netdev_priv(ndev);
	nd_irq_enable(adapter);
	netif_device_attach(ndev);
}

static const struct pci_error_handlers nd_err_handler = {
	.error_detected = nd_pci_error_detected,
	.slot_reset     = nd_pci_error_slot_reset,
	.resume         = nd_pci_error_resume,
};

/* =========================================================================
 * pci_driver
 * ========================================================================= */
static struct pci_driver nd_driver = {
	.name        = ND_DRV_NAME,
	.id_table    = nd_pci_tbl,
	.probe       = net_ndnic_probe,
	.remove      = net_ndnic_remove,
	.err_handler = &nd_err_handler,
};

module_pci_driver(nd_driver);

MODULE_AUTHOR("net_ndnic maintainers");
MODULE_DESCRIPTION("Industrial NIC dataplane driver (Stage 0)");
MODULE_LICENSE("GPL");
MODULE_VERSION(ND_DRV_VERSION);
