/*
 * UDP server
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
    udp_channel *client = udp_open(UDP_SERVER, NULL, 9998);
    if (!client) {
	fprintf(stderr, "udp_open()\n");
	return -1;
    }

    if ((r = udp_read(client, (uint8_t *)buf, BUF_SIZE)) > 0) {
	fprintf(stderr, "buf[%d]=%s\n", r, buf);
    }

    strcpy(buf, "Hello client!");
    if ((r = udp_write(client, (uint8_t *)buf, strlen(buf) + 1)) <= 0) {
	fprintf(stderr, "udp_write()\n");
    }

    udp_close(client);

    return 0;
}
