# SPDX-License-Identifier: (GPL-2.0 )
#
# Build XDP kernel program and user-space loader.
# This Makefile is independent of Kbuild.
#

CLANG      ?= clang
ARCH       ?= $(shell uname -m | sed 's/x86_64/x86/;s/aarch64/arm64/')
OUTPUT     := _build
VMLINUX    ?= $(CURDIR)/vmlinux/$(ARCH)/vmlinux.h
LIBBPF_SRC ?= $(CURDIR)/../../third_party/libbpf/src
LIBBPF_OBJ := $(OUTPUT)/libbpf.a

# 使用项目自带的 libbpf 头文件，避免系统头文件版本不一致
INCLUDES := \
    -I$(OUTPUT) \
    -I$(LIBBPF_SRC)/../include/uapi \
    -I$(dir $(VMLINUX))

BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) \
              $(INCLUDES) -Wall

USER_CFLAGS := -g -O2 -Wall $(INCLUDES)

# XDP 内核程序目标
BPF_OBJ := $(OUTPUT)/nd_xdp_kern.o
USER_BIN := $(OUTPUT)/nd_xdp_loader

all: $(BPF_OBJ) $(USER_BIN)

$(OUTPUT):
	mkdir -p $(OUTPUT)

$(VMLINUX):
	@echo "ERROR: vmlinux.h not found at $(VMLINUX)" >&2
	@echo "Generate it with: bpftool btf dump file /sys/kernel/btf/vmlinux format c > $(VMLINUX)" >&2
	@exit 1

$(BPF_OBJ): nd_xdp_kern.c $(VMLINUX) | $(OUTPUT)
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

$(USER_BIN): nd_xdp_user.c | $(OUTPUT)
	$(CC) $(USER_CFLAGS) $< -o $@ $(LIBBPF_OBJ) -lelf -lz -lzstd

$(LIBBPF_OBJ): | $(OUTPUT)
	$(MAKE) -C $(LIBBPF_SRC) OUTPUT=$(abspath $(OUTPUT))/ \
	    OBJDIR=$(abspath $(OUTPUT))/build/ \
	    DESTDIR=$(abspath $(OUTPUT))/ \
	    install_headers
	cp $(OUTPUT)/libbpf.a $@

clean:
	rm -rf $(OUTPUT)

.PHONY: all clean