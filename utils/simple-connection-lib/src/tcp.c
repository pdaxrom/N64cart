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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#define closesocket close
#else
#include <windows.h>
#include <shlwapi.h>
#define strcasestr StrStrIA
#endif

#define PORT 9930

#include "tcp.h"
#include "getrandom.h"
#include "base64.h"

#if defined(__APPLE__)

#include <libkern/OSByteOrder.h>
#define WS_NTOH64(n) OSSwapBigToHostInt64(n)
#define WS_NTOH32(n) OSSwapBigToHostInt32(n)
#define WS_NTOH16(n) OSSwapBigToHostInt16(n)
#define WS_HTON64(n) OSSwapHostToBigInt64(n)
#define WS_HTON16(n) OSSwapHostToBigInt16(n)

#else

#if defined(_WIN32)

#include <windows.h>

#define htobe16(x) __builtin_bswap16(x)
#define htole16(x) (x)
#define be16toh(x) __builtin_bswap16(x)
#define le16toh(x) (x)

#define htobe32(x) __builtin_bswap32(x)
#define htole32(x) (x)
#define be32toh(x) __builtin_bswap32(x)
#define le32toh(x) (x)

#define htobe64(x) __builtin_bswap64(x)
#define htole64(x) (x)
#define be64toh(x) __builtin_bswap64(x)
#define le64toh(x) (x)

#endif

#ifdef sgi
#include <ctype.h>
#include <alloca.h>

typedef int socklen_t;

#define htobe64 htonll
#define htobe32 htonl
#define htobe16 htons
#define ntobe64 ntohll
#define ntobe32 ntohl
#define ntobe16 ntohs

#endif

#define WS_NTOH64(n) be64toh(n)
#define WS_NTOH32(n) be32toh(n)
#define WS_NTOH16(n) be16toh(n)
#define WS_HTON64(n) htobe64(n)
#define WS_HTON32(n) htobe32(n)
#define WS_HTON16(n) htobe16(n)

#endif

enum
{
    WS_OPCODE_CONTINUATION = 0x00,
    WS_OPCODE_TEXT_FRAME = 0x01,
    WS_OPCODE_BINARY_FRAME = 0x02,
    WS_OPCODE_CLOSE = 0x08,
    WS_OPCODE_PING = 0x09,
    WS_OPCODE_PONG = 0x0A,
    WS_OPCODE_INVALID = 0xFF
};

typedef union ws_mask_s {
  unsigned char c[4];
  uint32_t u;
} ws_mask_t;

/* XXX: The union and the structs do not need to be named.
 *      We are working around a bug present in GCC < 4.6 which prevented
 *      it from recognizing anonymous structs and unions.
 *      See http://gcc.gnu.org/bugzilla/show_bug.cgi?id=4784
 */
#if _WIN32
#pragma pack(push, 1)
#endif
typedef struct
#if __GNUC__
__attribute__ ((__packed__)) 
#endif
ws_header_s {
  unsigned char b0;
  unsigned char b1;
  union {
    struct 
#if __GNUC__
    __attribute__ ((__packed__)) 
#endif
           {
      uint16_t l16;
      ws_mask_t m16;
    } s16;
    struct
#if __GNUC__
__attribute__ ((__packed__)) 
#endif
           {
      uint64_t l64;
      ws_mask_t m64;
    } s64;
    ws_mask_t m;
  } u;
} ws_header_t;
#if _WIN32
#pragma pack(pop)
#endif

typedef struct ws_s {
    ws_header_t header;
    int avail;
    int pos;
} ws_t;

#ifdef ENABLE_SSL
static const char *SSL_CIPHER_LIST = "ALL:!LOW";
#endif

#ifdef _WIN32
typedef int socklen_t;

static int winsock_inited = 0;
static int winsock_init(void)
{
    WSADATA w;

    if (winsock_inited)
	return 0;

    /* Open windows connection */
    if (WSAStartup(0x0101, &w) != 0) {
	fprintf(stderr, "Could not open Windows connection.\n");
	return -1;
    }
    
    winsock_inited = 1;
    return 0;
}
#endif

#ifdef sgi
static uint64_t htonll(uint64_t host_value)
{
    uint64_t result = 0;
    uint8_t *src = (uint8_t *)&host_value;
    uint8_t *dst = (uint8_t *)&result;

    for (int i = 0; i < 8; i++) {
        dst[i] = src[7 - i]; // Reverse the byte order
    }

    return result;
}

static uint64_t ntohll(uint64_t net_value)
{
    return htonll(net_value); // Same as htonll()
}

static char *strcasestr(const char *haystack, const char *needle)
{
    if (!*needle) {
        return (char *)haystack;
    }

    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;

        while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) {
            h++;
            n++;
        }

        if (!*n) {
            return (char *)haystack;
        }
    }

    return NULL;
}
#endif

#ifdef ENABLE_SSL
static SSL_CTX *ssl_initialize(char *sslkeyfile, char *sslcertfile)
{
    SSL_CTX *ssl_context;

    /* 0. initialize library */
    SSL_library_init();
    SSL_load_error_strings();

    /* 1. initialize context */
    if ((ssl_context = SSL_CTX_new(SSLv23_server_method())) == NULL) {
	fprintf(stderr, "Failed to initialize SSL context.\n");
	return NULL;
    }

    SSL_CTX_set_options(ssl_context, SSL_OP_ALL);

    if (!SSL_CTX_set_cipher_list(ssl_context, SSL_CIPHER_LIST)) {
	fprintf(stderr, "Failed to set SSL cipher list.\n");
	goto error1;
    }

    if (SSL_CTX_use_PrivateKey_file(ssl_context, sslkeyfile, SSL_FILETYPE_PEM) <= 0) {
	fprintf(stderr, "Failed to load private key file.\n");
	goto error1;
    }

    if (SSL_CTX_use_certificate_file(ssl_context, sslcertfile, SSL_FILETYPE_PEM) <= 0) {
	fprintf(stderr, "Failed to load certificate key file.\n");
	goto error1;
    }

    return ssl_context;
 error1:
    SSL_CTX_free(ssl_context);
    return NULL;
}

static SSL_CTX *ssl_client_initialize(void)
{
    SSL_CTX *ctx;
    /* Set up the library */
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    static const SSL_METHOD *meth;
    meth = SSLv23_client_method();
    ctx = SSL_CTX_new(meth);

    if (!ctx) {
	ERR_print_errors_fp(stderr);
    }

    return ctx;
}

static void ssl_tear_down(SSL_CTX *ctx)
{
    SSL_CTX_free(ctx);
}
#endif

tcp_channel *tcp_open(int mode, const char *addr, int port, char *sslkeyfile, char *sslcertfile)
{
#ifdef _WIN32
    if (winsock_init()) {
	return NULL;
    }
#endif

    tcp_channel *u = (tcp_channel *)malloc(sizeof(tcp_channel));
    memset(u, 0, sizeof(tcp_channel));

    u->mode = mode;
    u->primary_mode = mode;

    if ((u->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
	fprintf(stderr, "socket() error!\n");
	free(u);
	return NULL;
    }

    if ((mode == TCP_SERVER) || (mode == TCP_SSL_SERVER)) {
#ifndef _WIN32
	int yes = 1;
	if(setsockopt(u->s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
	    fprintf(stderr, "setsockopt() error!\n");
	    free(u);
	    return NULL;
	}
#endif

	memset(&u->my_addr, 0, sizeof(u->my_addr));
	u->my_addr.sin_family = AF_INET;
        u->my_addr.sin_addr.s_addr = INADDR_ANY;
        u->my_addr.sin_port = htons(port);

	if(bind(u->s, (struct sockaddr *)&u->my_addr, sizeof(u->my_addr)) == -1) {
	    fprintf(stderr, "bind() error!\n");
	    closesocket(u->s);
	    free(u);
	    return NULL;
	}

	if (listen(u->s, 10) == -1) {
	    fprintf(stderr, "listen() error!\n");
	    closesocket(u->s);
	    free(u);
	    return NULL;
	}

	if (mode == TCP_SSL_SERVER) {
#ifdef ENABLE_SSL
	    u->ctx = ssl_initialize(sslkeyfile, sslcertfile);
#else
	    fprintf(stderr, "ssl support missed in this configuration!\n");
	    closesocket(u->s);
	    free(u);
	    return NULL;
#endif
	}
    } else {
	struct hostent *server = gethostbyname(addr);
	if (server == NULL) {
	    fprintf(stderr, "gethostbyname() no such host\n");
	    free(u);
	    return NULL;
	}

	memset(&u->my_addr, 0, sizeof(u->my_addr));
	u->my_addr.sin_family = AF_INET;
        u->my_addr.sin_addr = *((struct in_addr *)server->h_addr);
        u->my_addr.sin_port = htons(port);

	if (connect(u->s, (struct sockaddr *)&u->my_addr, sizeof(struct sockaddr)) == -1) {
	    fprintf(stderr, "connect()\n");
	    closesocket(u->s);
	    free(u);
	    return NULL;
	}

	if (mode == TCP_SSL_CLIENT) {
#ifdef ENABLE_SSL
	    u->ctx = ssl_client_initialize();
	    u->ssl = SSL_new(u->ctx);
	    SSL_set_tlsext_host_name(u->ssl, addr);
	    SSL_set_fd(u->ssl, u->s);
	    int retval;
	    if ((retval = SSL_connect(u->ssl)) < 0) {
		fprintf(stderr, "SSL_connect(): %d\n", SSL_get_error(u->ssl, retval));
		SSL_free(u->ssl);
		ssl_tear_down(u->ctx);
		free(u);
		return NULL;
	    }
#else
	    fprintf(stderr, "ssl support missed in this configuration!\n");
	    closesocket(u->s);
	    free(u);
	    return NULL;
#endif
	}
    }

    u->host = (char *)addr;

    return u;
}

static int tcp_write_ws(tcp_channel *u, uint8_t opcode, char *buf, size_t len);

int tcp_close(tcp_channel *u)
{
    if (u->s >= 0) {
	if (u->connection_method == SIMPLE_CONNECTION_METHOD_WS) {
	    char buf[] = { 0, 0, 'C', 'l', 'o', 's', 'e', 'd' };
	    *((unsigned short *)buf) = htons(1000);
	    tcp_write_ws(u, WS_OPCODE_CLOSE, buf, sizeof(buf));
	}

#ifdef ENABLE_SSL
	if ((u->mode == TCP_SSL_CLIENT) || (u->mode == TCP_SSL_SERVER)) {
	    if (u->mode == TCP_SSL_CLIENT) {
		SSL_shutdown(u->ssl);
		SSL_free(u->ssl);
	    }
	    if (u->ctx) {
		ssl_tear_down(u->ctx);
	    }
	}
#endif
	closesocket(u->s);
    }

    if (u->ws_path) {
	free(u->ws_path);
    }

    if (u->ws) {
	free(u->ws);
    }

    free(u);
/*
#ifdef _WIN32
    if (winsock_inited) {
	WSACleanup();
	winsock_inited = 0;
    }
#endif
 */
    return 0;
}

tcp_channel *tcp_accept(tcp_channel *u)
{
    tcp_channel *n = (tcp_channel *)malloc(sizeof(tcp_channel));
    memset(n, 0, sizeof(tcp_channel));

    if (u->mode == TCP_SSL_SERVER) {
	n->mode = TCP_SSL_CLIENT;
    } else {
	n->mode = TCP_CLIENT;
    }

    n->primary_mode = u->mode;

    socklen_t l = sizeof(struct sockaddr);
    if ((n->s = accept(u->s, (struct sockaddr *)&n->my_addr, &l)) < 0) {
	fprintf(stderr, "accept()\n");
	free(n);
	return NULL;
    }

#ifdef ENABLE_SSL
    if (u->mode == TCP_SSL_SERVER) {
	if ((n->ssl = SSL_new(u->ctx)) == NULL) {
	    fprintf(stderr, "Failed to create SSL connection.\n");
	    closesocket(n->s);
	    free(n);
	    return NULL;
	}

	SSL_set_fd(n->ssl, n->s);

	if (SSL_accept(n->ssl) < 0) {
	    fprintf(stderr, "Unable to accept SSL connection.\n");
	    ERR_print_errors_fp(stderr);
	    SSL_free(n->ssl);
	    closesocket(n->s);
	    free(n);
	    return NULL;
	}
    }
#endif

    return n;
}

static int tcp_read_internal(tcp_channel *u, char *buf, size_t len)
{
    int r;

#ifdef ENABLE_SSL
    if ((u->mode == TCP_SSL_CLIENT) || (u->mode == TCP_SSL_SERVER)) {
	if ((r = SSL_read(u->ssl, buf, len)) < 0) {
	    fprintf(stderr, "SSL_read()\n");
	}
    } else
#endif
    {
	if ((r = recv(u->s, buf, len, 0)) == -1) {
	    fprintf(stderr, "recvfrom()\n");
	}
    }

    return r;
}

static int tcp_write_internal(tcp_channel *u, char *buf, size_t len)
{
    int r;
#ifdef ENABLE_SSL
    if ((u->mode == TCP_SSL_CLIENT) || (u->mode == TCP_SSL_SERVER)) {
	if ((r = SSL_write(u->ssl, buf, len)) < 0) {
	    fprintf(stderr, "SSL_write()\n");
	}
    } else
#endif
    {
	if ((r = send(u->s, buf, len, 0)) < 0) {
	    fprintf(stderr, "sendto()\n");
	}
    }

    return r;
}

static int get_http_header(tcp_channel *channel, char *head, int headLen)
{
    int nread;
    int totalRead = 0;
    char crlf[4] = { 0, 0, 0, 0 };

    while (totalRead < headLen - 1) {
	nread = tcp_read_internal(channel, &head[totalRead], 1);
	if (nread <= 0) {
//		fprintf(stderr, "%s: Cannot read response (%s)\n", __func__, strerror(errno));
	    return -1;
	}
	crlf[0] = crlf[1];
	crlf[1] = crlf[2];
	crlf[2] = crlf[3];
	crlf[3] = head[totalRead];

	totalRead += nread;

	if (crlf[0] == 13 && crlf[1] == 10 && crlf[2] == 13 && crlf[3] == 10) {
	    break;
	    }

	if (crlf[2] == 10 && crlf[3] == 10) {
	    break;
	}
    }

    head[totalRead] = 0;

    return totalRead;
}

static char *copy_string(char *dst, int dstSize, char *src, int srcSize)
{
    if (!dst) {
	dstSize = srcSize;
	dst = malloc(dstSize + 1);
    } else {
	dstSize = (srcSize > dstSize) ? dstSize : srcSize;
    }
    memcpy(dst, src, dstSize);
    dst[dstSize] = 0;
    return dst;
}

static char *header_get_field(char *header, char *field, char *str, int n)
{
    while (*header) {
	if (strcasestr(header, field) == header && *(header + strlen(field)) == ':') {
	    char *tmp1 = header + strlen(field) + 1;
	    for (tmp1++; *tmp1 == ' '; tmp1++) ;
	    char *tmp;
	    for (tmp = tmp1; *tmp != 0 && *tmp != '\n' && *tmp != '\r'; tmp++) ;
	    return copy_string(str, n, tmp1, tmp - tmp1);
	}

	while (*header && *header != '\r' && *header != '\n') {
	    header++;
	}
	while (*header && (*header == '\r' || *header == '\n')) {
	    header++;
	}
    }
    return NULL;
}

static char *header_get_path(char *header, char *str, int n)
{
    char *tmp = strchr(header, ' ');
    if (tmp) {
	char *tmp1 = strchr(tmp + 1, ' ');
	if (tmp1) {
	    return copy_string(str, n, tmp + 1, tmp1 - tmp - 1);
	}
    }
    if (str) {
	str[0] = 0;
    }
    return NULL;
}

static int http_ws_method_server(tcp_channel *channel, char *request, size_t len)
{
    char req[1024];
    char field[256];
#ifdef ENABLE_SSL
    unsigned char sha1buf[SHA_DIGEST_LENGTH];
#endif

    if (request) {
	*request = 0;
    }

    if (get_http_header(channel, req, sizeof(req)) <= 0) {
	fprintf(stderr, "%s: get_http_header()\n", __func__);
	return 0;
    }

    if (request && len) {
	strncpy(request, req, len);
    }

    //fprintf(stderr, "method ws get [%s]\n", req);

    if (!header_get_field(req, "Upgrade", field, sizeof(field))) {
	return 0;
    }

    if (strcasecmp(field, "websocket")) {
//	fprintf(stderr, "wrong Upgrade [%s]\n", field);
	return 0;
    }

    if (!header_get_field(req, "Connection", field, sizeof(field))) {
	return 0;
    }

    if (!strcasestr(field, "Upgrade")) {
	fprintf(stderr, "wrong Connection [%s]\n", field);
	return 0;
    }

    if (!header_get_path(req, field, sizeof(field))) {
	return 0;
    }

    if (channel->path) {
	if (strcmp(field, channel->path)) {
	    fprintf(stderr, "wrong path [%s]\n", field);
	    return 0;
	}
    } else {
	channel->ws_path = strdup(field);
    }

    if (!header_get_field(req, "Sec-WebSocket-Key", field, sizeof(field))) {
	return 0;
    }

    static const char *uuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    strncat(field, uuid, sizeof(field) - 1);

#ifdef ENABLE_SSL
    SHA1((unsigned char *)field, strlen(field), sha1buf);

    char *key_b64 = (char *)simple_connection_base64_encode((const unsigned char *)sha1buf, sizeof(sha1buf), NULL);
#else
    char *key_b64 = (char *)simple_connection_base64_encode((const unsigned char *)field, strlen(field), NULL);
#endif

#ifdef sgi
    sprintf(req, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\nSec-WebSocket-Protocol: binary\r\n\r\n",
	    key_b64);
#else
    snprintf(req, sizeof(req), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\nSec-WebSocket-Protocol: binary\r\n\r\n",
	    key_b64);
#endif

    free(key_b64);

    //fprintf(stderr, "Send req [%s]\n", req);

    if (tcp_write_internal(channel, req, strlen(req)) != strlen(req)) {
	if (request) {
	    *request = 0;
	}
	fprintf(stderr, "%s: tcp_write()\n", __func__);
	return 0;
    }

    return 1;
}

static int http_ws_method_client(tcp_channel *channel)
{
    char key[16];
    char req[1024];

    if (simple_connection_get_random(key, 16, 0) == -1) {
	fprintf(stderr, "%s: get_random()\n", __func__);
	return 0;
    }

    char *key_b64 = (char *)simple_connection_base64_encode((const unsigned char *)key, 16, NULL);

#ifdef sgi
    sprintf(req, "GET %s HTTP/1.1\r\nHost: %s\r\nSec-WebSocket-Version: 13\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Protocol: binary\r\n\r\n",
	    channel->path ? channel->path : "/", channel->host, key_b64);
#else
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nSec-WebSocket-Version: 13\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Protocol: binary\r\n\r\n",
	    channel->path ? channel->path : "/", channel->host, key_b64);
#endif

    free(key_b64);

    //fprintf(stderr, "Send req [%s]\n", req);

    if (tcp_write_internal(channel, req, strlen(req)) != strlen(req)) {
	fprintf(stderr, "%s: tcp_write()\n", __func__);
	return 0;
    }

    if (get_http_header(channel, req, sizeof(req)) <= 0) {
	fprintf(stderr, "%s: get_http_header()\n", __func__);
	return 0;
    }

    //fprintf(stderr, "method ws get [%s]\n", req);

    static const char *reply = "HTTP/1.1 101 Switching Protocols";

    if (strncasecmp(req, reply, strlen(reply))) {
	fprintf(stderr, "\nwebsocket error\n");
	return 0;
    }

    //fprintf(stderr, "\nwebsocket enabled\n");

    return 1;
}

int tcp_connection_upgrade(tcp_channel *u, int connection_method, const char *path, char *request, size_t len)
{
    if (connection_method == SIMPLE_CONNECTION_METHOD_WS) {
	ws_t *ws = malloc(sizeof(ws_t));
	ws->avail = 0;
	ws->pos = 0;
	u->path = (char *)path;
	if ((u->primary_mode == TCP_SSL_CLIENT) || (u->primary_mode == TCP_CLIENT)) {
	    if (http_ws_method_client(u)) {
		u->connection_method = connection_method;
		u->ws = ws;
		return 1;
	    }
	} else if ((u->primary_mode == TCP_SSL_SERVER) || (u->primary_mode == TCP_SERVER)) {
	    if (http_ws_method_server(u, request, len)) {
		u->connection_method = connection_method;
		u->ws = ws;
		return 1;
	    }
	}
	free(ws);
	return 0;
    }

    return 1;
}

static int send_ws_header(tcp_channel *channel, uint8_t opcode, int len)
{
    int sz;
    int blen = len;
    unsigned char *mask;
    ws_t *ws = channel->ws;

    ws->header.b0 = 0x80 | (opcode & 0x0f);
    if (blen <= 125) {
	ws->header.b1 = (uint8_t)blen;
	mask = ws->header.u.m.c;
	sz = 2;
    } else if (blen <= 65535) {
	ws->header.b1 = 0x7e;
	ws->header.u.s16.l16 = WS_HTON16((uint16_t)blen);
	mask = ws->header.u.s16.m16.c;
	sz = 4;
    } else {
	ws->header.b1 = 0x7f;
	ws->header.u.s64.l64 = WS_HTON64(blen);
	mask = ws->header.u.s64.m64.c;
	sz = 10;
    }

    if (channel->primary_mode == TCP_CLIENT || channel->primary_mode == TCP_SSL_CLIENT) {
	ws->header.b1 |= 0x80; // mask flag
	sz += 4;
	if (simple_connection_get_random(mask, 4, 0) == -1) {
	    fprintf(stderr, "%s: get_random()\n", __func__);
	    return 0;
	}
    }

    //write_log("SZ=%d KEY=%02X %02X %02X %02X\n", sz, mask[0], mask[1], mask[2], mask[3]);

    if (tcp_write_internal(channel, (char *)&ws->header, sz) != sz) {
	fprintf(stderr, "%s: tcp_write()\n", __func__);
	return 0;
    }

    return 1;
}

static int recv_ws_header(tcp_channel *channel)
{
    int len;

    ws_t *ws = channel->ws;

    if (tcp_read_internal(channel, (char *)&ws->header.b0, 1) != 1) {
	fprintf(stderr, "%s: tcp_read()\n", __func__);
	return 0;
    }

    if (tcp_read_internal(channel, (char *)&ws->header.b1, 1) != 1) {
	fprintf(stderr, "%s: tcp_read()\n", __func__);
	return 0;
    }

    //fprintf(stderr, "OPCODE %02X %02X\n", ws->header.b0 & 0xFF, ws->header.b1 & 0xFF);

    if ((ws->header.b0 != (0x80 | WS_OPCODE_BINARY_FRAME)) &&
	(ws->header.b0 != (0x80 | WS_OPCODE_CLOSE)) &&
	(ws->header.b0 != WS_OPCODE_CONTINUATION)) {
	fprintf(stderr, "Unknown ws opcode %02X %02X\n", ws->header.b0, ws->header.b1);
	return 0;
    }

    if (ws->header.b0 == WS_OPCODE_CONTINUATION) {
	fprintf(stderr, "Continuation %02X %02X\n", ws->header.b0, ws->header.b1);
	if ((ws->header.b1 & 0x7f) == 0) {
	    return recv_ws_header(channel);
	}
    }

    if (ws->header.b0 == (0x80 | WS_OPCODE_CLOSE)) {
	char buf[ws->header.b1 + 1];
	buf[ws->header.b1] = 0;
	if (tcp_read_internal(channel, buf, ws->header.b1) != ws->header.b1) {
	    fprintf(stderr, "%s: tcp_read()\n", __func__);
	    return 0;
	}

	fprintf(stderr, "Connection closed [%s]\n", buf + 2);

	return 0;
    }

    if ((ws->header.b1 & 0x7f) == 0x7e) {
	if (tcp_read_internal(channel, (char *)&ws->header.u.s16.l16, 2) != 2) {
	    fprintf(stderr, "%s: 0x7e - tcp_read()\n", __func__);
	    return 0;
	}
	len = WS_NTOH16(ws->header.u.s16.l16);
    } else if ((ws->header.b1 & 0x7f) == 0x7f) {
	if (tcp_read_internal(channel, (char *)&ws->header.u.s64.l64, 8) != 8) {
	    fprintf(stderr, "%s: 0x7f - tcp_read()\n", __func__);
	    return 0;
	}
	len = WS_NTOH64(ws->header.u.s64.l64);
    } else {
	len = ws->header.b1 & 0x7f;
    }

    if (ws->header.b1 & 0x80) {
	if ((ws->header.b1 & 0x7f) == 0x7e) {
	    if (tcp_read_internal(channel, (char *)ws->header.u.s16.m16.c, 4) != 4) {
		fprintf(stderr, "%s: 0x7e mask - tcp_read()\n", __func__);
		return 0;
	    }
	} else if ((ws->header.b1 & 0x7f) == 0x7f) {
	    if (tcp_read_internal(channel, (char *)ws->header.u.s64.m64.c, 4) != 4) {
		fprintf(stderr, "%s: 0x7f mask - tcp_read()\n", __func__);
		return 0;
	    }
	} else {
	    if (tcp_read_internal(channel, (char *)ws->header.u.m.c, 4) != 4) {
		fprintf(stderr, "%s: mask - tcp_read()\n", __func__);
		return 0;
	    }
	}
	//fprintf(stderr, "ws mask %08X\n", channel->ws.header.u.m.u);
	//fprintf(stderr, "ws mask?!\n");
    }

    //fprintf(stderr, "ws payload %d\n", len);

    return len;
}

static void ws_mask_data(ws_t *ws, char *data, int len)
{
    int i;
    unsigned char *mask;

    if (!(ws->header.b1 & 0x80)) {
	return;
    }

    switch (ws->header.b1 & 0x7f) {
    case 0x7e: mask = ws->header.u.s16.m16.c; break;
    case 0x7f: mask = ws->header.u.s64.m64.c; break;
    default: mask = ws->header.u.m.c;
    }

    for (i = 0; i < len; i++) {
      data[i] ^= mask[(ws->pos + i) % 4];
    }
}

static int tcp_write_ws(tcp_channel *u, uint8_t opcode, char *buf, size_t len)
{
    char *tmp = alloca(len);

    if (!send_ws_header(u, opcode, len)) {
	return 0;
    }
    ws_t *ws = u->ws;
    ws->pos = 0;

    memcpy(tmp, buf, len);

    if (u->primary_mode == TCP_CLIENT || u->primary_mode == TCP_SSL_CLIENT) {
	ws_mask_data(u->ws, tmp, len);
    }

    if (tcp_write_internal(u, tmp, len) != len) {
	fprintf(stderr, "tcp_write()\n");
	return 0;
    }

    return len;
}


int tcp_write(tcp_channel *u, void *buf, size_t len)
{
    if (u->connection_method == SIMPLE_CONNECTION_METHOD_WS) {
	return tcp_write_ws(u, WS_OPCODE_BINARY_FRAME, buf, len);
    }

    if (tcp_write_internal(u, buf, len) != len) {
	fprintf(stderr, "tcp_write()\n");
	return 0;
    }

    return len;
}

int tcp_read(tcp_channel *u, void *buf, size_t len)
{
    int avail;
    ws_t *ws = NULL;
    char *tmp = alloca(len);

    if (u->connection_method == SIMPLE_CONNECTION_METHOD_WS) {
	ws = u->ws;
	if (ws->avail > 0) {
	    avail = ws->avail;
	} else {
	    if (!(avail = recv_ws_header(u))) {
		return 0;
	    }
	    ws->avail = avail;
	    ws->pos = 0;
	}
	avail = (avail < len) ? avail : len;
    } else {
	avail = len;
    }

    //fprintf(stderr, ">>>>>> avail=%d need=%d\n", avail, len);

    int ret = tcp_read_internal(u, tmp, avail);
    if (ret > 0) {
	if (u->connection_method == SIMPLE_CONNECTION_METHOD_WS) {
	    ws->avail -= ret;
	    ws_mask_data(u->ws, tmp, ret);
	    ws->pos += ret;
	}

	memcpy(buf, tmp, ret);
    }

    return ret;
}
