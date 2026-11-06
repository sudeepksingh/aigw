add_executable(agd main.c)
target_link_libraries(agd ag ${UBUS_LIBRARY} ${UBOX_LIBRARY} ${UCI_LIBRARY})
install(TARGETS agd RUNTIME DESTINATION sbin)
