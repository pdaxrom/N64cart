AM_CFLAGS = $(OPENSSL_CFLAGS)
LIBS = $(OPENSSL_LIBS)

lib_LTLIBRARIES = libsimple_connection.la 
libsimple_connection_la_SOURCES = tcp.c tcp.h udp.c udp.h base64.c base64.h getrandom.c getrandom.h

libsimple_connectionincludedir = $(includedir)/simple-connection
libsimple_connectioninclude_HEADERS = $(top_srcdir)/src/tcp.h $(top_srcdir)/src/udp.h
