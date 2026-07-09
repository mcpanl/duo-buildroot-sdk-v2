################################################################################
#
# axp2101ctl
#
################################################################################

AXP2101CTL_VERSION = 1.0
AXP2101CTL_SITE = $(TOPDIR)/../device/zonhor-sg2000-glibc-arm64-emmc/axp2101ctl
AXP2101CTL_SITE_METHOD = local
AXP2101CTL_LICENSE = GPL-2.0

define AXP2101CTL_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/axp2101ctl $(@D)/axp2101ctl.c
endef

define AXP2101CTL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/axp2101ctl $(TARGET_DIR)/usr/bin/axp2101ctl
endef

$(eval $(generic-package))
