#ifndef __MAIN_H__
#define __MAIN_H__

#define PI_SRAM 1

#define UART_TX_PIN (28)
#define UART_RX_PIN (29)	/* not available on the pico */
#define UART_ID     uart0
#define BAUD_RATE   115200

#define SRAM_256KBIT_SIZE         0x00008000
#define SRAM_768KBIT_SIZE         0x00018000
#define SRAM_1MBIT_SIZE           0x00020000

extern volatile uint32_t rom_pages;
extern volatile uint32_t rom_start[4];
extern volatile uint32_t rom_size[4];

void n64_pi_restart(void);

#if PI_SRAM
extern uint8_t sram_8[];

void n64_save_sram(void);
#endif

#endif
