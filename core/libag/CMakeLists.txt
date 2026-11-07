add_library(ag STATIC log.c dns.c config.c agm.c domfeat.c qr.c)
target_link_libraries(ag ${UCI_LIBRARY} ${UBOX_LIBRARY} m)
