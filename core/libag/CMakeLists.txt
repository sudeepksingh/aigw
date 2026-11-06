add_library(ag STATIC log.c dns.c config.c)
target_link_libraries(ag ${UCI_LIBRARY} ${UBOX_LIBRARY})
