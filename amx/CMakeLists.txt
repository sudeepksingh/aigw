add_library(aigw-amx MODULE aigw_amx.c)
set_target_properties(aigw-amx PROPERTIES PREFIX "")
target_include_directories(aigw-amx PRIVATE ${AMXD_INCLUDE})
foreach(lib amxc amxp amxd amxo)
  find_library(${lib}_LIB ${lib} HINTS ${AIGW_AMX_ROOT}/lib)
  target_link_libraries(aigw-amx ${${lib}_LIB})
endforeach()
target_link_libraries(aigw-amx ${UBUS_LIBRARY} ${UBOX_LIBRARY})
# Ambiorix's headers use C11 features and GNU extensions
target_compile_options(aigw-amx PRIVATE -std=gnu11 -Wno-pedantic)

# the plug-in and the ODL amxrt loads it from (prplOS's layout)
install(TARGETS aigw-amx LIBRARY DESTINATION lib/amx/aigw-amx)
set(AIGW_AMX_SO ${CMAKE_INSTALL_PREFIX}/lib/amx/aigw-amx/aigw-amx.so)
configure_file(aigw-amx.odl.in ${CMAKE_CURRENT_BINARY_DIR}/aigw-amx.odl @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/aigw-amx.odl DESTINATION /etc/amx/aigw-amx)
# for tests: the same ODL, loading the plug-in from the build tree
set(AIGW_AMX_SO ${CMAKE_CURRENT_BINARY_DIR}/aigw-amx.so)
configure_file(aigw-amx.odl.in ${CMAKE_CURRENT_BINARY_DIR}/aigw-amx-build.odl @ONLY)
