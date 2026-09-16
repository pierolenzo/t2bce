modname := t2bce_ave
obj-m += $(modname).o

t2bce_ave-objs := ave.o protocol.o encoder.o video.o

# The DKMS stack stages both modules side by side; standalone repository builds
# use the common core at its repository location.
T2BCE_AVE_DIR := $(if $(src),$(abspath $(src)),$(CURDIR))
T2BCE_CORE_SRC ?= $(if $(wildcard $(T2BCE_AVE_DIR)/../t2bce_core),../t2bce_core,../../../../modules/t2bce_core)
MY_CFLAGS += -DWITHOUT_NVME_PATCH
ccflags-y += ${MY_CFLAGS}
ccflags-y += -I$(src) -I$(src)/$(T2BCE_CORE_SRC)/include
CC += ${MY_CFLAGS}

KVERSION := $(KERNELRELEASE)
ifeq ($(origin KERNELRELEASE), undefined)
KVERSION := $(shell uname -r)
endif

KDIR := /lib/modules/$(KVERSION)/build
MODDEST ?= /lib/modules/$(KVERSION)/updates
PWD := $(shell pwd)
T2BCE_CORE_SYMBOLS := $(abspath $(T2BCE_AVE_DIR)/$(T2BCE_CORE_SRC)/Module.symvers)

ifneq ($(wildcard $(T2BCE_CORE_SYMBOLS)),)
KBUILD_EXTRA_SYMBOLS += $(T2BCE_CORE_SYMBOLS)
endif

.PHONY: all clean install

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	@test -f t2bce_ave.ko || { echo "t2bce_ave.ko not found; run make first"; exit 1; }
	install -d -o root -g root -m 0755 $(MODDEST)
	install -o root -g root -m 0644 t2bce_ave.ko $(MODDEST)/t2bce_ave.ko
	depmod -a $(KVERSION)
