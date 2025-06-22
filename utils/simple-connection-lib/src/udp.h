/*
 *  UDP IO wrapper
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

#ifndef UDP_H
#define UDP_H

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#else
#include <windows.h>
#endif
#include <inttypes.h>

enum {
    UDP_SERVER = 0,
    UDP_CLIENT
};

typedef struct _udp_forward {
    struct sockaddr_in addr;
    char 	*label;
    int 	used;
    uint32_t	total;
    struct _udp_forward *next;
} udp_forward;

typedef struct _udp_channel {
    struct sockaddr_in my_addr;
    struct sockaddr_in *inp_addr;
    struct sockaddr_in *out_addr;
    int s;
    int mode;
    udp_forward	*forward;
} udp_channel;

#define udp_fd(u) (u->s)

#ifdef __cplusplus
extern "C" {
#endif

udp_channel *udp_open(int mode, char *addr, int port);
int udp_read(udp_channel *u, void *buf, size_t len);
int udp_write(udp_channel *u, void *buf, size_t len);
int udp_close(udp_channel *u);

int udp_read_src(udp_channel *u, void *buf, size_t len);
void udp_commit_dst(udp_channel *u);

int udp_forward_add(udp_channel *u, char *label);
int udp_forward_write(udp_channel *u, char *label, void *buf, size_t len);
void udp_forward_show(udp_channel *u);
void udp_forward_remove_inactive(udp_channel *u);

#ifdef __cplusplus
}
#endif

#endif
