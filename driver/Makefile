# vgpu - out-of-tree kernel module
#
# Built against the same source tree the guest boots
# (docs/adr/0003), so kernel and module always match.

obj-m := vgpu.o
vgpu-y := vgpu_drv.o vgpu_engine.o vgpu_compute.o

KDIR ?= $(HOME)/linux
PWD := $(shell pwd)

# modpost needs the vmlinux exported symbols to resolve imports. This tree
# has no top-level Module.symvers, so point at the existing vmlinux.symvers.
KBUILD_EXTRA_SYMBOLS ?= $(KDIR)/vmlinux.symvers

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	# the module is picked up by scripts/20-initramfs.sh

.PHONY: all clean install
