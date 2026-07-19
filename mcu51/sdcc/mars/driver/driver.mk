DRV_PATH = $(BUILDDIR)/../../driver

# Minimal: robot MMIO only (delay is software loop in main.c)
c_srcs += \
	$(DRV_PATH)/cvi_reg.c \

inc_dir += \
	$(DRV_PATH) \
	$(BUILDDIR)/../../include
