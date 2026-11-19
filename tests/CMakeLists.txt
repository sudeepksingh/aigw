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

add_executable(test_warn test_warn.c)
target_link_libraries(test_warn ag)
add_test(NAME early_warning COMMAND test_warn)

add_executable(test_upgrade test_upgrade.c)
target_link_libraries(test_upgrade ag)
add_test(NAME upgrade_check COMMAND test_upgrade)

add_executable(test_why test_why.c)
target_link_libraries(test_why ag)
add_test(NAME why_slow COMMAND test_why)
set_tests_properties(why_slow PROPERTIES ENVIRONMENT "TZ=UTC")

add_executable(test_offer test_offer.c)
target_link_libraries(test_offer ag)
add_test(NAME isp_signals COMMAND test_offer)

add_executable(test_sqm test_sqm.c)
target_link_libraries(test_sqm ag)
add_test(NAME smart_queueing COMMAND test_sqm)

add_executable(test_calls test_calls.c)
target_link_libraries(test_calls ag)
add_test(NAME calls_first COMMAND test_calls)

add_executable(test_brief test_brief.c)
target_link_libraries(test_brief ag)
add_test(NAME support_brief COMMAND test_brief)
set_tests_properties(support_brief PROPERTIES ENVIRONMENT "TZ=UTC")

add_executable(test_proactive test_proactive.c)
target_link_libraries(test_proactive ag)
add_test(NAME proactive_before_the_call COMMAND test_proactive)

add_executable(test_tr143 test_tr143.c)
target_link_libraries(test_tr143 ag)
add_test(NAME tr143_diagnostics COMMAND test_tr143)

add_executable(test_sni test_sni.c)
target_link_libraries(test_sni ag)
add_test(NAME server_names COMMAND test_sni ${PROJECT_SOURCE_DIR}/tests/data)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME cortex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cortex.py)
  add_test(NAME staged_rollout COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_rollout.py)
  # v0.42: ISP ticketing against a local mock TMF621 server (tests/tmf621_mock.py, 127.0.0.1 only)
  add_test(NAME isp_tickets COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_tickets.py)
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
