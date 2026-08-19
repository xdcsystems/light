include $(TOPDIR)/rules.mk

PKG_NAME:=light_control
PKG_VERSION:=1.0.23
PKG_RELEASE:=1

PKG_LICENSE:=MIT
PKG_MAINTAINER:=xdcsystems

# Absolute path to the CMake project. Override with LIGHT_CONTROL_SRC=...
PKG_SOURCE_PATH:=$(if $(LIGHT_CONTROL_SRC),$(LIGHT_CONTROL_SRC),/mnt/c/Dev/light_control/light/light_control)
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

# DISABLE_CMAKE_INSTALL:=1
# CMAKE_INSTALL_PREFIX:=/usr

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	cp -fpR $(PKG_SOURCE_PATH)/CMakeLists.txt $(PKG_BUILD_DIR)/
	cp -fpR $(PKG_SOURCE_PATH)/src $(PKG_BUILD_DIR)/
	cp -fpR $(PKG_SOURCE_PATH)/include $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(CMAKE_MAKE)
endef

define Package/light_control
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Light Control Daemon
  DEPENDS:=+libuci +libubox +libubus +libstdcpp +libatomic +libc +libpthread
endef

define Package/light_control/description
  Daemon for light control and stats on OpenWrt.
endef

CMAKE_OPTIONS += \
	-DCMAKE_BUILD_TYPE=Release \
	-DENABLE_UBUS=ON \
	-DOPENWRT_BUILD=ON \
	-DCMAKE_CXX_STANDARD=17 \
	-DCMAKE_EXE_LINKER_FLAGS="-znow -zrelro"

define Package/light_control/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(CP) $(PKG_BUILD_DIR)/light_control $(1)/usr/bin/

	$(INSTALL_DIR) $(1)/etc/config
	$(CP) ./files/light_control.config $(1)/etc/config/light_control

	$(INSTALL_DIR) $(1)/etc/init.d
	$(CP) ./files/light_control.init $(1)/etc/init.d/light_control
	
	chmod 0755 $(1)/etc/init.d/light_control
endef

$(eval $(call BuildPackage,light_control))
