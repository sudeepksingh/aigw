add_executable(test_dns test_dns.c)
target_link_libraries(test_dns ag)
add_test(NAME dns_parser COMMAND test_dns)
