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
PKG_VERSION:=0.54.0
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
  # since OpenWRT 22.03. The flow sensor needs conntrack netlink. Smart
  # queueing (aigw-sqm): tc, cake, and ifb for the download; calls and games
  # first: flower filters (skbedit comes with kmod-sched-core).
  DEPENDS:=+libubox +libubus +libuci +libblobmsg-json +libnetfilter-conntrack +kmod-nf-conntrack-netlink +kmod-nft-bridge +kmod-nfnetlink-log +kmod-nft-offload +tc-tiny +kmod-sched-core +kmod-sched-cake +kmod-ifb +kmod-sched-flower
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
  DEPENDS:=+aigw +libubox +libblobmsg-json @AIGW_FEATURE_PLUGINS
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
  DEPENDS:=+aigw +libubox +libblobmsg-json +libuci @AIGW_FEATURE_PLUGINS
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
  DEPENDS:=+aigw +wireguard-tools +kmod-wireguard @AIGW_FEATURE_REMOTE
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

define Package/aigw-demo
  SECTION:=net
  CATEGORY:=Network
  TITLE:=AIGW OS demo image only: a simulated home and fleet (QEMU virtual machines only)
  # only in the demo image (platform/openwrt/build-demo-image.sh), which adds
  # python3 for cortex and curl for the bridge; never released as a package
  DEPENDS:=+aigw +aigw-bridge +luci-app-aigw @AIGW_PROFILE_MAX
endef

define Package/aigw-demo/description
  The demo image's simulated home and fleet (docs/try-it.md): three days of a
  home's timeline with a slow evening, a call and an upgrade, and cortex with
  24 simulated homes and its dashboard. It turns itself on only in a QEMU
  virtual machine, and labels everything as demo data. Not for routers.
endef

# the features (cmake/features.cmake, docs/features.md): make menuconfig, Network > aigw
AIGW_FEATURES:=WIFI_HEAL MESH COVERAGE TR143 CARE UPGRADE WHY OFFERS SQM CALLS BRIEF PROACTIVE AREA AREAPREDICT FED FLEETLEARN PLUGINS SNI METER FASTPATH REMOTE KEYS PROVEN
PKG_CONFIG_DEPENDS:=$(foreach f,$(AIGW_FEATURES),CONFIG_AIGW_FEATURE_$(f))

define Package/aigw/config
  menu "AIGW OS features"
  choice
    prompt "Profile"
    default AIGW_PROFILE_FULL
    help
      Which features AIGW OS is built with (docs/features.md). Each profile
      has all of the one before: Minimal < Small < Full < Max. A feature that
      is not built in takes no flash, and its commands say so.

      Full (the default) fits 1024 KB of flash; Max has every feature and
      fits 2048 KB, for routers with 16 MB of flash or more. A new feature
      comes in Max first, and joins Full only when Full still fits its
      1024 KB with headroom on every architecture. aigw-selftest checks the
      installed profile's budget.
  config AIGW_PROFILE_MAX
    bool "Max: every feature (16 MB of flash or more; 2048 KB budget)"
  config AIGW_PROFILE_FULL
    bool "Full: the features most routers have room for (1024 KB budget)"
  config AIGW_PROFILE_SMALL
    bool "Small: for 8 or 16 MB of flash (what an ISP needs most)"
  config AIGW_PROFILE_MINIMAL
    bool "Minimal: the core only"
  config AIGW_PROFILE_CUSTOM
    bool "Custom: pick the features below"
  endchoice
  config AIGW_FEATURE_WIFI_HEAL
    bool "Self-Healing Wi-Fi, band steering, channel healing, neighbour-aware channels" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM || AIGW_PROFILE_SMALL
  config AIGW_FEATURE_MESH
    bool "Smarter mesh Wi-Fi over an EasyMesh controller" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_COVERAGE
    bool "Coverage placement advice" if AIGW_PROFILE_CUSTOM
    depends on AIGW_FEATURE_WHY
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_TR143
    bool "TR-143 diagnostics (IPPing, TraceRoute, Download, Upload, NSLookup)" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM || AIGW_PROFILE_SMALL
  config AIGW_FEATURE_CARE
    bool "Proactive care, checks, early warning, the experience score, the speed test" if AIGW_PROFILE_CUSTOM
    depends on AIGW_FEATURE_TR143
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM || AIGW_PROFILE_SMALL
  config AIGW_FEATURE_UPGRADE
    bool "Firmware upgrades that check themselves and roll back" if AIGW_PROFILE_CUSTOM
    depends on AIGW_FEATURE_CARE
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM || AIGW_PROFILE_SMALL
  config AIGW_FEATURE_WHY
    bool "Why was it slow, the home's timeline" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM || AIGW_PROFILE_SMALL
  config AIGW_FEATURE_OFFERS
    bool "Signals for the ISP (a faster plan, mesh Wi-Fi, and more)" if AIGW_PROFILE_CUSTOM
    depends on AIGW_FEATURE_WHY && AIGW_FEATURE_CARE
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_SQM
    bool "Self-tuning smart queueing" if AIGW_PROFILE_CUSTOM
    depends on AIGW_FEATURE_WHY
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM || AIGW_PROFILE_SMALL
  config AIGW_FEATURE_CALLS
    bool "Calls and games first" if AIGW_PROFILE_CUSTOM
    depends on AIGW_FEATURE_SQM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_BRIEF
    bool "The help-desk brief" if AIGW_PROFILE_CUSTOM
    depends on AIGW_FEATURE_CARE && AIGW_FEATURE_WHY
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM || AIGW_PROFILE_SMALL
  config AIGW_FEATURE_PROACTIVE
    bool "Proactive care before the call, tickets to the ISP" if AIGW_PROFILE_CUSTOM
    depends on AIGW_FEATURE_BRIEF
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM || AIGW_PROFILE_SMALL
  config AIGW_FEATURE_AREA
    bool "Area outages, fixes held while many homes fail at once" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_AREAPREDICT
    bool "Area fault prediction: line health trends, the shared plant found before it fails" if AIGW_PROFILE_CUSTOM
    depends on AIGW_FEATURE_CARE
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_CUSTOM
    help
      Each of the line's numbers day by day (sync rate, noise margin, optical
      level, FEC and CRC errors, retrains and drops, the next hop's delay and
      loss, the speed test): which way it goes and when it would be a fault
      (agctl line, Device.X_AIGW_.LineHealth., LineDegrading!). The ISP's
      cortex compares the homes of one cabinet or OLT and predicts a failing
      shared plant days before the first calls: one ticket, not one per home.
      No faster plan is offered on a degrading line. It only watches and tells.
      Needs CARE. In the max profile only (since v0.54.0): full has no room
      for it within 1024 KB. Adds 20 KB of installed flash on x86-64
      (aarch64 28, mipsel 20).
      See docs/area-prediction.md.
  config AIGW_FEATURE_FED
    bool "Federated learning" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_FLEETLEARN
    bool "Fleet learning: every home teaches the next (about 30 KB)" if AIGW_PROFILE_CUSTOM
    depends on AIGW_FEATURE_FED
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_CUSTOM
    help
      How fixes went in other homes (counts per fault, fix and coarse
      context, k homes or more, signed by cortex) ranks the fixes AIGW
      may use here, blended with this home's own outcomes; a new gateway
      warns from its first day against the fleet's normal. It never acts
      by itself: act.mode and the policies decide. Sharing this home's
      summary is opt-in (fleet.share). Needs federated learning (FED);
      uses the bridge (PLUGINS) to reach cortex. About 30 KB of flash.
      In the max profile (since v0.51.1): it needs an ISP-side collector.
      See docs/fleet-learning.md.
  config AIGW_FEATURE_PLUGINS
    bool "Plug-ins and skills (MQTT, Home Assistant, the bridge to cortex)" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_SNI
    bool "Connection names from TLS and QUIC handshakes" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_METER
    bool "Per-device traffic meter (eBPF)" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_FASTPATH
    bool "Kernel fast path (flowtable)" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_REMOTE
    bool "Remote access over WireGuard" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_KEYS
    bool "Per-device Wi-Fi keys" if AIGW_PROFILE_CUSTOM
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM
  config AIGW_FEATURE_PROVEN
    bool "Proven self-repair: every fix predicted, measured, undone if it didn't help" if AIGW_PROFILE_CUSTOM
    depends on AIGW_FEATURE_CARE
    default y if AIGW_PROFILE_MAX || AIGW_PROFILE_FULL || AIGW_PROFILE_CUSTOM || AIGW_PROFILE_SMALL
    help
      Each fix AIGW takes or proposes (a channel, a device kept on 2.4 GHz,
      smart queueing, a mesh move) gets a receipt: what it predicts, what was
      measured after it against the home's normal for that hour, and whether
      it helped. A fix that didn't help is undone (enforce mode only). In
      advise mode, what AIGW would have done is measured without it (never
      applied). The fix rate by fault and kind of fix, and an optional
      confidence gate: act only with kinds proven here. agctl repair;
      Device.X_AIGW_.Repair. (docs/proven-repair.md). 40 KB on x86-64.
  endmenu
endef

CMAKE_OPTIONS += -DAIGW_TESTS=OFF $(foreach f,$(AIGW_FEATURES),-DAIGW_FEATURE_$(f)=$(if $(CONFIG_AIGW_FEATURE_$(f)),ON,OFF))

# aarch64 and MIPS: binutils aligns segments to 64 KB pages by default, which
# pads each program with up to 60 KB of zeros in flash (agd crossed one such
# line in v0.40; on mipsel, agdns and each plug-in carried 35 to 50 KB of
# them). OpenWRT's arm64 and MIPS kernels use 4 KB pages, so link for those.
ifneq ($(filter aarch64 mips mipsel,$(ARCH)),)
  TARGET_LDFLAGS += -Wl,-z,max-page-size=4096
endif

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(AIGW_SRC)/CMakeLists.txt $(AIGW_SRC)/cmake $(AIGW_SRC)/include $(AIGW_SRC)/core \
		$(AIGW_SRC)/agd $(AIGW_SRC)/agdns $(AIGW_SRC)/agctl $(AIGW_SRC)/plugins $(AIGW_SRC)/cwmp \
		$(AIGW_SRC)/demo $(AIGW_SRC)/scripts $(AIGW_SRC)/tests $(AIGW_SRC)/models $(AIGW_SRC)/LICENSE \
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
		$(1)/etc/hotplug.d/dhcp $(1)/usr/share/aigw/models \
		$(1)/lib/upgrade/keep.d $(1)/usr/lib/aigw/backends $(1)/usr/libexec/aigw
	$(INSTALL_DATA) $(PKG_INSTALL_DIR)/usr/share/aigw/models/domain-risk.agm $(1)/usr/share/aigw/models/
	# platform handlers: Wi-Fi settings and firmware, as a management system changes them
	$(INSTALL_BIN) ./files/wifi-config $(1)/usr/libexec/aigw/wifi-config
	$(INSTALL_BIN) ./files/firmware $(1)/usr/libexec/aigw/firmware
	# the mesh handler: an EasyMesh controller's Data Elements northbound (TR-181), when one runs here
	$(if $(CONFIG_AIGW_FEATURE_MESH),$(INSTALL_BIN) ./files/mesh-dataelements $(1)/usr/libexec/aigw/mesh-dataelements)
	$(if $(CONFIG_AIGW_FEATURE_PLUGINS),$(INSTALL_DIR) $(1)/usr/share/aigw/plugins/iot-guard)
	$(if $(CONFIG_AIGW_FEATURE_PLUGINS),$(INSTALL_DATA) $(PKG_INSTALL_DIR)/usr/share/aigw/plugins/iot-guard/manifest.json \
		$(1)/usr/share/aigw/plugins/iot-guard/)
	# the WAN's own numbers for line health (an OEM's modem may have its own: line.handler)
	$(if $(CONFIG_AIGW_FEATURE_AREAPREDICT),$(INSTALL_BIN) ./files/line-stats $(1)/usr/libexec/aigw/line-stats)
	# per-device Wi-Fi keys, your plug-ins, trusted keys, model updates and
	# device history survive a firmware upgrade
	echo /etc/aigw/ > $(1)/lib/upgrade/keep.d/aigw
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/agd $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/agdns $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-nft $(1)/usr/sbin/
	$(if $(CONFIG_AIGW_FEATURE_SQM),$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-sqm $(1)/usr/sbin/)
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-dhcp-event $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/aigw-selftest $(1)/usr/sbin/
	$(INSTALL_DATA) ./files/aigw.hotplug-dhcp $(1)/etc/hotplug.d/dhcp/90-aigw
	# agctl is agd called by another name (one copy of the program in flash)
	$(LN) ../sbin/agd $(1)/usr/bin/agctl
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

# An upgrade keeps remote access as it was: opkg runs the old prerm with "upgrade"
# (and PKG_UPGRADE=1) before the new package's scripts. Up to 0.42 that prerm turned
# remote access off (agctl remote off) on an upgrade too; what it leaves: remote off,
# phones still in the peers file, the config written a moment ago. Then postinst
# turns it on again (also off within 5 minutes of an upgrade by hand: on again).
define Package/aigw-remote/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
[ "$${PKG_UPGRADE:-0}" = 1 ] || exit 0
dir=$$(uci -q get aigw.remote.dir); dir=$${dir:-/etc/aigw/remote}
if [ "$$(uci -q get aigw.remote.enabled)" = 0 ] && [ -n "$$(uci -q get aigw.remote.endpoint)" ] &&
   [ -s "$$dir/peers" ] && [ -n "$$(find /etc/config/aigw -mmin -5 2>/dev/null)" ]; then
	# the setting, for agd's next start; and on now, through agd (whichever version runs at
	# this point, perhaps still starting)
	uci set aigw.remote.enabled=1
	uci commit aigw
	i=0
	until agctl remote on "$$(uci -q get aigw.remote.endpoint)" >/dev/null 2>&1; do
		i=$$((i + 1)); [ $$i -ge 10 ] && break; sleep 1
	done
	logger -t aigw "remote access on again after the upgrade (AIGW 0.42 and older turned it off)"
fi
exit 0
endef

define Package/aigw-remote/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
case " $${PKG_UPGRADE:-0} $$* " in *" 1 "*|*" upgrade "*) exit 0 ;; esac
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

# as for aigw: an upgrade keeps it as it was. Up to 0.42 the prerm turned it off at
# boot on an upgrade: with an ACS set, it is on at boot again (it does nothing until
# aigw.mgmt.enabled is 1)
define Package/aigw-cwmp/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
[ "$${PKG_UPGRADE:-0}" = 1 ] || exit 0
if [ -n "$$(uci -q get aigw.cwmp.url)" ] && ! /etc/init.d/aigw-cwmp enabled 2>/dev/null; then
	/etc/init.d/aigw-cwmp enable
fi
exit 0
endef

define Package/aigw-cwmp/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
case " $${PKG_UPGRADE:-0} $$* " in *" 1 "*|*" upgrade "*) exit 0 ;; esac
/etc/init.d/aigw-cwmp stop 2>/dev/null
/etc/init.d/aigw-cwmp disable 2>/dev/null
if uci -q get firewall.aigw_cwmp >/dev/null; then
	uci -q delete firewall.aigw_cwmp
	uci commit firewall
	fw4 reload 2>/dev/null
fi
exit 0
endef

CORTEX_SRC:=$(AIGW_SRC)/cortex/aigw_cortex

define Package/aigw-demo/install
	$(INSTALL_DIR) $(1)/usr/libexec/aigw $(1)/etc/init.d $(1)/usr/lib/aigw-demo/cortex/aigw_cortex/static
	# not installed by "make install": built for the test suite, packaged only here
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/demo/aigw-demo $(1)/usr/libexec/aigw/
	$(INSTALL_BIN) ./files/aigw-demo.init $(1)/etc/init.d/aigw-demo
	$(INSTALL_DATA) $(CORTEX_SRC)/*.py $(1)/usr/lib/aigw-demo/cortex/aigw_cortex/
	$(INSTALL_DATA) $(CORTEX_SRC)/static/* $(1)/usr/lib/aigw-demo/cortex/aigw_cortex/static/
endef

LUCI_SRC:=$(AIGW_SRC)/platform/openwrt/luci-app-aigw

define Package/luci-app-aigw/install
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d $(1)/usr/share/rpcd/acl.d \
		$(1)/www/luci-static/resources/view/aigw $(1)/www/luci-static/resources/aigw
	$(INSTALL_DATA) $(LUCI_SRC)/root/usr/share/luci/menu.d/luci-app-aigw.json $(1)/usr/share/luci/menu.d/
	# Wi-Fi keys and remote access: with either
	$(if $(CONFIG_AIGW_FEATURE_KEYS)$(CONFIG_AIGW_FEATURE_REMOTE),$(INSTALL_DATA) \
		$(LUCI_SRC)/root/usr/share/luci/menu.d/luci-app-aigw-access.json $(1)/usr/share/luci/menu.d/)
	$(INSTALL_DATA) $(LUCI_SRC)/root/usr/share/rpcd/acl.d/luci-app-aigw.json $(1)/usr/share/rpcd/acl.d/
	$(INSTALL_DATA) $(LUCI_SRC)/htdocs/luci-static/resources/view/aigw/overview.js \
		$(LUCI_SRC)/htdocs/luci-static/resources/view/aigw/devices.js \
		$(LUCI_SRC)/htdocs/luci-static/resources/view/aigw/decisions.js \
		$(LUCI_SRC)/htdocs/luci-static/resources/view/aigw/plugins.js $(1)/www/luci-static/resources/view/aigw/
	$(if $(CONFIG_AIGW_FEATURE_KEYS)$(CONFIG_AIGW_FEATURE_REMOTE),$(INSTALL_DATA) \
		$(LUCI_SRC)/htdocs/luci-static/resources/view/aigw/access.js $(1)/www/luci-static/resources/view/aigw/)
	$(INSTALL_DATA) $(LUCI_SRC)/htdocs/luci-static/resources/aigw/logo-mark.svg $(1)/www/luci-static/resources/aigw/
endef

define Package/luci-app-aigw/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
rm -f /tmp/luci-indexcache* /tmp/luci-modulecache/* 2>/dev/null
/etc/init.d/rpcd reload 2>/dev/null
exit 0
endef

# An upgrade keeps AIGW as it was: on at boot if it was, its fw4 include. opkg runs
# the old prerm with "upgrade" (PKG_UPGRADE=1) first; OpenWRT's own scripts stop agd
# then and start the new one after. Up to 0.42 that prerm also turned AIGW off at
# boot and removed the fw4 include on an upgrade (uci-defaults puts the include
# back). The new preinst runs right after that prerm with "upgrade <old version>"
# (opkg's record still names the old one): from 0.42 or older, AIGW is turned on
# at boot again by postinst.
define Package/aigw/preinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
[ "$$1" = upgrade ] || [ "$${PKG_UPGRADE:-0}" = 1 ] || exit 0
v=$${2:-$$(sed -n 's/^Version: //p' /usr/lib/opkg/info/aigw.control 2>/dev/null)}
v=$${v%%-*}; maj=$${v%%.*}; min=$${v#*.}; min=$${min%%.*}
case "$$maj.$$min" in *[!0-9.]*|.*|*.) exit 0 ;; esac
if [ "$$maj" -eq 0 ] && [ "$$min" -lt 43 ]; then
	mkdir -p /tmp/aigw-upgrade
	touch /tmp/aigw-upgrade/enable
fi
exit 0
endef

define Package/aigw/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
if [ -e /tmp/aigw-upgrade/enable ]; then
	rm -f /tmp/aigw-upgrade/enable
	if ! /etc/init.d/aigw enabled 2>/dev/null; then
		/etc/init.d/aigw enable
		logger -t aigw "on at boot again after the upgrade (AIGW 0.42 and older turned it off)"
	fi
fi
exit 0
endef

define Package/aigw/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
case " $${PKG_UPGRADE:-0} $$* " in *" 1 "*|*" upgrade "*) exit 0 ;; esac
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
$(eval $(call BuildPackage,aigw-demo))
