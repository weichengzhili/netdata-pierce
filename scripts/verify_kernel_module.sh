#!/usr/bin/env bash
# 内核模块构建与符号验证
set -euo pipefail

KDIR="${KDIR:-/lib/modules/$(uname -r)/build}"
DRIVER_DIR="$(cd "$(dirname "$0")/../kernel/driver" && pwd)"
FAIL=0

pass() { printf '  [PASS] %s\n' "$*"; }
fail() { printf '  [FAIL] %s\n' "$*"; FAIL=$((FAIL+1)); }
info() { printf '[INFO] %s\n' "$*"; }

# ---- 0. 前置检查 ----
info "KDIR = $KDIR"
if [[ ! -d "$KDIR" ]]; then
    fail "内核头文件目录不存在：$KDIR（请安装 linux-headers-$(uname -r)）"
    exit 1
fi

KVER_BUILD=$(make -s -C "$KDIR" kernelversion 2>/dev/null || echo "unknown")
KVER_RUN=$(uname -r)
info "构建内核版本：$KVER_BUILD"
info "运行内核版本：$KVER_RUN"
if [[ "$KVER_BUILD" != "$KVER_RUN" ]]; then
    fail "构建内核版本与运行内核版本不一致，module_layout 会失败"
fi

# ---- 1. 干净构建 ----
info "构建模块..."
cd "$DRIVER_DIR"
make KDIR="$KDIR" clean >/dev/null
if make KDIR="$KDIR" -j"$(nproc)" 2>build.log; then
    pass "模块构建成功"
else
    fail "模块构建失败，见 $(pwd)/build.log"
    tail -40 build.log
    exit 1
fi

# ---- 2. 产物存在性 ----
if [[ -f net_ndnic.ko ]]; then
    pass "net_ndnic.ko 存在"
else
    fail "net_ndnic.ko 未生成"
    exit 1
fi

# ---- 3. 模块元数据 ----
info "检查 modinfo..."
if modinfo net_ndnic.ko >/dev/null 2>&1; then
    pass "modinfo 可读"
    if modinfo net_ndnic.ko | grep -q '^license:.*GPL'; then
        pass "license 为 GPL（符号解析前提）"
    else
        fail "license 非 GPL，EXPORT_SYMBOL_GPL 符号将无法解析"
    fi
else
    fail "modinfo 读取失败"
fi

# ---- 4. 未定义符号检查 ----
info "检查未定义符号..."
UNDEF=$(nm -u net_ndnic.ko 2>/dev/null | awk '{print $2}' | sort -u)
if [[ -z "$UNDEF" ]]; then
    pass "无未定义符号"
else
    # 过滤内核导出的合法符号
    UNEXPECTED=""
    while IFS= read -r sym; do
        [[ -z "$sym" ]] && continue
        if ! grep -rq "EXPORT_SYMBOL.*\b$sym\b" "$KDIR" 2>/dev/null; then
            UNEXPECTED+="$sym"$'\n'
        fi
    done <<< "$UNDEF"
    if [[ -z "$UNEXPECTED" ]]; then
        pass "所有未定义符号均由内核导出"
    else
        fail "存在非内核导出的未定义符号："
        printf '%s\n' "$UNEXPECTED" | sed 's/^/         /'
    fi
fi

# ---- 5. 多文件 objs 完整性 ----
info "检查 -objs 列表完整性..."
EXPECTED_OBJS=$(grep -A20 'net_ndnic-objs' Makefile | grep -oE 'net_ndnic_[a-z]+\.o' | sort)
ACTUAL_OBJS=$(nm net_ndnic.ko 2>/dev/null | grep -oE 'nd_(pci|irq|napi|ring|xdp|ethtool|stats|main)_' | sed 's/_$//' | sort -u)
MISSING=""
for obj in $EXPECTED_OBJS; do
    base="${obj%.o}"
    if ! grep -q "${base}" <(nm net_ndnic.ko 2>/dev/null); then
        MISSING+="$obj"$'\n'
    fi
done
if [[ -z "$MISSING" ]]; then
    pass "所有 -objs 目标均已链接"
else
    fail "以下 -objs 目标未在 .ko 中出现（可能漏编译）："
    printf '%s\n' "$MISSING" | sed 's/^/         /'
fi

# ---- 6. 版本兼容头文件约束 ----
info "检查业务文件是否绕过 compat.h..."
VIOLATIONS=$(grep -l 'LINUX_VERSION_CODE' "$DRIVER_DIR"/net_ndnic_{main,pci,irq,napi,ring,xdp,ethtool,stats}.c 2>/dev/null || true)
if [[ -z "$VIOLATIONS" ]]; then
    pass "业务文件未直接使用 LINUX_VERSION_CODE"
else
    fail "以下文件直接使用了 LINUX_VERSION_CODE，应改调 compat.h 封装："
    printf '%s\n' "$VIOLATIONS" | sed 's/^/         /'
fi

# ---- 7. 加载/卸载冒烟测试（需要 root） ----
if [[ $EUID -eq 0 ]]; then
    info "尝试加载/卸载模块..."
    if insmod net_ndnic.ko 2>insmod.log; then
        pass "insmod 成功"
        rmmod net_ndnic 2>rmmod.log && pass "rmmod 成功" || fail "rmmod 失败"
    else
        # 没有匹配硬件时加载失败是预期的，只记录
        info "insmod 未成功（可能无匹配硬件），日志："
        tail -5 insmod.log | sed 's/^/         /'
    fi
else
    info "非 root，跳过 insmod/rmmod 冒烟测试"
fi

# ---- 汇总 ----
echo
if [[ $FAIL -eq 0 ]]; then
    echo "==== 全部验证通过 ===="
    exit 0
else
    echo "==== 存在 $FAIL 项失败 ===="
    exit 1
fi