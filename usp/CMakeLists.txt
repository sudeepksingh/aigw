add_library(aigw-usp MODULE aigw_usp.c)
set_target_properties(aigw-usp PROPERTIES PREFIX "")
target_include_directories(aigw-usp PRIVATE
  ${AIGW_OBUSPA_SRC}/src/include
  ${AIGW_OBUSPA_SRC}/src/vendor)
# the agent's feature switches: its headers change with them (scripts/build-obuspa.sh builds with these)
set(AIGW_OBUSPA_DEFINES "DISABLE_STOMP=1;ENABLE_MQTT=1;ENABLE_UDS=1;REMOVE_DEVICE_BULKDATA=1"
  CACHE STRING "the defines obuspa was configured with")
target_compile_definitions(aigw-usp PRIVATE ${AIGW_OBUSPA_DEFINES})
find_package(Threads REQUIRED)
target_link_libraries(aigw-usp ${UBUS_LIBRARY} ${UBOX_LIBRARY} Threads::Threads)
install(TARGETS aigw-usp LIBRARY DESTINATION lib/aigw/usp)
install(PROGRAMS aigw-usp-run DESTINATION sbin)
