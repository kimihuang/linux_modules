# SPDX-License-Identifier: GPL-2.0-only

all:
	make -C $(KERNEL_SRC) O=$(KERNEL_SRC)/output M=$(PWD) modules

clean:
	make -C $(KERNEL_SRC) O=$(KERNEL_SRC)/output M=$(PWD) clean

install:
	mkdir -p $(INSTALL_MOD_PATH)
#cp src/my_device.ko $(INSTALL_MOD_PATH)
	depmod -a

.PHONY: all clean install
