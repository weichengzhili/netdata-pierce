# SPDX-License-Identifier: GPL-2.0
#
# netdata-pierce —— 工业网卡（ndnic）数据面工程顶层构建脚本
#
# 组件划分：
#   kernel/common   : 内核/用户态共享 UAPI 头文件（仅 INTERFACE 目标，方便被各处 include）
#   kernel/driver   : net_ndnic.ko       —— 走 Kbuild（make -C <KDIR> M=... modules）
#   kernel/xdp      : nd_xdp_kern.bpf.o  —— clang -target bpf 编译
#                     nd_xdp_loader      —— 用户态 libbpf loader
#   control/        : 用户态控制面静态库（有 CMakeLists.txt 则 add_subdirectory）
#   dataplane/      : 用户态数据面静态库
#   apps/           : 每个含 main() 的源文件单独生成一个可执行程序
#   main.cpp        : 顶层 CLI（netdata-pierce）
#
# 内核态组件只在 Linux 上启用；非 Linux 平台自动跳过（不会配置失败），
# 因此本文件在 Windows/macOS 上也能正常 configure/build（只编用户态部分）。
#

cmake_minimum_required(VERSION 3.20)

project(netdata-pierce
        VERSION 0.1.0
        DESCRIPTION "Industrial NIC dataplane: kernel driver + XDP + user-space control/dataplane"
        LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# 0. 全局设置
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_EXTENSIONS ON)      # 内核风格代码常用 GNU 扩展
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(NDP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(NDP_DRIVER_DIR "${NDP_ROOT}/kernel/driver")
set(NDP_XDP_DIR "${NDP_ROOT}/kernel/xdp")
set(NDP_UAPI_DIR "${NDP_ROOT}/kernel/common")
set(NDP_BPF_OUT "${CMAKE_BINARY_DIR}/kernel/xdp")
file(MAKE_DIRECTORY "${NDP_BPF_OUT}")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(NDP_LINUX TRUE)
else()
    set(NDP_LINUX FALSE)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# 1. 构建开关
# ---------------------------------------------------------------------------
option(NDP_BUILD_KMOD        "构建内核模块 net_ndnic.ko（仅 Linux）"     ${NDP_LINUX})
option(NDP_BUILD_XDP         "构建 XDP 程序与 libbpf loader（仅 Linux）" ${NDP_LINUX})
option(NDP_BUILD_XDP_SKEL    "用 bpftool 生成 BPF skeleton 头文件"       ON)
option(NDP_BUILD_APPS        "构建 apps/ 下的用户态程序"                  ON)
option(NDP_BUILD_CLI         "构建顶层 CLI（main.cpp -> netdata-pierce）" ON)
option(NDP_BUILD_TESTS       "注册 CTest 用例（scripts/verify_*.sh）"      OFF)
option(NDP_AFXDP_ZEROCOPY    "启用 AF_XDP 零拷贝路径（CONFIG_NET_NDNIC_AFXDP_ZC=y/m）" OFF)
option(NDP_USE_BUNDLED_LIBBPF "优先使用 third_party/libbpf 源码构建"      ON)

set(NDP_KERNEL_BUILD_DIR "" CACHE PATH "内核构建树（KDIR），默认 /lib/modules/<release>/build")
set(NDP_VMLINUX_H        "" CACHE FILEPATH "自定义 vmlinux.h 路径（留空则自动从 BTF 生成）")
set(NDP_BPF_CLANG        "" CACHE FILEPATH "用于编译 BPF 的 clang 路径（留空自动查找）")
set(NDP_BPFTOOL          "" CACHE FILEPATH "bpftool 路径（留空自动查找）")

# ---------------------------------------------------------------------------
# 2. 工具/依赖查找
# ---------------------------------------------------------------------------
find_program(NDP_MAKE_EXECUTABLE NAMES make gmake)
find_program(NDP_BASH_EXECUTABLE NAMES bash)

if(NDP_BPF_CLANG)
    set(CLANG_FOR_BPF "${NDP_BPF_CLANG}")
else()
    find_program(CLANG_FOR_BPF NAMES clang clang-18 clang-17 clang-16 clang-15)
endif()
if(NDP_BPFTOOL)
    set(BPFTOOL_EXECUTABLE "${NDP_BPFTOOL}")
else()
    find_program(BPFTOOL_EXECUTABLE NAMES bpftool)
endif()
find_program(LLVM_STRIP_EXECUTABLE NAMES llvm-strip)

find_package(PkgConfig QUIET)
if(PkgConfig)
    pkg_check_modules(PC_LIBBPF QUIET libbpf)
endif()

set(NDP_LIBBPF_SRC "${NDP_ROOT}/third_party/libbpf/src")
find_path(LIBBPF_INCLUDE_DIR
        NAMES bpf/libbpf.h
        HINTS ${PC_LIBBPF_INCLUDEDIR} "${NDP_LIBBPF_SRC}/../include" /usr/include /usr/local/include)
find_library(LIBBPF_LIBRARY
        NAMES bpf libbpf.a
        HINTS ${PC_LIBBPF_LIBDIR} "${NDP_LIBBPF_SRC}" /usr/lib64 /usr/lib /usr/local/lib)
find_library(LIBELF_LIBRARY NAMES elf)
find_library(LIBZSTD_LIBRARY NAMES zstd)
find_package(ZLIB QUIET)

# ---------------------------------------------------------------------------
# 3. 通用辅助函数
# ---------------------------------------------------------------------------
# 判断源文件是否定义了 main()（apps/ 目录据此决定是否生成可执行文件）
function(ndp_source_has_main _src _out)
    file(STRINGS "${_src}" _lines REGEX "^[ \t]*int[ \t]+main[ \t]*\\(")
    if(_lines)
        set(${_out} TRUE PARENT_SCOPE)
    else()
        set(${_out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 4. 共享 UAPI 头文件（INTERFACE 目标）
# ---------------------------------------------------------------------------
add_library(ndp_uapi INTERFACE)
target_include_directories(ndp_uapi INTERFACE "${NDP_UAPI_DIR}" "${NDP_DRIVER_DIR}")

# 头文件里若混入了带空格的文件名（例如 " nd_uapi.h"），#include 会失败，这里给出提示
file(GLOB_RECURSE NDP_BAD_FILENAMES LIST_DIRECTORIES FALSE "${NDP_ROOT}/kernel/* *")
if(NDP_BAD_FILENAMES)
    message(WARNING "以下文件名包含空格，建议重命名（例如 \" nd_uapi.h\" -> \"nd_uapi.h\"）：")
    foreach(_f IN LISTS NDP_BAD_FILENAMES)
        message(WARNING "  ${_f}")
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 5. 内核模块：net_ndnic.ko（Kbuild）
# ---------------------------------------------------------------------------
if(NDP_BUILD_KMOD)
    if(NOT NDP_LINUX)
        message(STATUS "[kmod] 非 Linux 平台，跳过内核模块构建")
    elseif(NOT EXISTS "${NDP_DRIVER_DIR}/Makefile")
        message(STATUS "[kmod] 未找到 kernel/driver/Makefile，跳过")
    else()
        # 内核版本号 / KDIR
        set(NDP_KERNEL_RELEASE "")
        if(EXISTS "/proc/sys/kernel/osrelease")
            file(READ "/proc/sys/kernel/osrelease" NDP_KERNEL_RELEASE)
            string(STRIP "${NDP_KERNEL_RELEASE}" NDP_KERNEL_RELEASE)
        endif()
        if(NOT NDP_KERNEL_BUILD_DIR)
            if(NDP_KERNEL_RELEASE)
                set(NDP_KERNEL_BUILD_DIR "/lib/modules/${NDP_KERNEL_RELEASE}/build")
            else()
                set(NDP_KERNEL_BUILD_DIR "/lib/modules/$(shell uname -r)/build")
            endif()
        endif()
        if(NOT EXISTS "${NDP_KERNEL_BUILD_DIR}")
            message(WARNING "[kmod] 内核构建树不存在：${NDP_KERNEL_BUILD_DIR}"
                    "（可用 -DNDP_KERNEL_BUILD_DIR=<path> 指定），仍会注册目标，构建时才会报错")
        endif()
        if(NOT NDP_MAKE_EXECUTABLE)
            message(WARNING "[kmod] 未找到 make，无法构建内核模块")
        endif()

        set(NDP_KMOD_OBJS
                net_ndnic_main
                net_ndnic_pci
                net_ndnic_irq
                net_ndnic_napi
                net_ndnic_ring
                net_ndnic_xdp
                net_ndnic_ethtool
                net_ndnic_stats)

        set(NDP_KMOD_SOURCES "")
        foreach(_m IN LISTS NDP_KMOD_OBJS)
            list(APPEND NDP_KMOD_SOURCES "${NDP_DRIVER_DIR}/${_m}.c")
        endforeach()
        file(GLOB NDP_KMOD_HEADERS CONFIGURE_DEPENDS
                "${NDP_DRIVER_DIR}/*.h" "${NDP_UAPI_DIR}/*.h")

        if(NDP_AFXDP_ZEROCOPY)
            set(NDP_KMOD_AFXDP "y")
        else()
            set(NDP_KMOD_AFXDP "n")
        endif()

        set(NDP_KMOD_OUTPUT "${CMAKE_BINARY_DIR}/kernel/driver/net_ndnic.ko")

        if(NDP_MAKE_EXECUTABLE)
            add_custom_command(
                    OUTPUT "${NDP_KMOD_OUTPUT}"
                    COMMAND ${CMAKE_COMMAND} -E env
                            "CONFIG_NET_NDNIC_AFXDP_ZC=${NDP_KMOD_AFXDP}"
                            "${NDP_MAKE_EXECUTABLE}" -C "${NDP_KERNEL_BUILD_DIR}"
                            "M=${NDP_DRIVER_DIR}" modules
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${NDP_DRIVER_DIR}/net_ndnic.ko" "${NDP_KMOD_OUTPUT}"
                    DEPENDS ${NDP_KMOD_SOURCES} ${NDP_KMOD_HEADERS} "${NDP_DRIVER_DIR}/Makefile"
                    WORKING_DIRECTORY "${NDP_DRIVER_DIR}"
                    COMMENT "[kmod] make -C ${NDP_KERNEL_BUILD_DIR} M=kernel/driver modules"
                    VERBATIM)
            add_custom_target(net_ndnic_ko ALL DEPENDS "${NDP_KMOD_OUTPUT}")

            add_custom_target(kmod_clean
                    COMMAND ${CMAKE_COMMAND} -E env "CONFIG_NET_NDNIC_AFXDP_ZC=${NDP_KMOD_AFXDP}"
                            "${NDP_MAKE_EXECUTABLE}" -C "${NDP_KERNEL_BUILD_DIR}"
                            "M=${NDP_DRIVER_DIR}" clean
                    WORKING_DIRECTORY "${NDP_DRIVER_DIR}"
                    COMMENT "[kmod] clean")

            if(NDP_BASH_EXECUTABLE AND EXISTS "${NDP_ROOT}/scripts/verify_kernel_module.sh")
                add_custom_target(kmod_verify
                        COMMAND "${NDP_BASH_EXECUTABLE}"
                                "${NDP_ROOT}/scripts/verify_kernel_module.sh"
                        DEPENDS net_ndnic_ko
                        WORKING_DIRECTORY "${NDP_ROOT}"
                        COMMENT "[kmod] verify net_ndnic.ko")
            endif()
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# 6. XDP：BPF 程序 + 用户态 loader
# ---------------------------------------------------------------------------
if(NDP_BUILD_XDP)
    if(NOT NDP_LINUX)
        message(STATUS "[xdp] 非 Linux 平台，跳过 XDP 构建")
    elseif(NOT CLANG_FOR_BPF)
        message(STATUS "[xdp] 未找到 clang，跳过 BPF 程序构建（可用 -DNDP_BPF_CLANG=<path> 指定）")
    else()
        # 6.1 vmlinux.h：优先用户指定 -> BTF 生成 -> 报错
        if(NDP_VMLINUX_H AND EXISTS "${NDP_VMLINUX_H}")
            set(NDP_VMLINUX_H_PATH "${NDP_VMLINUX_H}")
            set(NDP_VMLINUX_TARGET "")
        elseif(BPFTOOL_EXECUTABLE AND EXISTS "/sys/kernel/btf/vmlinux")
            set(NDP_VMLINUX_H_PATH "${NDP_BPF_OUT}/vmlinux.h")
            add_custom_command(
                    OUTPUT "${NDP_VMLINUX_H_PATH}"
                    COMMAND "${BPFTOOL_EXECUTABLE}" btf dump file /sys/kernel/btf/vmlinux format c
                            > "${NDP_VMLINUX_H_PATH}"
                    COMMENT "[xdp] 生成 vmlinux.h"
                    VERBATIM)
            set(NDP_VMLINUX_TARGET "${NDP_VMLINUX_H_PATH}")
        else()
            message(WARNING "[xdp] 既无 NDP_VMLINUX_H 也无 /sys/kernel/btf/vmlinux（BTF 未开启），"
                    "BPF 程序可能无法编译；可用 -DNDP_VMLINUX_H=<path> 指定")
            set(NDP_VMLINUX_H_PATH "")
            set(NDP_VMLINUX_TARGET "")
        endif()

        # 目标架构（BPF 侧用 x86/arm64 等简称）
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            set(NDP_BPF_ARCH "x86")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
            set(NDP_BPF_ARCH "arm64")
        else()
            string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" NDP_BPF_ARCH)
        endif()

        # 6.2 nd_xdp_kern.bpf.o
        set(NDP_BPF_OBJ "${NDP_BPF_OUT}/nd_xdp_kern.bpf.o")
        set(NDP_BPF_STRIP_CMD "")
        if(LLVM_STRIP_EXECUTABLE)
            set(NDP_BPF_STRIP_CMD COMMAND "${LLVM_STRIP_EXECUTABLE}" -g "${NDP_BPF_OBJ}")
        endif()
        add_custom_command(
                OUTPUT "${NDP_BPF_OBJ}"
                COMMAND "${CLANG_FOR_BPF}" -g -O2 -target bpf
                        -D__TARGET_ARCH_${NDP_BPF_ARCH}
                        -I"${NDP_BPF_OUT}" -I"${NDP_UAPI_DIR}" -I"${NDP_XDP_DIR}"
                        -Wall
                        -c "${NDP_XDP_DIR}/nd_xdp_kern.c" -o "${NDP_BPF_OBJ}"
                ${NDP_BPF_STRIP_CMD}
                DEPENDS "${NDP_XDP_DIR}/nd_xdp_kern.c" ${NDP_VMLINUX_TARGET}
                COMMENT "[xdp] clang -target bpf -> nd_xdp_kern.bpf.o"
                VERBATIM)
        add_custom_target(nd_xdp_kern ALL DEPENDS "${NDP_BPF_OBJ}")

        # 6.3 可选：bpftool gen skeleton
        if(NDP_BUILD_XDP_SKEL AND BPFTOOL_EXECUTABLE)
            set(NDP_SKEL_H "${NDP_BPF_OUT}/nd_xdp_kern.skel.h")
            add_custom_command(
                    OUTPUT "${NDP_SKEL_H}"
                    COMMAND "${BPFTOOL_EXECUTABLE}" gen skeleton "${NDP_BPF_OBJ}"
                            > "${NDP_SKEL_H}"
                    DEPENDS "${NDP_BPF_OBJ}"
                    COMMENT "[xdp] 生成 nd_xdp_kern.skel.h"
                    VERBATIM)
            add_custom_target(nd_xdp_skel ALL DEPENDS "${NDP_SKEL_H}")
        endif()

        # 6.4 libbpf：bundled（ExternalProject）或系统库
        set(NDP_LIBBPF_TARGET "")
        set(NDP_LIBBPF_DEPS "")
        if(NDP_USE_BUNDLED_LIBBPF AND EXISTS "${NDP_LIBBPF_SRC}/Makefile" AND NDP_MAKE_EXECUTABLE)
            include(ExternalProject)
            set(NDP_LIBBPF_PREFIX "${CMAKE_BINARY_DIR}/third_party/libbpf")
            ExternalProject_Add(libbpf_bundled
                    SOURCE_DIR "${NDP_LIBBPF_SRC}"
                    PREFIX "${NDP_LIBBPF_PREFIX}"
                    BUILD_IN_SOURCE 1
                    CONFIGURE_COMMAND ""
                    BUILD_COMMAND "${NDP_MAKE_EXECUTABLE}" -C "${NDP_LIBBPF_SRC}"
                                   BUILD_STATIC_ONLY=y
                                   OBJDIR=${NDP_LIBBPF_PREFIX}/build
                                   DESTDIR=${NDP_LIBBPF_PREFIX}/stage
                                   INCLUDEDIR= LIBDIR= UAPIDIR=
                                   install
                    INSTALL_COMMAND ""
                    BUILD_BYPRODUCTS "${NDP_LIBBPF_PREFIX}/build/libbpf.a"
                    LOG_BUILD ON)
            add_library(ndp_libbpf STATIC IMPORTED GLOBAL)
            set_target_properties(ndp_libbpf PROPERTIES
                    IMPORTED_LOCATION "${NDP_LIBBPF_PREFIX}/build/libbpf.a"
                    INTERFACE_INCLUDE_DIRECTORIES
                        "${NDP_LIBBPF_SRC}" "${NDP_LIBBPF_SRC}/../include/uapi")
            set(NDP_LIBBPF_TARGET "ndp_libbpf")
            set(NDP_LIBBPF_DEPS "libbpf_bundled")
        elseif(LIBBPF_LIBRARY AND LIBBPF_INCLUDE_DIR)
            add_library(ndp_libbpf UNKNOWN IMPORTED GLOBAL)
            set_target_properties(ndp_libbpf PROPERTIES
                    IMPORTED_LOCATION "${LIBBPF_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${LIBBPF_INCLUDE_DIR}")
            set(NDP_LIBBPF_TARGET "ndp_libbpf")
        endif()

        # 6.5 用户态 loader 可执行文件
        ndp_source_has_main("${NDP_XDP_DIR}/nd_xdp_user.c" NDP_XDP_USER_HAS_MAIN)
        if(NOT NDP_XDP_USER_HAS_MAIN)
            message(STATUS "[xdp] kernel/xdp/nd_xdp_user.c 尚未实现 main()，跳过 nd_xdp_loader")
        elseif(NOT NDP_LIBBPF_TARGET)
            message(STATUS "[xdp] 未找到 libbpf（系统库或 third_party/libbpf），跳过 nd_xdp_loader")
        else()
            add_executable(nd_xdp_loader "${NDP_XDP_DIR}/nd_xdp_user.c"
                    "${NDP_XDP_DIR}/nd_xdp_user.h")
            target_include_directories(nd_xdp_loader PRIVATE
                    "${NDP_XDP_DIR}" "${NDP_BPF_OUT}" "${NDP_UAPI_DIR}")
            target_link_libraries(nd_xdp_loader PRIVATE ${NDP_LIBBPF_TARGET} ndp_uapi)
            if(LIBELF_LIBRARY)
                target_link_libraries(nd_xdp_loader PRIVATE "${LIBELF_LIBRARY}")
            endif()
            if(LIBZSTD_LIBRARY)
                target_link_libraries(nd_xdp_loader PRIVATE "${LIBZSTD_LIBRARY}")
            endif()
            if(ZLIB_FOUND)
                target_link_libraries(nd_xdp_loader PRIVATE ZLIB::ZLIB)
            endif()
            add_dependencies(nd_xdp_loader nd_xdp_kern ${NDP_LIBBPF_DEPS})
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# 7. 用户态：control / dataplane 库
# ---------------------------------------------------------------------------
if(NDP_BUILD_APPS)
    foreach(_mod IN ITEMS control dataplane)
        set(_dir "${NDP_ROOT}/${_mod}")
        if(EXISTS "${_dir}/CMakeLists.txt")
            add_subdirectory("${_dir}" "${CMAKE_BINARY_DIR}/${_mod}")
        else()
            file(GLOB _srcs CONFIGURE_DEPENDS "${_dir}/*.c" "${_dir}/*.cc" "${_dir}/*.cpp")
            file(GLOB _hdrs CONFIGURE_DEPENDS "${_dir}/*.h" "${_dir}/*.hpp")
            if(_srcs)
                add_library(ndp_${_mod} STATIC ${_srcs} ${_hdrs})
                target_include_directories(ndp_${_mod} PUBLIC "${_dir}")
                target_link_libraries(ndp_${_mod} PUBLIC ndp_uapi)
            else()
                message(STATUS "[${_mod}] 目录暂无源文件，跳过 ndp_${_mod}")
            endif()
        endif()
    endforeach()

    # -----------------------------------------------------------------------
    # 8. apps/：每个含 main() 的源文件 -> 一个可执行程序
    # -----------------------------------------------------------------------
    set(_apps_dir "${NDP_ROOT}/apps")
    if(EXISTS "${_apps_dir}/CMakeLists.txt")
        add_subdirectory("${_apps_dir}" "${CMAKE_BINARY_DIR}/apps")
    else()
        file(GLOB _app_srcs CONFIGURE_DEPENDS "${_apps_dir}/*.c" "${_apps_dir}/*.cc" "${_apps_dir}/*.cpp")
        if(NOT _app_srcs)
            message(STATUS "[apps] 目录暂无源文件，跳过")
        endif()
        foreach(_src IN LISTS _app_srcs)
            ndp_source_has_main("${_src}" _has_main)
            if(NOT _has_main)
                continue()
            endif()
            get_filename_component(_name "${_src}" NAME_WE)
            add_executable(${_name} "${_src}")
            target_link_libraries(${_name} PRIVATE ndp_uapi)
            if(TARGET ndp_control)
                target_link_libraries(${_name} PRIVATE ndp_control)
            endif()
            if(TARGET ndp_dataplane)
                target_link_libraries(${_name} PRIVATE ndp_dataplane)
            endif()
            list(APPEND NDP_APP_TARGETS ${_name})
        endforeach()
    endif()
endif()

# ---------------------------------------------------------------------------
# 9. 顶层 CLI：main.cpp -> netdata-pierce
# ---------------------------------------------------------------------------
if(NDP_BUILD_CLI AND EXISTS "${NDP_ROOT}/main.cpp")
    add_executable(netdata-pierce "${NDP_ROOT}/main.cpp")
    target_link_libraries(netdata-pierce PRIVATE ndp_uapi)
    if(TARGET ndp_control)
        target_link_libraries(netdata-pierce PRIVATE ndp_control)
    endif()
    if(TARGET ndp_dataplane)
        target_link_libraries(netdata-pierce PRIVATE ndp_dataplane)
    endif()
endif()

# ---------------------------------------------------------------------------
# 10. 测试（scripts/verify_*.sh）
# ---------------------------------------------------------------------------
if(NDP_BUILD_TESTS)
    enable_testing()
    if(NDP_LINUX AND NDP_BASH_EXECUTABLE)
        if(TARGET net_ndnic_ko AND EXISTS "${NDP_ROOT}/scripts/verify_kernel_module.sh")
            add_test(NAME verify_kernel_module
                    COMMAND "${NDP_BASH_EXECUTABLE}" "${NDP_ROOT}/scripts/verify_kernel_module.sh")
        endif()
        if(TARGET nd_xdp_kern AND EXISTS "${NDP_ROOT}/scripts/verify_xdp_build.sh")
            add_test(NAME verify_xdp_build
                    COMMAND "${NDP_BASH_EXECUTABLE}" "${NDP_ROOT}/scripts/verify_xdp_build.sh")
        endif()
        if(EXISTS "${NDP_ROOT}/scripts/verify_all.sh")
            file(STRINGS "${NDP_ROOT}/scripts/verify_all.sh" _all_lines LIMIT_INPUT 1024)
            if(_all_lines)
                add_test(NAME verify_all
                        COMMAND "${NDP_BASH_EXECUTABLE}" "${NDP_ROOT}/scripts/verify_all.sh")
            endif()
        endif()
    elseif(NOT NDP_LINUX)
        # 非 Linux：只跑用户态可执行文件的冒烟测试
        get_property(_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_t IN LISTS _all_targets)
            get_target_property(_type ${_t} TYPE)
            if(_type STREQUAL "EXECUTABLE")
                add_test(NAME smoke_${_t} COMMAND ${_t})
            endif()
        endforeach()
    endif()
endif()

# ---------------------------------------------------------------------------
# 11. 安装规则
# ---------------------------------------------------------------------------
set(NDP_RUNTIME_TARGETS "")
foreach(_t IN ITEMS netdata-pierce nd_xdp_loader ${NDP_APP_TARGETS})
    if(TARGET ${_t})
        list(APPEND NDP_RUNTIME_TARGETS ${_t})
    endif()
endforeach()
if(NDP_RUNTIME_TARGETS)
    install(TARGETS ${NDP_RUNTIME_TARGETS}
            RUNTIME DESTINATION bin
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib)
endif()
foreach(_t IN ITEMS ndp_control ndp_dataplane)
    if(TARGET ${_t})
        install(TARGETS ${_t} ARCHIVE DESTINATION lib)
    endif()
endforeach()

if(TARGET net_ndnic_ko)
    if(NDP_KERNEL_RELEASE)
        set(NDP_KMOD_INSTALL_DIR "lib/modules/${NDP_KERNEL_RELEASE}/extra")
    else()
        set(NDP_KMOD_INSTALL_DIR "lib/modules/extra")
    endif()
    install(FILES "${NDP_KMOD_OUTPUT}"
            DESTINATION "${NDP_KMOD_INSTALL_DIR}"
            OPTIONAL)
endif()
if(TARGET nd_xdp_kern)
    install(FILES "${NDP_BPF_OBJ}" DESTINATION "share/netdata-pierce/xdp" OPTIONAL)
endif()
install(DIRECTORY "${NDP_UAPI_DIR}/" DESTINATION "include/netdata-pierce"
        FILES_MATCHING PATTERN "*.h")
install(DIRECTORY "${NDP_ROOT}/scripts/" OPTIONAL
        DESTINATION "share/netdata-pierce/scripts"
        FILES_MATCHING
        PATTERN "*.sh"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)

# ---------------------------------------------------------------------------
# 12. 配置摘要
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== netdata-pierce ${PROJECT_VERSION} 配置摘要 ===")
message(STATUS "  平台            : ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  构建类型        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C/C++ 编译器    : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} / ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(NDP_LINUX)
    message(STATUS "  内核模块        : ${NDP_BUILD_KMOD} (KDIR=${NDP_KERNEL_BUILD_DIR})")
    message(STATUS "  AF_XDP 零拷贝   : ${NDP_AFXDP_ZEROCOPY}")
    message(STATUS "  XDP             : ${NDP_BUILD_XDP}")
    message(STATUS "    clang (BPF)   : ${CLANG_FOR_BPF}")
    message(STATUS "    bpftool       : ${BPFTOOL_EXECUTABLE}")
    message(STATUS "    vmlinux.h     : ${NDP_VMLINUX_H_PATH}")
    message(STATUS "    libbpf        : ${LIBBPF_LIBRARY}")
else()
    message(STATUS "  内核模块 / XDP  : 跳过（非 Linux 平台）")
endif()
message(STATUS "  apps            : ${NDP_BUILD_APPS}")
message(STATUS "  CLI             : ${NDP_BUILD_CLI}")
message(STATUS "  tests           : ${NDP_BUILD_TESTS}")
message(STATUS "================================================")
message(STATUS "")
