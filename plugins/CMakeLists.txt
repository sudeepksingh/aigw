# First-party plug-ins: each lands in share/aigw/plugins/<name>/, where agd
# finds it by itself (packages: aigw for the skills, aigw-mqtt for the connector).
add_executable(ag-mqtt mqtt/ag-mqtt.c)
target_link_libraries(ag-mqtt ${BLOBMSG_JSON_LIBRARY} ${UBOX_LIBRARY})
install(TARGETS ag-mqtt RUNTIME DESTINATION share/aigw/plugins/mqtt)
install(FILES mqtt/manifest.json DESTINATION share/aigw/plugins/mqtt)
install(FILES iot-guard/manifest.json DESTINATION share/aigw/plugins/iot-guard)
