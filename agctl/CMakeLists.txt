add_executable(agctl main.c selftest.c)
target_link_libraries(agctl ag ${UBUS_LIBRARY} ${BLOBMSG_JSON_LIBRARY} ${UBOX_LIBRARY} ${UCI_LIBRARY})
install(TARGETS agctl RUNTIME DESTINATION bin)
