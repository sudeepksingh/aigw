cmake_minimum_required(VERSION 3.13)
project(aigw VERSION 0.48.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(AIGW_TESTS "Build unit tests" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
# small on a router's flash (the 1 MB budget, aigw-selftest): unused functions
# dropped at link time, and no unwind tables (C, no exceptions; large on
# aarch64). About 11% smaller. Debug builds keep everything
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables)
  add_link_options(-Wl,--gc-sections)
  # link-time optimisation: the whole program at once, so code used once is
  # inlined and duplicates go. Where the compiler supports it (OpenWRT's SDK
  # does, with gcc-ar)
  option(AIGW_LTO "Link-time optimisation in release builds" ON)
  if(AIGW_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT AIGW_IPO OUTPUT AIGW_IPO_WHY LANGUAGES C)
    if(AIGW_IPO)
      set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
      message(STATUS "no link-time optimisation: ${AIGW_IPO_WHY}")
    endif()
  endif()
endif()
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

# the features this build has (cmake/features.cmake, docs/features.md)
include(${PROJECT_SOURCE_DIR}/cmake/features.cmake)

include_directories(${PROJECT_BINARY_DIR}/include ${PROJECT_SOURCE_DIR}/include ${UBOX_INCLUDE_DIR} ${UBUS_INCLUDE_DIR} ${UCI_INCLUDE_DIR} ${NFCT_INCLUDE_DIR})

add_subdirectory(core/libag)
add_subdirectory(agd)
add_subdirectory(agdns)
add_subdirectory(agctl)
if(AIGW_FEATURE_PLUGINS)
  add_subdirectory(plugins)
endif()
# the demo image's simulated home shows every feature: only with all of them
list(LENGTH AIGW_FEATURES_ON n_on)
list(LENGTH AIGW_FEATURE_NAMES n_all)
if(n_on EQUAL n_all)
  add_subdirectory(demo)
endif()

# USP (TR-369): the plug-in for OB-USP-Agent, built against the obuspa source
# tree the agent is built from (scripts/build-obuspa.sh fetches the pinned one)
set(AIGW_OBUSPA_SRC "" CACHE PATH "obuspa source tree: build the aigw-usp plug-in for it")
if(AIGW_OBUSPA_SRC)
  add_subdirectory(usp)
endif()

# TR-069: the data model daemon for icwmp (needs nothing from icwmp to build)
add_subdirectory(cwmp)

install(PROGRAMS scripts/aigw-nft scripts/aigw-dhcp-event scripts/aigw-selftest DESTINATION sbin)
if(AIGW_FEATURE_SQM)
  install(PROGRAMS scripts/aigw-sqm DESTINATION sbin)
endif()
if(AIGW_FEATURE_REMOTE)
  install(PROGRAMS scripts/aigw-remote DESTINATION sbin)
endif()
install(FILES models/domain-risk.agm DESTINATION share/aigw/models)

if(AIGW_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
