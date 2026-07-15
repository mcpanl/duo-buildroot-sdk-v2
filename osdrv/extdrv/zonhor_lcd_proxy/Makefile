PWD := $(shell pwd)

ifneq ($(KERNELRELEASE),)

ccflags-y += -I$(src)
ccflags-y += -I$(src)/../../include
ccflags-y += -I$(src)/../../interdrv/rtos_cmdqu/
ccflags-y += -Wall -Wextra -Wno-unused-parameter

cv181x_zonhor_lcd_proxy-objs = zonhor_lcd_proxy.o
obj-m += cv181x_zonhor_lcd_proxy.o

KBUILD_EXTRA_SYMBOLS += $(src)/../../interdrv/rtos_cmdqu/Module.symvers

else

all:
	$(MAKE) ARCH=$(ARCH) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	rm -f *.o .*.cmd modules.order Module.symvers *.ko *.mod.c
	rm -rf .tmp_versions

endif
