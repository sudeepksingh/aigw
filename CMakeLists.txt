cmake_minimum_required(VERSION 3.13)
project(aigw VERSION 0.25.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(AIGW_TESTS "Build unit tests" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
add_compile_definitions(AG_VERSION="${PROJECT_VERSION}"
  AG_DATADIR="${CMAKE_INSTALL_PREFIX}/share/aigw")

# OpenWRT libraries (packaged on OpenWRT; built by scripts/build-deps.sh elsewhere)
find_path(UBOX_INCLUDE_DIR libubox/uloop.h)
find_path(UBUS_INCLUDE_DIR libubus.h)
find_path(UCI_INCLUDE_DIR uci.h)
find_library(UBOX_LIBRARY ubox)
find_library(UBUS_LIBRARY ubus)
find_library(UCI_LIBRARY uci)
find_library(BLOBMSG_JSON_LIBRARY blobmsg_json)

foreach(v UBOX_INCLUDE_DIR UBUS_INCLUDE_DIR UCI_INCLUDE_DIR UBOX_LIBRARY UBUS_LIBRARY UCI_LIBRARY BLOBMSG_JSON_LIBRARY)
  if(NOT ${v})
    message(FATAL_ERROR "${v} not found. Install libubox, ubus and uci first: sudo ./scripts/build-deps.sh")
  endif()
endforeach()

# connection tracking (flow sensor)
find_path(NFCT_INCLUDE_DIR libnetfilter_conntrack/libnetfilter_conntrack.h)
find_library(NFCT_LIBRARY netfilter_conntrack)
find_library(NFNETLINK_LIBRARY nfnetlink)
foreach(v NFCT_INCLUDE_DIR NFCT_LIBRARY NFNETLINK_LIBRARY)
  if(NOT ${v})
    message(FATAL_ERROR "${v} not found. Debian/Ubuntu: apt install libnetfilter-conntrack-dev")
  endif()
endforeach()

include_directories(${PROJECT_SOURCE_DIR}/include ${UBOX_INCLUDE_DIR} ${UBUS_INCLUDE_DIR} ${UCI_INCLUDE_DIR} ${NFCT_INCLUDE_DIR})

add_subdirectory(core/libag)
add_subdirectory(agd)
add_subdirectory(agdns)
add_subdirectory(agctl)
add_subdirectory(plugins)

# USP (TR-369): the plug-in for OB-USP-Agent, built against the obuspa source
# tree the agent is built from (scripts/build-obuspa.sh fetches the pinned one)
set(AIGW_OBUSPA_SRC "" CACHE PATH "obuspa source tree: build the aigw-usp plug-in for it")
if(AIGW_OBUSPA_SRC)
  add_subdirectory(usp)
endif()

# TR-069: the data model daemon for icwmp (needs nothing from icwmp to build)
add_subdirectory(cwmp)

# prplOS: an Ambiorix component (amxrt plug-in) publishing AIGW's subtree on
# prplOS's bus. Built when Ambiorix is found: on prplOS / an SDK with it, or
# AIGW_AMX_ROOT from scripts/build-ambiorix.sh
set(AIGW_AMX_ROOT "" CACHE PATH "Ambiorix install root (scripts/build-ambiorix.sh)")
find_path(AMXD_INCLUDE amxd/amxd_dm.h HINTS ${AIGW_AMX_ROOT}/include)
if(AMXD_INCLUDE)
  add_subdirectory(amx)
endif()

install(PROGRAMS scripts/aigw-nft scripts/aigw-dhcp-event scripts/aigw-remote scripts/aigw-selftest DESTINATION sbin)
install(FILES models/domain-risk.agm DESTINATION share/aigw/models)

if(AIGW_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
