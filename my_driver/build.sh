# 指定内核源码路径（如果不在默认位置）
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export KERNEL_SRC=/home/lion/workdir/sourcecode/linux-6.9.3

# 执行编译
#cd my_driver
make clean
#make
