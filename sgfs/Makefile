#
# Makefile for the linux r2nvmm routines.
#
# 2016.09.20
# http://stackoverflow.com/questions/3707517/make-file-echo-displaying-path-string
# http://stackoverflow.com/questions/16467718/how-to-print-out-a-variable-in-makefile
#$(info $$CONFIG_MMU is [${CONFIG_MMU}])

obj-m += r2nvmm.o
KERNELDIR ?= /lib/modules/$(shell uname -r)/bulid
PWD := $(shell pwd)

#file-mmu-y := file-nommu.o
#file-mmu-$(CONFIG_MMU) := file-mmu.o
#r2nvmm-objs += inode.o $(file-mmu-y)

r2nvmm-objs += inode.o file.o super.o

all:
#	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	make -C /lib/modules/$(shell uname -r)/build M=`pwd`
clean:
#	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	make -C /lib/modules/$(shell uname -r)/build M=`pwd` clean
