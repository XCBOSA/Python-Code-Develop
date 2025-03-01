MYTOOL_VERSION = 1.0
MYTOOL_SITE = ./package/0xcvmkitosboot/src
MYTOOL_SITE_METHOD = local

define MYTOOL_BUILD_CMDS
	$(MAKE) CC="$(TARGET_CC)" LD="$(TARGET_LD)" -C $(@D)
endef

define MYTOOL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/xcvmkitosroot $(TARGET_DIR)/sbin/xcvmkitosroot
endef