#!/bin/bash
#
# Build memmap kernel module
#

set -e

# 获取项目根目录
PROJECT_ROOT=$(cd "$(dirname "$0")/../../.." && pwd)

# 检查是否已选择板卡配置
if [ -z "$BOARD_NAME" ]; then
    echo "错误: 未选择板卡配置，请先执行: source build/envsetup.sh && lunch"
    exit 1
fi

# 检查内核输出目录
if [ -z "$KERNEL_OUT_DIR" ]; then
    echo "错误: KERNEL_OUT_DIR 未设置"
    exit 1
fi

echo "======================================"
echo "编译 memmap 内核模块..."
echo "======================================"
echo "板卡: $BOARD_NAME"
echo "内核输出目录: $KERNEL_OUT_DIR"
echo "交叉编译工具链: $CROSS_COMPILE"
echo ""

# 进入 memmap 目录
cd "$PROJECT_ROOT/src/modules/memmap"

# 编译模块
make -C "$KERNEL_OUT_DIR" M="$PROJECT_ROOT/src/modules/memmap" \
    ARCH="$KERNEL_ARCH" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    modules

if [ $? -eq 0 ]; then
    echo ""
    echo "======================================"
    echo "memmap 模块编译成功!"
    echo "======================================"
    echo "模块: $(pwd)/memmap.ko"
    ls -lh memmap.ko
    echo ""
    echo "使用方法:"
    echo "  # 加载模块 (映射 256MB @ 0x78000000)"
    echo "  insmod memmap.ko memmap=256M\\\$0x78000000"
    echo ""
    echo "  # 或者加载多个区域"
    echo "  insmod memmap.ko memmap=\\\"256M\\\$0x78000000,64M@0x80000000\\\""
    echo ""
    echo "  # 查看设备"
    echo "  ls -l /dev/memblock*"
    echo ""
    echo "  # 格式化设备"
    echo "  mkfs.ext4 /dev/memblock0"
    echo ""
    echo "  # 挂载设备"
    echo "  mkdir -p /mnt/memdisk"
    echo "  mount /dev/memblock0 /mnt/memdisk"
    echo "======================================"
else
    echo "memmap 模块编译失败!"
    exit 1
fi
