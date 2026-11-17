CIRCUD_PACKAGE_MAKEFILE:=$(realpath $(lastword $(MAKEFILE_LIST)))

include $(TOPDIR)/rules.mk

PKG_NAME:=circud
PKG_VERSION:=0.1.0
PKG_RELEASE:=2

PKG_MAINTAINER:=circud contributors
PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_PARALLEL:=1
PKG_BUILD_FLAGS:=no-mips16
PKG_INSTALL:=1

# The SDK evaluates package Makefiles with CURDIR set to the SDK root. Resolve
# this feed Makefile first so a src-link feed still locates the circud checkout
# rather than the SDK tree.
CIRCUD_SOURCE_DIR:=$(abspath $(dir $(CIRCUD_PACKAGE_MAKEFILE))/../../..)
CIRCUJS_SOURCE_DIR:=$(CIRCUD_SOURCE_DIR)/.ci/circu.js
LLHTTP_SOURCE_DIR:=$(CIRCUD_SOURCE_DIR)/.ci/llhttp

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/circud
  SECTION:=admin
  CATEGORY:=Administration
  TITLE:=Small management framework built on circu.js
  URL:=https://github.com/imzlh/circud
  DEPENDS:=+libstdcpp +libatomic +boost-json +libopenssl +libsqlite3 \
	+zlib +minizip +libcurl +libexpat +libffi
endef

define Package/circud/description
 circud provides package-isolated QuickJS runtimes, lifecycle management,
 status snapshots and a small HTTP management surface.
endef

define Package/circud/conffiles
/etc/circud/config.json
endef

define Build/Prepare
	rm -rf $(PKG_BUILD_DIR)
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) $(CIRCUD_SOURCE_DIR)/CMakeLists.txt $(PKG_BUILD_DIR)/
	$(CP) $(CIRCUD_SOURCE_DIR)/config.example.json $(PKG_BUILD_DIR)/
	$(CP) $(CIRCUD_SOURCE_DIR)/LICENSE $(PKG_BUILD_DIR)/
	$(CP) $(CIRCUD_SOURCE_DIR)/include $(PKG_BUILD_DIR)/
	$(CP) $(CIRCUD_SOURCE_DIR)/packaging $(PKG_BUILD_DIR)/
	$(CP) $(CIRCUD_SOURCE_DIR)/public $(PKG_BUILD_DIR)/
	$(CP) $(CIRCUD_SOURCE_DIR)/src $(PKG_BUILD_DIR)/
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/circu.js $(PKG_BUILD_DIR)/llhttp
	tar --exclude-vcs -C $(CIRCUJS_SOURCE_DIR) -cf - . | \
		tar -C $(PKG_BUILD_DIR)/circu.js -xf -
	tar --exclude-vcs -C $(LLHTTP_SOURCE_DIR) -cf - . | \
		tar -C $(PKG_BUILD_DIR)/llhttp -xf -
endef

CMAKE_OPTIONS += \
	-DBUILD_TESTING=OFF \
	-DCIRCUD_ENABLE_ASAN=OFF \
	-DCIRCUD_ENABLE_STRICT_WARNINGS=OFF \
	-DCIRCUD_LIBCJS_SOURCE=$(PKG_BUILD_DIR)/circu.js \
	-DFETCHCONTENT_SOURCE_DIR_LLHTTP=$(PKG_BUILD_DIR)/llhttp

define Package/circud/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/circud $(1)/usr/bin/circud
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/packaging/circud.init \
		$(1)/etc/init.d/circud
	$(INSTALL_DIR) $(1)/etc/circud
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/packaging/openwrt/circud/config.json \
		$(1)/etc/circud/config.json
	$(INSTALL_DIR) $(1)/usr/share/circud
	$(INSTALL_DATA) $(PKG_INSTALL_DIR)/usr/share/circud/circud.example.json \
		$(1)/usr/share/circud/circud.example.json
	$(INSTALL_DIR) $(1)/usr/share/circud/public
	$(CP) $(PKG_INSTALL_DIR)/usr/share/circud/public/. \
		$(1)/usr/share/circud/public/
endef

$(eval $(call BuildPackage,circud))
