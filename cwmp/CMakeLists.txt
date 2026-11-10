add_executable(aigw-cwmp-dm aigw_cwmp.c)
target_link_libraries(aigw-cwmp-dm ag ${UBUS_LIBRARY} ${UBOX_LIBRARY} ${UCI_LIBRARY})
install(TARGETS aigw-cwmp-dm RUNTIME DESTINATION sbin)
install(PROGRAMS aigw-cwmp-run DESTINATION sbin)
