add_executable(aigw-cwmp-dm aigw_cwmp.c)
target_link_libraries(aigw-cwmp-dm ag ${UBUS_LIBRARY} ${UBOX_LIBRARY} ${UCI_LIBRARY})
install(TARGETS aigw-cwmp-dm RUNTIME DESTINATION sbin)
install(PROGRAMS aigw-cwmp-run DESTINATION sbin)
# icwmp's FactoryReset runs "defaultreset": through agd (libexec, not on PATH by
# default on a PC, where aigw-cwmp-run puts it there for icwmp)
install(PROGRAMS defaultreset DESTINATION libexec/aigw)
