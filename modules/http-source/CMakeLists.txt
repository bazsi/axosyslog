if (NOT DEFINED ENABLE_HTTP_SOURCE OR ENABLE_HTTP_SOURCE)
  find_package(LibMHD)
endif()

module_switch(ENABLE_HTTP_SOURCE "Enable http source" LibMHD_FOUND)
if (NOT ENABLE_HTTP_SOURCE)
  return ()
endif ()

if (NOT LibMHD_FOUND)
  message(FATAL_ERROR "HTTP source module enabled, but libmicrohttpd not found")
endif ()

set(HTTP_SOURCE_SOURCES
    httpsource.h
    httpsource.c
    http-server-listener.h
    http-server-listener.c
    httpsource-parser.c
    httpsource-parser.h
    httpsource-plugin.c
)

add_module(
  TARGET http-source
  GRAMMAR httpsource-grammar
  INCLUDES ${LibMHD_INCLUDE_DIR}
  DEPENDS ${LibMHD_LIBRARIES}
  SOURCES ${HTTP_SOURCE_SOURCES}
)
