add_library(ag STATIC log.c dns.c config.c agm.c agm_flat.c domfeat.c qr.c sign.c sni.c)
target_link_libraries(ag ${UCI_LIBRARY} ${UBOX_LIBRARY} m)
