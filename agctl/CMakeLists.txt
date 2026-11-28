# agctl is built into agd (agd/CMakeLists.txt) and is a link to it: one copy of libag and the
# libraries in flash instead of two. agd runs agctl's main when it is called as "agctl".
add_custom_target(agctl ALL
  COMMAND ${CMAKE_COMMAND} -E create_symlink ../agd/agd ${CMAKE_CURRENT_BINARY_DIR}/agctl
  DEPENDS agd
  COMMENT "agctl: a link to agd")
install(CODE "file(MAKE_DIRECTORY \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin\")
execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ../sbin/agd \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/agctl\")
message(STATUS \"Installing: \$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/agctl -> ../sbin/agd\")")
