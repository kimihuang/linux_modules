# Memmap 模块 Buildroot Package 说明

## 概述

memmap 模块现在作为一个独立的 Buildroot package 构建，这样可以：
- 修改模块源码后只需要重新编译模块，不需要重新编译整个内核
- 快速迭代开发和调试

## 目录结构

```
board/board_qemu_a/br2_external/package/memmap-module/
├── Config.in              # Buildroot 配置选项
├── memmap-module.mk       # 构建规则
└── memmap-module.hash     # 哈希校验

src/modules/memmap/
├── src/
│   └── memmap.c          # 驱动源码
├── COPYING               # 许可证文件
└── README.md             # 说明文档
```

## 编译方法

### 方法1: 使用快速脚本（推荐）

修改源码后执行：

```bash
source build/envsetup.sh
lunch board_qemu_a
scripts/rebuild-memmap.sh
```

然后重新生成 rootfs：

```bash
make rootfs-rebuild
```

### 方法2: 使用 Buildroot 命令

```bash
source build/envsetup.sh
lunch board_qemu_a

# 只重新编译 memmap 模块
make memmap-module-rebuild

# 重新生成 rootfs 镜像
make rootfs-rebuild
```

### 方法3: 完全重新编译

```bash
source build/envsetup.sh
lunch board_qemu_a
make
```

## 配置选项

在 Buildroot 配置中启用/禁用 memmap 模块：

```bash
make menuconfig
# 进入: Quantum Board Packages -> memmap-module
```

或者直接编辑 `board/board_qemu_a/br2_external/configs/buildroot_defconfig`：

```
BR2_PACKAGE_MEMMAP_MODULE=y    # 启用
# BR2_PACKAGE_MEMMAP_MODULE is not set  # 禁用
```

## 与内核内建模块的区别

### 之前（内核内建模块）
- 模块编译在内核构建过程中
- 修改源码需要重新编译整个内核：`make linux-rebuild`
- 模块路径：`kernel/drivers/modules/memmap/memmap.ko`

### 现在（Buildroot package）
- 模块作为独立的 package 编译
- 修改源码只需要重新编译模块：`make memmap-module-rebuild`
- 模块路径：`drivers/memmap/memmap.ko`

## 测试

启动 QEMU 后，模块会自动加载（通过 S80memdisk init 脚本）：

```bash
boot
```

手动测试：

```bash
# 加载模块
insmod /lib/modules/$(uname -r)/kernel/drivers/memmap/memmap.ko memmap_param="256M\$0x78000000"

# 查看设备
ls -l /dev/memblock0

# 格式化
mkfs.ext4 /dev/memblock0

# 挂载
mkdir -p /mnt/memdisk
mount /dev/memblock0 /mnt/memdisk

# 测试写入
echo "Hello, memmap!" > /mnt/memdisk/test.txt
cat /mnt/memdisk/test.txt
```

## 开发工作流

1. **修改源码**
   ```bash
   vim src/modules/memmap/src/memmap.c
   ```

2. **快速编译**
   ```bash
   scripts/rebuild-memmap.sh
   ```

3. **重新生成 rootfs**
   ```bash
   make rootfs-rebuild
   ```

4. **测试**
   ```bash
   boot
   ```

## 故障排除

### 模块路径错误

如果 S80memdisk 脚本找不到模块，检查路径：

```bash
# 查找模块位置
find /mnt -name "memmap.ko"
```

如果路径不是预期的，检查 `board/board_qemu_a/rootfs-overlay/etc/init.d/S80memdisk`。

### 编译错误

确保内核已经编译过至少一次：

```bash
make linux-rebuild
```

### 模块加载失败

查看内核日志：

```bash
dmesg | grep memmap
```

检查模块参数和地址是否正确。
