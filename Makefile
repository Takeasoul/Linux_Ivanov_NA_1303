KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
KBUILD_CC ?= $(if $(shell command -v gcc-14 2>/dev/null),gcc-14,gcc)

obj-m += simplefs.o
simplefs-y := src/simplefs.o

CFLAGS_simplefs.o += -I$(PWD)/include/uapi
ccflags-y += -I$(PWD)/include/uapi

.PHONY: all module user clean

all: module user

module:
	$(MAKE) -C $(KDIR) M=$(PWD) CC=$(KBUILD_CC) modules

user: tools/simplefsctl

tools/simplefsctl: tools/simplefsctl.c include/uapi/linux/simplefs_ioctl.h
	$(CC) -Wall -Wextra -Werror -O2 -Iinclude/uapi -o $@ tools/simplefsctl.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) tools/simplefsctl
