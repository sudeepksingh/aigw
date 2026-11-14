add_executable(test_dns test_dns.c)
target_link_libraries(test_dns ag)
add_test(NAME dns_parser COMMAND test_dns)

add_executable(test_qr test_qr.c)
target_link_libraries(test_qr ag)
add_test(NAME qr_encoder COMMAND test_qr)

add_executable(test_sign test_sign.c)
target_link_libraries(test_sign ag)
add_test(NAME signatures COMMAND test_sign)

add_executable(test_heal test_heal.c)
target_link_libraries(test_heal ag)
add_test(NAME wifi_diagnosis COMMAND test_heal)

add_executable(test_diag test_diag.c)
target_link_libraries(test_diag ag)
add_test(NAME diagnose COMMAND test_diag)
add_executable(test_care test_care.c)
target_link_libraries(test_care ag)
add_test(NAME proactive_care COMMAND test_care)

add_executable(test_sni test_sni.c)
target_link_libraries(test_sni ag)
add_test(NAME server_names COMMAND test_sni ${PROJECT_SOURCE_DIR}/tests/data)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME cortex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cortex.py)
endif()

# example model back-ends (plugins/examples/backend): one that agrees with the reference, one that doesn't
add_library(example_backend MODULE ${PROJECT_SOURCE_DIR}/plugins/examples/backend/example_backend.c)
add_library(example_backend_broken MODULE ${PROJECT_SOURCE_DIR}/plugins/examples/backend/example_backend.c)
target_compile_definitions(example_backend_broken PRIVATE EXAMPLE_BROKEN)
set_target_properties(example_backend example_backend_broken PROPERTIES PREFIX "")

add_executable(test_agm test_agm.c)
target_link_libraries(test_agm ag m)
add_test(NAME agm_runtime COMMAND test_agm
  ${PROJECT_SOURCE_DIR}/models/domain-risk.agm
  ${PROJECT_SOURCE_DIR}/tests/data/domain-risk-golden.tsv)
