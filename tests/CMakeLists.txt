add_executable(test_dns test_dns.c)
target_link_libraries(test_dns ag)
add_test(NAME dns_parser COMMAND test_dns)

add_executable(test_qr test_qr.c)
target_link_libraries(test_qr ag)
add_test(NAME qr_encoder COMMAND test_qr)

add_executable(test_sign test_sign.c)
target_link_libraries(test_sign ag)
add_test(NAME signatures COMMAND test_sign)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME cortex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cortex.py)
endif()

add_executable(test_agm test_agm.c)
target_link_libraries(test_agm ag m)
add_test(NAME agm_runtime COMMAND test_agm
  ${PROJECT_SOURCE_DIR}/models/domain-risk.agm
  ${PROJECT_SOURCE_DIR}/tests/data/domain-risk-golden.tsv)
