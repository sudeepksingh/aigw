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
PKG_VERSION:=0.7.0
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
  DEPENDS:=+libubox +libubus +libuci +libblobmsg-json +libnetfilter-conntrack +kmod-nf-conntrack-netlink +kmod-nft-bridge
endef

define Package/aigw/description
  AIGW turns the router into a gateway that understands the home.
  This package contains the core: agd (brain), agdns (DNS sensor),
  agctl (command line) and the nftables integration.
endef

define Package/aigw/conffiles
/etc/config/aigw
endef

define Package/aigw-mqtt
  SECTION:=net
  CATEGORY:=Network
  TITLE:=AIGW plug-in: MQTT connector with Home Assistant discovery
  DEPENDS:=+aigw +libubox +libblobmsg-json
endef

define Package/aigw-mqtt/description
  An AIGW plug-in: telemetry from MQTT becomes context for the brain, and
  devices appear in Home Assistant with presence and a Quarantine switch.
  agd finds it by itself once installed; it waits until an MQTT broker
  answers (default 127.0.0.1:1883, or: uci set aigw.mqtt=plugin;
  uci set aigw.mqtt.broker=<address>; uci commit aigw).
endef

CMAKE_OPTIONS += -DAIGW_TESTS=OFF

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(AIGW_SRC)/CMakeLists.txt $(AIGW_SRC)/include $(AIGW_SRC)/core \
		$(AIGW_SRC)/agd $(AIGW_SRC)/agdns $(AIGW_SRC)/agctl $(AIGW_SRC)/plugins \
		$(AIGW_SRC)/scripts $(AIGW_SRC)/tests $(AIGW_SRC)/models $(AIGW_SRC)/LICENSE \
		$(PKG_BUILD_DIR)/
endef

define Package/aigw/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/bin $(1)/etc/init.d $(1)/etc/config $(1)/etc/uci-defaults \
		$(1)/etc/hotplug.d/dhcp $(1)/usr/share/aigw/models $(1)/usr/share/aigw/plugins/iot-guard \
		$(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) $(PKG_INSTALL_DIR)/usr/share/aigw/models/domain-risk.agm $(1)/usr/share/aigw/models/
	$(INSTALL_DATA) $(PKG_INSTALL_DIR)/usr/share/aigw/plugins/iot-guard/manifest.json \
		$(1)/usr/share/aigw/plugins/iot-guard/
	# per-device Wi-Fi keys, your plug-ins and device history survive a firmware upgrade
	echo /etc/aigw/ > $(1)/lib/upgrade/keep.d/aigw
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

define Package/aigw-mqtt/install
	$(INSTALL_DIR) $(1)/usr/share/aigw/plugins/mqtt
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/share/aigw/plugins/mqtt/ag-mqtt $(1)/usr/share/aigw/plugins/mqtt/
	$(INSTALL_DATA) $(PKG_INSTALL_DIR)/usr/share/aigw/plugins/mqtt/manifest.json $(1)/usr/share/aigw/plugins/mqtt/
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
$(eval $(call BuildPackage,aigw-mqtt))
