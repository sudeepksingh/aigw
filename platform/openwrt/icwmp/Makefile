# icwmp (icwmpd), iopsys's TR-069 client, unmodified, at the commit AIGW OS
# is tested with (scripts/build-icwmp.sh builds the same one). It reaches its
# data model only over ubus: aigw-cwmp serves AIGW OS's there. Only the client
# is packaged; its data model plug-in for bbfdm is not needed.

include $(TOPDIR)/rules.mk

PKG_NAME:=icwmp
PKG_VERSION:=2026.11.03
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://dev.iopsys.eu/bbf/icwmp.git
PKG_SOURCE_VERSION:=0579a5181579396c2eded021a93c94ffe67d4eec
PKG_MIRROR_HASH:=skip

PKG_LICENSE:=BSD-3-Clause
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=AIGW contributors

PKG_BUILD_PARALLEL:=1
CMAKE_SOURCE_SUBDIR:=src

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/icwmp
  SECTION:=net
  CATEGORY:=Network
  TITLE:=icwmp: iopsys's TR-069 (CWMP) client
  URL:=https://dev.iopsys.eu/bbf/icwmp
  DEPENDS:=+libubox +libubus +libuci +libblobmsg-json +libjson-c +libcurl +libmxml +libuuid +libopenssl +zlib
endef

define Package/icwmp/description
  iopsys's open source TR-069 (CWMP) client. Install aigw-cwmp to manage
  AIGW OS through it.
endef

CMAKE_OPTIONS += -DBBF_VENDOR_PREFIX=X_AIGW_
# icwmp builds with -Werror; these warnings depend on the compiler and libc, not on icwmp
TARGET_CFLAGS += -Wno-error=unused-result -Wno-error=format-truncation -Wno-error=unused-function \
	-Wno-error=stringop-truncation -Wno-error=format-overflow -Wno-error=deprecated-declarations

define Package/icwmp/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/icwmpd
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/src/icwmpd $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,icwmp))
