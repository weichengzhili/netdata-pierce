#!/usr/bin/env bash
set -euo pipefail

XDP_DIR="$(cd "$(dirname "$0")/../kernel/xdp" && pwd)"
FAIL=0

pass() { printf '  [PASS] %s\n' "$*"; }
fail() { printf '  [FAIL] %s\n' "$*"; FAIL=$((FAIL+1)); }

cd "$XDP_DIR"

# 1. vmlinux.h 存在性
VMLINUX="${VMLINUX:-$XDP_DIR/vmlinux/$(uname -m)/vmlinux.h}"
if [[ -f "$VMLINUX" ]]; then
    pass "vmlinux.h 存在：$VMLINUX"
else
    fail "vmlinux.h 缺失，请用 bpftool btf dump 生成"
fi

# 2. libbpf 版本锁定
if pkg-config --exists libbpf; then
    LIBBPF_VER=$(pkg-config --modversion libbpf)
    pass "系统 libbpf 版本：$LIBBPF_VER"
else
    info "系统未安装 libbpf，将使用 vendored 版本"
fi

# 3. 构建 BPF 目标
make clean >/dev/null
if make 2>build.log; then
    pass "XDP 构建成功"
else
    fail "XDP 构建失败"
    tail -30 build.log
fi

# 4. BPF 对象合法性
if [[ -f _build/nd_xdp_kern.o ]]; then
    pass "nd_xdp_kern.o 存在"
    if llvm-objdump -h _build/nd_xdp_kern.o 2>/dev/null | grep -q 'xdp'; then
        pass "BPF 对象含 XDP 节"
    else
        fail "BPF 对象无 XDP 节，可能未正确编译"
    fi
    if llvm-readelf -S _build/nd_xdp_kern.o 2>/dev/null | grep -q '.BTF'; then
        pass "BTF 节存在"
    else
        fail "BTF 节缺失，libbpf 无法加载"
    fi
else
    fail "nd_xdp_kern.o 未生成"
fi

# 5. 加载器链接完整性
if [[ -x _build/nd_xdp_loader ]]; then
    pass "nd_xdp_loader 可执行"
    if ldd _build/nd_xdp_loader 2>/dev/null | grep -q 'not found'; then
        fail "加载器存在未解析的动态库"
        ldd _build/nd_xdp_loader | grep 'not found' | sed 's/^/         /'
    else
        pass "加载器动态库依赖完整"
    fi
else
    fail "nd_xdp_loader 未生成或不可执行"
fi

echo
[[ $FAIL -eq 0 ]] && { echo "==== XDP 验证通过 ===="; exit 0; } \
                   || { echo "==== 存在 $FAIL 项失败 ===="; exit 1; }