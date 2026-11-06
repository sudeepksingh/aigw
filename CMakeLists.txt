cmake_minimum_required(VERSION 3.13)
project(aigw VERSION 0.1.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(AIGW_TESTS "Build unit tests" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
add_compile_definitions(AG_VERSION="${PROJECT_VERSION}")

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

include_directories(${PROJECT_SOURCE_DIR}/include ${UBOX_INCLUDE_DIR} ${UBUS_INCLUDE_DIR} ${UCI_INCLUDE_DIR})

add_subdirectory(core/libag)
add_subdirectory(agd)
add_subdirectory(agdns)
add_subdirectory(agctl)

install(PROGRAMS scripts/aigw-nft DESTINATION sbin)

if(AIGW_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
