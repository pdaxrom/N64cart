/*
 * Simple udp chat program
 * Start server: udpchat -server
 * Start client: udpchat
 */
 
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/select.h>
#endif
#include <unistd.h>
#include <string.h>

#include "../src/udp.h"

#define BUF_SIZE	256

#define DEFAULT_PORT	9998

int main(int argc, char *argv[])
{
    char buf[BUF_SIZE];
    int r;
    int chat_server = 0;
    char *bind_addr = "0.0.0.0";
    char *remote_addr = "127.0.0.1";
    int port = DEFAULT_PORT;

    while (argc > 1) {
	if (!strcmp(argv[1], "-h")) {
	    fprintf(stderr, "Usage : %s [-server [-bind <IP address>]] [-remote <Server IP>] [-port <Port>]\n", argv[0]);
            return -1;
	} else if (!strcmp(argv[1], "-server"))
	    chat_server = 1;
	else if (!strcmp(argv[1], "-bind")) {
	    bind_addr = argv[2];
	    argc--;
	    argv++;
	} else if (!strcmp(argv[1], "-remote")) {
	    remote_addr = argv[2];
	    argc--;
	    argv++;
	} else if (!strcmp(argv[1], "-port")) {
	    port = atoi(argv[2]);
	    argc--;
	    argv++;
	}
	argc--;
	argv++;
    }

    if (chat_server)
	fprintf(stderr, "Server: bind address %s, port %d\n", bind_addr, port);
    else
	fprintf(stderr, "Client: remote address %s, port %d\n", remote_addr, port);

    udp_channel *udp = udp_open(chat_server?UDP_SERVER:UDP_CLIENT, chat_server?bind_addr:remote_addr, port);
    if (!udp) {
	fprintf(stderr, "udp_open()\n");
	return -1;
    }

    if (!chat_server) {
	strcpy(buf, "Hello server!");

	if ((r = udp_write(udp, (uint8_t *)buf, strlen(buf) + 1)) <= 0) {
	    fprintf(stderr, "udp_write()\n");
	}
    }

    while (1) {
	struct timeval tv = {1, 0};
	fd_set readfs;
	FD_ZERO(&readfs);
	FD_SET(udp_fd(udp), &readfs);
	FD_SET(0, &readfs);

	if ((r = select(udp_fd(udp) + 1, &readfs, NULL, NULL, &tv)) > 0) {
	    if (FD_ISSET(udp_fd(udp), &readfs)) {
		if ((r = udp_read(udp, (uint8_t *)buf, BUF_SIZE)) > 0) {
		    write(1, buf, strlen(buf));
		}
	    }
	    if (FD_ISSET(0, &readfs)) {
		if ((r = read(0, buf, BUF_SIZE)) > 0) {
		    buf[r] = 0;
		    if ((r = udp_write(udp, (uint8_t *)buf, strlen(buf) + 1)) <= 0) {
			fprintf(stderr, "udp_write()\n");
		    }
		}
	    }
	}
    }

    udp_close(udp);

    return 0;
}
