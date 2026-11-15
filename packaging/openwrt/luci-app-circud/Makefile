LUCI_APP_CIRCUD_MAKEFILE:=$(realpath $(lastword $(MAKEFILE_LIST)))
CIRCUD_SOURCE_DIR:=$(abspath $(dir $(LUCI_APP_CIRCUD_MAKEFILE))/../../..)

# gh-action-sdk builds the feed at /feed in an isolated SDK container. Run the
# native LuCI/rpcd tests there while the matching target ucode runtime and
# socket module are still available; the composite action exports only bin/
# after the container exits. Other architectures and ordinary feed builds are
# left untouched.
define Build/Prepare/luci-app-circud
	if [ "$(FEEDNAME)" = "circud" ] && \
	   [ -x "$(STAGING_DIR_ROOT)/lib/ld-musl-x86_64.so.1" ] && \
	   [ -x "$(CIRCUD_SOURCE_DIR)/tests/run_luci_target_tests.sh" ]; then \
		"$(CIRCUD_SOURCE_DIR)/tests/run_luci_target_tests.sh" \
			"$(STAGING_DIR_ROOT)" "$(CIRCUD_SOURCE_DIR)"; \
	fi
endef

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-circud
PKG_VERSION:=0.1.0
PKG_RELEASE:=5

LUCI_TITLE:=LuCI support for circud
LUCI_DEPENDS:=+luci-base +circud +ucode-mod-socket +script-utils
LUCI_PKGARCH:=all
LUCI_URL:=https://github.com/imzlh/circud

PKG_LICENSE:=MIT
PKG_MAINTAINER:=circud contributors

# The package lives in circud's own feed, not below feeds/luci/applications.
# Resolve the SDK's installed LuCI feed explicitly instead of using ../../luci.mk.
include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
