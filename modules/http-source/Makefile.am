if ENABLE_HTTP_SOURCE

module_LTLIBRARIES                  += modules/http-source/libhttp-source.la
modules_http_source_libhttp_source_la_SOURCES = \
  modules/http-source/http-source.h               \
  modules/http-source/http-source.c               \
  modules/http-source/http-server-listener.h      \
  modules/http-source/http-server-listener.c      \
  modules/http-source/http-source-config.h        \
  modules/http-source/http-source-config.c        \
  modules/http-source/http-source-grammar.y       \
  modules/http-source/http-source-parser.c        \
  modules/http-source/http-source-parser.h        \
  modules/http-source/http-source-plugin.c

modules_http_source_libhttp_source_la_CPPFLAGS  = \
  $(AM_CPPFLAGS)                          \
  $(LIBMICROHTTPD_CFLAGS)                 \
  -I$(top_srcdir)/modules/http-source     \
  -I$(top_builddir)/modules/http-source

modules_http_source_libhttp_source_la_LIBADD  = $(MODULE_DEPS_LIBS) $(LIBMICROHTTPD_LIBS)

modules_http_source_libhttp_source_la_LDFLAGS = $(MODULE_LDFLAGS)

EXTRA_modules_http_source_libhttp_source_la_DEPENDENCIES = $(MODULE_DEPS_LIBS)

modules/http-source modules/http-source/ mod-http-source: modules/http-source/libhttp-source.la
else
modules/http-source modules/http-source/ mod-http-source:
endif

BUILT_SOURCES       +=      \
  modules/http-source/http-source-grammar.y       \
  modules/http-source/http-source-grammar.c       \
  modules/http-source/http-source-grammar.h

EXTRA_DIST        +=      \
  modules/http-source/http-source-grammar.ym       \
  modules/http-source/CMakeLists.txt

.PHONY: modules/http-source/ mod-http-source
