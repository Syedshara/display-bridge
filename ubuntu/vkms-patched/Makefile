# SPDX-License-Identifier: GPL-2.0-only
# Out-of-tree build for patched vkms module (adds 2880x1800 mode)

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Kbuild targets (used by kernel build system when called via M=)
vkms-y := \
	vkms_drv.o \
	vkms_plane.o \
	vkms_output.o \
	vkms_formats.o \
	vkms_crtc.o \
	vkms_composer.o \
	vkms_writeback.o \
	vkms_connector.o \
	vkms_config.o

obj-m += vkms.o
