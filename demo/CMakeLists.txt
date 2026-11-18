# the demo image's simulated home (docs/try-it.md). Built with everything (the
# test suite uses it: tests/demo-test.sh) but never installed: only the demo
# image carries it (the aigw-demo OpenWRT package, platform/openwrt/aigw/Makefile)
add_executable(aigw-demo aigw-demo.c)
target_link_libraries(aigw-demo ag ${BLOBMSG_JSON_LIBRARY} ${UBOX_LIBRARY})
