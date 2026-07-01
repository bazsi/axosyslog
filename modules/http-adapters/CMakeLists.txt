# the response adapters serve the HTTP (CURL) destination and the request
# authenticators serve the http-source module; build if either is enabled

if (NOT ENABLE_CURL AND NOT ENABLE_HTTP_SOURCE)
  return ()
endif ()

set(HTTP_ADAPTERS_SOURCES
    http-adapter.c
    http-adapter.h
    http-adapters-parser.c
    http-adapters-parser.h
    http-adapters-plugin.c
    splunk-adapter.c
    splunk-adapter.h
    splunk-hec-auth.c
    splunk-hec-auth.h
)

add_module(
  TARGET http-adapters
  GRAMMAR http-adapters-grammar
  INCLUDES ${PROJECT_SOURCE_DIR}
  SOURCES ${HTTP_ADAPTERS_SOURCES}
)

add_test_subdirectory(tests)
