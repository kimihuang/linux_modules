# Memmap Memory Driver

## 概述

memmap 内核模块驱动程序提供了对预留内存区域的块设备访问。它将物理内存区域映射为块设备，可用作临时存储。

## 特性

- 将物理内存区域映射为块设备（`/dev/memblock0`, `/dev/memblock1`, ...）
- 支持多个内存区域（最多 8 个设备）
- 使用 block multiqueue API
- 支持读写操作
- 可作为模块或内置驱动编译

## 编译

### 方法 1: 使用构建脚本（推荐）

```bash
# 初始化环境
source build/envsetup.sh
lunch board_qemu_a

# 编译模块
cd src/modules/memmap
./build.sh
```

### 方法 2: 直接使用内核 Makefile

```bash
# 初始化环境
source build/envsetup.sh
lunch board_qemu_a

# 编译所有驱动模块（包括 memmap）
make modules

# 或只编译 memmap
make M=drivers/modules/memmap
```

### 方法 3: 使用顶层 Makefile

```bash
# 初始化环境
source build/envsetup.sh
lunch board_qemu_a

# 编译所有模块
make modules
```

## 使用方法

### 加载模块

```bash
# 加载模块，映射 256MB 内存 @ 0x78000000
insmod memmap.ko memmap=256M\$0x78000000

# 或使用转义字符
insmod memmap.ko memmap="256M\\\$0x78000000"

# 加载多个内存区域
insmod memmap.ko memmap="256M\$0x78000000,64M@0x80000000"
```

### 参数格式

格式：`<大小><单位>@<地址>` 或 `<大小><单位>\$<地址>`

- 大小：数字（如 256）
- 单位：K（KB）、M（MB）、G（GB）
- 地址：十六进制地址（如 0x78000000）

示例：
- `256M\$0x78000000` - 256MB @ 0x78000000
- `64K@0x90000000` - 64KB @ 0x90000000
- `1G\$0x40000000` - 1GB @ 0x40000000

### 查看设备

```bash
# 查看创建的设备
ls -l /dev/memblock*
# 输出: /dev/memblock0

# 查看内核日志
dmesg | grep memmap
```

### 格式化设备

```bash
# 格式化为 ext4
mkfs.ext4 /dev/memblock0

# 或格式化为 vfat
mkfs.vfat /dev/memblock0
```

### 挂载设备

```bash
# 创建挂载点
mkdir -p /mnt/memdisk

# 挂载
mount /dev/memblock0 /mnt/memdisk

# 查看内容
ls -la /mnt/memdisk
```

### 使用设备

```bash
# 写入文件
echo "Hello, memdisk!" > /mnt/memdisk/test.txt

# 读取文件
cat /mnt/memdisk/test.txt

# 拷贝文件
cp /tmp/data.bin /mnt/memdisk/

# 查看使用情况
df -h /mnt/memdisk
```

### 卸载和清理

```bash
# 卸载设备
umount /mnt/memdisk

# 卸载模块
rmmod memmap
```

## 内核配置

在内核配置中启用 memmap 驱动：

```
Device Drivers  --->
    [*] Quantum Drivers  --->
        [*] Quantum Memmap Memory Driver
```

或直接在配置文件中设置：

```
CONFIG_MEMMAP_MODULE=m
```

## 预留内存

确保在设备树中预留了相应的内存区域：

```dts
reserved-memory {
    memdisk_reserved: memdisk@78000000 {
        compatible = "shared-dma-pool";
        reg = <0x00 0x78000000 0x00 0x10000000>; /* 256MB */
        no-map;
    };
};
```

## 故障排查

### 问题 1: 设备未创建

**症状**: 加载模块后没有 `/dev/memblock0` 设备

**解决方案**:
- 检查内核日志：`dmesg | grep memmap`
- 确认内存地址正确且未被其他驱动使用
- 检查 `CONFIG_BLK_DEV_RAM` 和 `CONFIG_MEMMAP_MODULE` 配置

### 问题 2: 内存映射失败

**症状**: `Failed to remap physical memory`

**解决方案**:
- 确认内存地址在可用范围内
- 检查设备树预留配置
- 确认内存未被其他内核子系统占用

### 问题 3: 无法挂载

**症状**: `mount: /dev/memblock0 is not a valid block device`

**解决方案**:
- 确认设备存在：`ls -l /dev/memblock0`
- 格式化设备：`mkfs.ext4 /dev/memblock0`
- 检查内核日志中的错误信息

## 性能优化

### 使用大块大小

```bash
# 使用 1MB 块大小
dd if=/dev/zero of=/mnt/memdisk/test.img bs=1M count=100
```

### 调整队列深度

修改驱动中的 `dev->tag_set.queue_depth` 值。

## 安全注意事项

1. 直接映射物理内存需要适当的权限
2. 错误的内存操作可能导致系统崩溃
3. 确保预留的内存区域不会与其他内核子系统冲突
4. 使用 `no-map` 属性防止内核分配此内存

## 相关文档

- 主文档: `/docs/MEMDISK_GUIDE.md`
- 设备树绑定: 内核源码 `Documentation/devicetree/bindings/reserved-memory/`
- Block 层文档: 内核源码 `Documentation/block/`
