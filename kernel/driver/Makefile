# SPDX-License-Identifier: GPL-2.0
#
# net_ndnic: Industrial NIC dataplane driver — Stage 0
#
# Build:
#   make              # build net_ndnic.ko
#   make KDIR=...     # build against a specific kernel tree
#   make clean
#

ifneq ($(KERNELRELEASE),)
# ============================================================
# Kbuild 环境：由内核顶层 Makefile 包含
# ============================================================

obj-m := net_ndnic.o

# 多文件模块：模块名不能与任何源文件同名
net_ndnic-objs := \
    net_ndnic_main.o \
    net_ndnic_pci.o \
    net_ndnic_irq.o \
    net_ndnic_napi.o \
    net_ndnic_ring.o \
    net_ndnic_xdp.o \
    net_ndnic_ethtool.o \
    net_ndnic_stats.o

# XDP 内核程序不进入 Kbuild；由 kernel/xdp/Makefile 独立编译
# 这里只声明依赖关系，避免误将 nd_xdp_kern.c 当作内核模块源文件

# 条件编译开关：通过 Kconfig 或命令行传入
# CONFIG_NET_NDNIC_AFXDP_ZC=y 时启用零拷贝辅助路径
ccflags-$(CONFIG_NET_NDNIC_AFXDP_ZC) += -DND_AFXDP_ZEROCOPY

# 确保驱动私有头文件可被所有源文件包含
ccflags-y += -I$(src)

else
# ============================================================
# 用户环境：直接执行 make
# ============================================================

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# 可选：交叉编译
# ARCH ?= arm64
# CROSS_COMPILE ?= aarch64-linux-gnu-

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a
verify:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	$(MAKE) -C $(PWD)/../../../scripts verify_kernel_module.sh \
	    KDIR=$(KDIR) || exit 1
.PHONY: default clean install verify
endif