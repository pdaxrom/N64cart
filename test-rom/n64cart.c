#include <stdint.h>
#include <libdragon.h>
#include "n64cart.h"

uint8_t n64cart_uart_getc(void)
{
    while (!(io_read(N64CART_UART_CTRL) & N64CART_UART_RX_AVAIL)) ;

    return io_read(N64CART_UART_RXTX) & 0xff;
}

void n64cart_uart_putc(uint8_t data)
{
    while (!(io_read(N64CART_UART_CTRL) & N64CART_UART_TX_FREE));

    io_write(N64CART_UART_RXTX, data);
}

void n64cart_uart_puts(char *str)
{
    while (*str) {
	char c = *str++;
	n64cart_uart_putc(c);
	if (c == '\n') {
	    n64cart_uart_putc('\r');
	}
    }
}

uint16_t n64cart_get_rom_pages(void)
{
    return io_read(N64CART_ROM_PAGE_CTRL) >> 16;
}

void n64cart_set_rom_page(uint16_t page)
{
    io_write(N64CART_ROM_PAGE_CTRL, page);
}
