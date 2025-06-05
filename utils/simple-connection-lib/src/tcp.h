/*
 *  TCP IO wrapper
 *
 *  Copyright (c) 2008-2021 Alexander Chukov <sashz@pdaXrom.org>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */

#ifndef TCP_H
#define TCP_H

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#else
#include <windows.h>
#endif
#ifdef ENABLE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

enum {
    TCP_SERVER = 0,
    TCP_SSL_SERVER,
    TCP_CLIENT,
    TCP_SSL_CLIENT
};

enum {
    SIMPLE_CONNECTION_METHOD_DIRECT = 0,
    SIMPLE_CONNECTION_METHOD_CONNECT,
    SIMPLE_CONNECTION_METHOD_WS
};

typedef struct _tcp_channel {
    int s;
    struct sockaddr_in my_addr;
    int mode;
    int primary_mode;
#ifdef ENABLE_SSL
    SSL *ssl;
    SSL_CTX *ctx;
#endif
    char *host;
    char *path;
    char *ws_path;
    int connection_method;
    /* ws socket mode */
    void *ws;
} tcp_channel;

#define tcp_fd(u) (u->s)

#define tcp_ws_path(u) (u->ws_path)

#ifdef __cplusplus
extern "C" {
#endif

tcp_channel *tcp_open(int mode, const char *addr, int port, char *sslkeyfile, char *sslcertfile);
tcp_channel *tcp_accept(tcp_channel *u);
int tcp_connection_upgrade(tcp_channel *u, int connection_method, const char *path, char *request, size_t len);
int tcp_read(tcp_channel *u, void *buf, size_t len);
int tcp_write(tcp_channel *u, void *buf, size_t len);
int tcp_close(tcp_channel *u);

#ifdef __cplusplus
}
#endif

#endif
