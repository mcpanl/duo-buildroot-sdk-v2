PWD := $(shell pwd)

ifneq ($(KERNELRELEASE),)

ccflags-y += -Wall -Wextra -Wno-unused-parameter

cv181x_zonhor_lcd_bl-objs = zonhor_lcd_bl.o
obj-m += cv181x_zonhor_lcd_bl.o

else

all:
	$(MAKE) ARCH=$(ARCH) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	rm -f *.o .*.cmd modules.order Module.symvers *.ko *.mod.c
	rm -rf .tmp_versions

endif
