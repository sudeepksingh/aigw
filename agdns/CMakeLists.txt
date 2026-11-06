add_executable(agdns main.c)
target_link_libraries(agdns ag ${UBUS_LIBRARY} ${UBOX_LIBRARY} ${UCI_LIBRARY})
install(TARGETS agdns RUNTIME DESTINATION sbin)
