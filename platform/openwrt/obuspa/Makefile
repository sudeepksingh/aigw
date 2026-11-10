# OB-USP-Agent (obuspa), the Broadband Forum's USP (TR-369) agent, unmodified,
# at the version AIGW OS is tested with. aigw-usp adds AIGW OS's data model to
# it as a plug-in. Same switches as scripts/build-obuspa.sh: MQTT and Unix
# domain sockets; no STOMP, CoAP, WebSockets or bulk data.

include $(TOPDIR)/rules.mk

PKG_NAME:=obuspa
PKG_VERSION:=11.0.7
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/BroadbandForum/obuspa.git
PKG_SOURCE_VERSION:=59028beba21471d19bd3842ef2632ceb6ca8c7fc
PKG_MIRROR_HASH:=skip

PKG_LICENSE:=BSD-3-Clause
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=AIGW contributors

PKG_FIXUP:=autoreconf
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/obuspa
  SECTION:=net
  CATEGORY:=Network
  TITLE:=OB-USP-Agent: the Broadband Forum's USP (TR-369) agent
  URL:=https://github.com/BroadbandForum/obuspa
  DEPENDS:=+libopenssl +libsqlite3 +zlib +libmosquitto-ssl
endef

define Package/obuspa/description
  The Broadband Forum's open source USP (TR-369) agent, over MQTT. Install
  aigw-usp to manage AIGW OS through it.
endef

CONFIGURE_ARGS += \
	--disable-stomp \
	--disable-coap \
	--disable-websockets \
	--disable-bulkdata

# vendor plug-ins (aigw-usp) build against the agent's own headers
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/obuspa
	$(CP) $(PKG_BUILD_DIR)/src/include/*.h $(PKG_BUILD_DIR)/src/vendor/vendor_defs.h $(1)/usr/include/obuspa/
endef

define Package/obuspa/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/obuspa $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,obuspa))
