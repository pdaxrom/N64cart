/*
 * TCP server
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifndef _WIN32
#include <sys/select.h>
#endif

#include "../src/tcp.h"

#define BUF_SIZE	256

int main(int argc, char *argv[])
{
    char buf[BUF_SIZE];
    int r;
    tcp_channel *server = tcp_open(TCP_SERVER, NULL, 9998, NULL, NULL);
    if (!server) {
	fprintf(stderr, "tcp_open()\n");
	return -1;
    }

    tcp_channel *client = tcp_accept(server);
    if (!client) {
	fprintf(stderr, "tcp_accept()\n");
	return -1;
    }

    if (!tcp_connection_upgrade(client, SIMPLE_CONNECTION_METHOD_WS, "/", NULL, 0)) {
	fprintf(stderr, "tcp_connection_upgrade()\n");
	return -1;
    }

    if ((r = tcp_read(client, buf, BUF_SIZE)) > 0) {
	fprintf(stderr, "buf[%d]=%s\n", r, buf);
    }

    strcpy(buf, "Hello client!");
    if ((r = tcp_write(client, buf, strlen(buf) + 1)) <= 0) {
	fprintf(stderr, "tcp_write()\n");
    }

    tcp_close(client);
    tcp_close(server);

    return 0;
}
