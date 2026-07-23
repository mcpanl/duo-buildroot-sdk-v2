DRV_PATH = $(BUILDDIR)/../../driver

# Minimal: robot MMIO only (LED timing uses RTC DW timer one-shot in main.c)
c_srcs += \
	$(DRV_PATH)/cvi_reg.c \

inc_dir += \
	$(DRV_PATH) \
	$(BUILDDIR)/../../include
