#ifndef __MAIN_H__
#define __MAIN_H__

#define UART_TX_PIN (28)
#define UART_RX_PIN (29)	/* not available on the pico */
#define UART_ID     uart0
#define BAUD_RATE   115200

extern volatile uint32_t rom_pages;
extern volatile uint32_t rom_start[4];
extern volatile uint32_t rom_size[4];

void n64_pi_restart(void);

#endif
