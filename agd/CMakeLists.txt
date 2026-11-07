add_executable(agd main.c devices.c endpoint.c flow.c wifi.c alerts.c baseline.c act.c)
target_link_libraries(agd ag ${UBUS_LIBRARY} ${BLOBMSG_JSON_LIBRARY} ${UBOX_LIBRARY} ${UCI_LIBRARY}
  ${NFCT_LIBRARY} ${NFNETLINK_LIBRARY})
install(TARGETS agd RUNTIME DESTINATION sbin)
