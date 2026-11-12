# aigw-usp: AIGW OS managed by USP (TR-369). A plug-in for the unmodified
# OB-USP-Agent (obuspa) that serves AIGW OS's data model from agd (ubus ag.dm),
# a launcher that configures the agent from /etc/config/aigw, and its service.

include $(TOPDIR)/rules.mk

PKG_NAME:=aigw-usp
PKG_VERSION:=0.26.1
PKG_RELEASE:=1

PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=AIGW contributors
PKG_BUILD_DEPENDS:=obuspa

include $(INCLUDE_DIR)/package.mk

# repository root (this Makefile is in <repo>/platform/openwrt/aigw-usp/)
AIGW_SRC:=$(abspath $(CURDIR)/../../..)

define Package/aigw-usp
  SECTION:=net
  CATEGORY:=Network
  TITLE:=AIGW OS management by USP / TR-369 (data model for OB-USP-Agent)
  DEPENDS:=+aigw +obuspa +libubox +libubus
endef

define Package/aigw-usp/description
  Lets an ISP's USP (TR-369) controller manage AIGW OS: devices, alerts,
  decisions (approve, reject, undo), quarantine and block, over MQTT.
  Off until you turn it on: uci set aigw.mgmt.enabled=1; uci set
  aigw.usp.broker=<MQTT broker>; uci commit aigw; /etc/init.d/aigw-usp start.
  What a controller may do is set by aigw.mgmt.actions and
  aigw.mgmt.alert_details; AIGW OS's guardrails always apply.
endef

# the agent's feature switches: its headers change with them (see ../obuspa)
USP_DEFS:=-DDISABLE_STOMP=1 -DENABLE_MQTT=1 -DENABLE_UDS=1 -DREMOVE_DEVICE_BULKDATA=1

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(AIGW_SRC)/usp/aigw_usp.c $(AIGW_SRC)/usp/aigw-usp-run $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -fPIC -shared -Wall $(USP_DEFS) \
		-I$(STAGING_DIR)/usr/include/obuspa \
		-o $(PKG_BUILD_DIR)/aigw-usp.so $(PKG_BUILD_DIR)/aigw_usp.c \
		$(TARGET_LDFLAGS) -lubus -lubox -lpthread
endef

define Package/aigw-usp/install
	$(INSTALL_DIR) $(1)/usr/lib/aigw/usp $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/aigw-usp.so $(1)/usr/lib/aigw/usp/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/aigw-usp-run $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/aigw-usp.init $(1)/etc/init.d/aigw-usp
endef

$(eval $(call BuildPackage,aigw-usp))
