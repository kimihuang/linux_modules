# Makefile for memmap driver

# 目标模块名称
MODULE_NAME := memmap

# 源文件
SRC := src/memmap.c

# 目标对象文件
OBJ := $(SRC:.c=.o)

# 内核构建系统
KERNEL_BUILD ?= /lib/modules/$(shell uname -r)/build

# 默认目标
all: $(MODULE_NAME).ko

$(MODULE_NAME).ko: $(OBJ)
	make -C $(KERNEL_BUILD) M=$(PWD) modules

clean:
	make -C $(KERNEL_BUILD) M=$(PWD) clean

install:
	make -C $(KERNEL_BUILD) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
