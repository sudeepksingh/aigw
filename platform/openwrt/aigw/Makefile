# OpenWRT package for AIGW. Builds from the repository this file lives in.
#
# In an OpenWRT SDK or buildroot, add to feeds.conf:
#   src-link aigw /path/to/aigw/platform/openwrt
# or
#   src-git aigw https://github.com/sudeepksingh/aigw.git;main
# then: ./scripts/feeds update aigw && ./scripts/feeds install aigw
#       make package/aigw/compile
#
# Or just run platform/openwrt/build-sdk.sh, which does all of this.

include $(TOPDIR)/rules.mk

PKG_NAME:=aigw
PKG_VERSION:=0.2.0
PKG_RELEASE:=1

PKG_LICENSE:=Apache-2.0
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=AIGW contributors

PKG_BUILD_PARALLEL:=1
PKG_INSTALL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

# repository root (this Makefile is in <repo>/platform/openwrt/aigw/)
AIGW_SRC:=$(abspath $(CURDIR)/../../..)

define Package/aigw
  SECTION:=net
  CATEGORY:=Network
  TITLE:=AIGW AI-native gateway core (agd, agdns, agctl)
  # nft and its kernel modules come with firewall4, the default firewall
  # since OpenWRT 22.03. The flow sensor needs conntrack netlink.
  DEPENDS:=+libubox +libubus +libuci +libblobmsg-json +libnetfilter-conntrack +kmod-nf-conntrack-netlink
endef

define Package/aigw/description
  AIGW turns the router into a gateway that understands the home.
  This package contains the core: agd (brain), agdns (DNS sensor),
  agctl (command line) and the nftables integration.
endef

define Package/aigw/conffiles
/etc/config/aigw
endef

CMAKE_OPTIONS += -DAIGW_TESTS=OFF

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(AIGW_SRC)/CMakeLists.txt $(AIGW_SRC)/include $(AIGW_SRC)/core \
		$(AIGW_SRC)/agd $(AIGW_SRC)/agdns $(AIGW_SRC)/agctl \
		$(AIGW_SRC)/scripts $(AIGW_SRC)/tests $(AIGW_SRC)/LICENSE \
		$(PKG_BUILD_DIR)/
endef

define Package/aigw/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/bin $(1)/etc/init.d $(1)/etc/config $(1)/etc/uci-defaults \
		$(1)/etc/hotplug.d/dhcp
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/agd $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/agdns $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-nft $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-dhcp-event $(1)/usr/sbin/
	$(INSTALL_DATA) ./files/aigw.hotplug-dhcp $(1)/etc/hotplug.d/dhcp/90-aigw
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/agctl $(1)/usr/bin/
	$(INSTALL_BIN) ./files/aigw.init $(1)/etc/init.d/aigw
	$(INSTALL_CONF) $(AIGW_SRC)/config/aigw $(1)/etc/config/aigw
	$(INSTALL_BIN) ./files/aigw.uci-defaults $(1)/etc/uci-defaults/90-aigw
endef

define Package/aigw/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/aigw stop 2>/dev/null
/etc/init.d/aigw disable 2>/dev/null
if uci -q get firewall.aigw >/dev/null; then
	uci -q delete firewall.aigw
	uci commit firewall
	fw4 reload 2>/dev/null
fi
exit 0
endef

$(eval $(call BuildPackage,aigw))
