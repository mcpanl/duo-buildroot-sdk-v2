################################################################################
#
# btop
#
################################################################################

BTOP_VERSION = 1.2.2
BTOP_SITE = https://github.com/aristocratos/btop/archive/refs/tags
BTOP_SOURCE = v$(BTOP_VERSION).tar.gz

BTOP_LICENSE = Apache-2.0
BTOP_LICENSE_FILES = LICENSE

# 使用 Buildroot 的编译器
BTOP_MAKE_ENV = \
    CXX="$(TARGET_CXX)" \
    CXXFLAGS="$(TARGET_CXXFLAGS) -static -fno-stack-protector -U_FORTIFY_SOURCE" \
    LDFLAGS="$(TARGET_LDFLAGS) -static -fno-stack-protector"

define BTOP_BUILD_CMDS
	$(MAKE) -C $(@D) $(BTOP_MAKE_ENV)
endef

define BTOP_INSTALL_TARGET_CMDS
	$(INSTALL) -D $(@D)/bin/btop $(TARGET_DIR)/usr/bin/btop
endef

$(eval $(generic-package))

