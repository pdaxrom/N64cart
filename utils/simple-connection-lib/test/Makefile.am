AM_CFLAGS = $(OPENSSL_STATIC_CFLAGS) -I../src
LIBS = ../src/libsimple_connection.la $(OPENSSL_STATIC_LIBS)

noinst_PROGRAMS = tcpclient tcpserver tcpclient-ws tcpserver-ws tcpsslclient tcpsslserver tcpsslclient-ws tcpsslserver-ws udpchat udpclient udpserver

tcpclient_SOURCES = tcpclient.c
tcpclient_LDADD = ../src/libsimple_connection.la

tcpserver_SOURCES = tcpserver.c
tcpserver_LDADD = ../src/libsimple_connection.la

tcpclient_ws_SOURCES = tcpclient-ws.c
tcpclient_ws_LDADD = ../src/libsimple_connection.la

tcpserver_ws_SOURCES = tcpserver-ws.c
tcpserver_ws_LDADD = ../src/libsimple_connection.la

tcpsslclient_SOURCES = tcpsslclient.c
tcpsslclient_LDADD = ../src/libsimple_connection.la

tcpsslserver_SOURCES = tcpsslserver.c
tcpsslserver_LDADD = ../src/libsimple_connection.la

tcpsslclient_ws_SOURCES = tcpsslclient-ws.c
tcpsslclient_ws_LDADD = ../src/libsimple_connection.la

tcpsslserver_ws_SOURCES = tcpsslserver-ws.c
tcpsslserver_ws_LDADD = ../src/libsimple_connection.la

udpclient_SOURCES = udpclient.c
udpclient_LDADD = ../src/libsimple_connection.la

udpserver_SOURCES = udpserver.c
udpserver_LDADD = ../src/libsimple_connection.la

udpchat_SOURCES = udpchat.c
udpchat_LDADD = ../src/libsimple_connection.la
