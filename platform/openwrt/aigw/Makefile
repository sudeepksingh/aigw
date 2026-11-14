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
PKG_VERSION:=0.27.4
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
  TITLE:=AIGW OS core: the AI-powered home gateway (agd, agdns, agctl)
  # nft and its kernel modules come with firewall4, the default firewall
  # since OpenWRT 22.03. The flow sensor needs conntrack netlink.
  DEPENDS:=+libubox +libubus +libuci +libblobmsg-json +libnetfilter-conntrack +kmod-nf-conntrack-netlink +kmod-nft-bridge +kmod-nfnetlink-log +kmod-nft-offload
endef

define Package/aigw/description
  AIGW OS, the AI-powered operating system for home gateways, turns the
  router into a gateway that understands the home.
  This package contains the core: agd (brain), agdns (DNS sensor),
  agctl (command line) and the nftables integration.
endef

define Package/aigw/conffiles
/etc/config/aigw
endef

define Package/aigw-mqtt
  SECTION:=net
  CATEGORY:=Network
  TITLE:=AIGW OS plug-in: MQTT connector with Home Assistant discovery
  DEPENDS:=+aigw +libubox +libblobmsg-json
endef

define Package/aigw-mqtt/description
  An AIGW plug-in: telemetry from MQTT becomes context for the brain, and
  devices appear in Home Assistant with presence and a Quarantine switch.
  agd finds it by itself once installed; it waits until an MQTT broker
  answers (default 127.0.0.1:1883, or: uci set aigw.mqtt=plugin;
  uci set aigw.mqtt.broker=<address>; uci commit aigw).
endef

define Package/aigw-bridge
  SECTION:=net
  CATEGORY:=Network
  TITLE:=AIGW OS plug-in: bridge to cortex (fleet history, signed model updates)
  # curl (TLS, certificate checks) is needed at run time; the plug-in waits
  # until it is installed. It is not a build dependency, so the package
  # builds from the base feed alone.
  DEPENDS:=+aigw +libubox +libblobmsg-json +libuci
endef

define Package/aigw-bridge/description
  An AIGW plug-in that links the gateway to cortex, AIGW's optional
  services outside: it reports state (never traffic), fetches signed model
  updates for agd to check and hot-load, and brings suggestions from the AI
  assistant, which wait for your approval. Needs curl (opkg install curl).
  Set it up with: agctl bridge <cortex URL>; agctl bridge token.
endef

define Package/aigw-remote
  SECTION:=net
  CATEGORY:=Network
  TITLE:=AIGW OS remote access over WireGuard (peers with QR codes)
  DEPENDS:=+aigw +wireguard-tools +kmod-wireguard
  PKGARCH:=all
endef

define Package/aigw-remote/description
  Remote access into the home over WireGuard, managed by AIGW: one peer per
  phone, set up by scanning a QR code; lookups from the phone go through
  AIGW wherever it is. Turn it on with: agctl remote on <public address or
  DDNS name>; then: agctl remote add <name>. Adds a firewall zone and a rule
  for the UDP port (51820) the first time.
endef

define Package/aigw-cwmp
  SECTION:=net
  CATEGORY:=Network
  TITLE:=AIGW OS management by TR-069 / CWMP (data model for icwmp)
  DEPENDS:=+aigw +icwmp +libubox +libubus +libuci
endef

define Package/aigw-cwmp/description
  Lets an ISP's ACS manage AIGW OS over TR-069: devices, alerts, decisions
  (approve, reject, undo), quarantine and block, the same data model USP
  controllers get. aigw-cwmp-dm serves it to the unmodified icwmp client.
  Off until you turn it on: uci set aigw.mgmt.enabled=1; uci set
  aigw.cwmp.url=<ACS URL>; uci commit aigw; /etc/init.d/aigw-cwmp start.
  What an ACS may do is set by aigw.mgmt.actions and aigw.mgmt.alert_details;
  AIGW OS's guardrails always apply.
endef

define Package/luci-app-aigw
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI pages for AIGW OS
  # needs LuCI itself (luci-base); not a build dependency, so the package
  # builds from the base feed alone
  DEPENDS:=+aigw +rpcd
  PKGARCH:=all
endef

define Package/luci-app-aigw/description
  AIGW OS in the router's web interface (menu "AIGW OS"): overview, devices with
  quarantine / priority / zone, alerts and decisions with approve, reject
  and undo, per-device Wi-Fi keys and remote access with QR codes,
  plug-ins and models.
endef

CMAKE_OPTIONS += -DAIGW_TESTS=OFF

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(AIGW_SRC)/CMakeLists.txt $(AIGW_SRC)/include $(AIGW_SRC)/core \
		$(AIGW_SRC)/agd $(AIGW_SRC)/agdns $(AIGW_SRC)/agctl $(AIGW_SRC)/plugins $(AIGW_SRC)/cwmp \
		$(AIGW_SRC)/scripts $(AIGW_SRC)/tests $(AIGW_SRC)/models $(AIGW_SRC)/LICENSE \
		$(PKG_BUILD_DIR)/
endef

# a router maker's NPU back-end package builds against this header
# (DEPENDS:=+aigw, #include <ag/backend.h>) and installs its library into
# /usr/lib/aigw/backends
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/ag
	$(CP) $(AIGW_SRC)/include/ag/backend.h $(1)/usr/include/ag/
endef

define Package/aigw/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/bin $(1)/etc/init.d $(1)/etc/config $(1)/etc/uci-defaults \
		$(1)/etc/hotplug.d/dhcp $(1)/usr/share/aigw/models $(1)/usr/share/aigw/plugins/iot-guard \
		$(1)/lib/upgrade/keep.d $(1)/usr/lib/aigw/backends $(1)/usr/libexec/aigw
	$(INSTALL_DATA) $(PKG_INSTALL_DIR)/usr/share/aigw/models/domain-risk.agm $(1)/usr/share/aigw/models/
	# platform handlers: Wi-Fi settings and firmware, as a management system changes them
	$(INSTALL_BIN) ./files/wifi-config $(1)/usr/libexec/aigw/wifi-config
	$(INSTALL_BIN) ./files/firmware $(1)/usr/libexec/aigw/firmware
	$(INSTALL_DATA) $(PKG_INSTALL_DIR)/usr/share/aigw/plugins/iot-guard/manifest.json \
		$(1)/usr/share/aigw/plugins/iot-guard/
	# per-device Wi-Fi keys, your plug-ins, trusted keys, model updates and
	# device history survive a firmware upgrade
	echo /etc/aigw/ > $(1)/lib/upgrade/keep.d/aigw
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/agd $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/agdns $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-nft $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-dhcp-event $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-selftest $(1)/usr/sbin/
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

define Package/aigw-bridge/install
	$(INSTALL_DIR) $(1)/usr/share/aigw/plugins/bridge
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/share/aigw/plugins/bridge/ag-bridge $(1)/usr/share/aigw/plugins/bridge/
	$(INSTALL_DATA) $(PKG_INSTALL_DIR)/usr/share/aigw/plugins/bridge/manifest.json $(1)/usr/share/aigw/plugins/bridge/
endef

define Package/aigw-remote/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-remote $(1)/usr/sbin/
endef

define Package/aigw-remote/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
# take the tunnel, the open port and the firewall zone away with the package
agctl remote off >/dev/null 2>&1 || /usr/sbin/aigw-remote down agwg >/dev/null 2>&1
exit 0
endef

define Package/aigw-cwmp/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-cwmp-dm $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-cwmp-run $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/aigw-cwmp.init $(1)/etc/init.d/aigw-cwmp
	# icwmp's FactoryReset RPC runs "defaultreset" (iopsys has one; OpenWRT doesn't)
	$(INSTALL_DIR) $(1)/usr/libexec/aigw
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/libexec/aigw/defaultreset $(1)/usr/libexec/aigw/
endef

define Package/aigw-cwmp/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/aigw-cwmp stop 2>/dev/null
/etc/init.d/aigw-cwmp disable 2>/dev/null
if uci -q get firewall.aigw_cwmp >/dev/null; then
	uci -q delete firewall.aigw_cwmp
	uci commit firewall
	fw4 reload 2>/dev/null
fi
exit 0
endef

LUCI_SRC:=$(AIGW_SRC)/platform/openwrt/luci-app-aigw

define Package/luci-app-aigw/install
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d $(1)/usr/share/rpcd/acl.d \
		$(1)/www/luci-static/resources/view/aigw $(1)/www/luci-static/resources/aigw
	$(INSTALL_DATA) $(LUCI_SRC)/root/usr/share/luci/menu.d/luci-app-aigw.json $(1)/usr/share/luci/menu.d/
	$(INSTALL_DATA) $(LUCI_SRC)/root/usr/share/rpcd/acl.d/luci-app-aigw.json $(1)/usr/share/rpcd/acl.d/
	$(INSTALL_DATA) $(LUCI_SRC)/htdocs/luci-static/resources/view/aigw/*.js $(1)/www/luci-static/resources/view/aigw/
	$(INSTALL_DATA) $(LUCI_SRC)/htdocs/luci-static/resources/aigw/logo-mark.svg $(1)/www/luci-static/resources/aigw/
endef

define Package/luci-app-aigw/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
rm -f /tmp/luci-indexcache* /tmp/luci-modulecache/* 2>/dev/null
/etc/init.d/rpcd reload 2>/dev/null
exit 0
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
$(eval $(call BuildPackage,aigw-bridge))
$(eval $(call BuildPackage,aigw-remote))
$(eval $(call BuildPackage,aigw-cwmp))
$(eval $(call BuildPackage,luci-app-aigw))
