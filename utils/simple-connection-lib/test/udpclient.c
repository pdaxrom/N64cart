/*
 * UDP client
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../src/udp.h"

#define BUF_SIZE	256

int main(int argc, char *argv[])
{
    char buf[BUF_SIZE];
    int r;
    udp_channel *server = udp_open(UDP_CLIENT, "127.0.0.1", 9998);
    if (!server) {
	fprintf(stderr, "udp_open()\n");
	return -1;
    }

    strcpy(buf, "Hello server!");

    if ((r = udp_write(server, (uint8_t *)buf, strlen(buf) + 1)) <= 0) {
	fprintf(stderr, "udp_write()\n");
    }

    if ((r = udp_read(server, (uint8_t *)buf, BUF_SIZE)) > 0) {
	fprintf(stderr, "buf[%d]=%s\n", r, buf);
    }

    udp_close(server);

    return 0;
}
