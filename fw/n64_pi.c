#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "flashrom.h"
#include "main.h"
#include "n64_pi.pio.h"
#include "n64.h"

// The rom to load in normal .z64, big endian, format
#include "rom.h"
static uint16_t *rom_file_16;

static inline uint32_t swap16(uint32_t value)
{
    // 0x11223344 => 0x33441122
    return (value << 16) | (value >> 16);
}

static inline uint32_t swap8(uint16_t value)
{
    // 0x1122 => 0x2211
    return (value << 8) | (value >> 8);
}

void n64_pi(void)
{
    rom_file_16 = (uint16_t *) rom_file;

    PIO pio = pio0;
    pio_clear_instruction_memory(pio);
    uint offset = pio_add_program(pio, &pi_program);
    pi_program_init(pio, 0, offset);
    pio_sm_set_enabled(pio, 0, true);

    gpio_put(LED_PIN, 0);

    // Wait for reset to be released
    while (gpio_get(N64_COLD_RESET) == 0) {
	tight_loop_contents();
    }

    uint32_t last_addr = 0;
    uint32_t word;

    uint32_t addr = pio_sm_get_blocking(pio, 0);
    do {
	if (addr == 0) {
	    //READ
#if 1
	    if (last_addr == 0x10000000) {
		// Configure bus to run slowly.
		// This is better patched in the rom, so we won't need a branch here.
		// But let's keep it here so it's easy to import roms easily.
		// 0x8037FF40 in big-endian
		//word = 0x40FF3780;
		//word = 0x40203780;
		////word = 0x40123780;
		//word = 0x401c3780;
		word = 0x3780;
		pio_sm_put(pio, 0, swap8(word));
		last_addr += 2;

		//word = 0x40FF;
		word = 0x401c;
		//word = 0x4012;
		addr = pio_sm_get_blocking(pio, 0);
		if (addr == 0) {
		    goto hackentry;
		}

		continue;
	    } else
#endif
	    if (last_addr >= 0x10000000 && last_addr <= 0x1FBFFFFF) {
		do {
		    word = rom_file_16[(last_addr & 0xFFFFFF) >> 1];
 hackentry:
		    pio_sm_put(pio, 0, swap8(word));
		    last_addr += 2;
		    addr = pio_sm_get_blocking(pio, 0);
		} while (addr == 0);

		continue;
	    } else if (last_addr == 0x1fd01002) {
		word =
		    ((uart_get_hw(UART_ID)->fr & UART_UARTFR_TXFF_BITS) ? 0x00 : 0x0200) |
		    ((uart_get_hw(UART_ID)->fr & UART_UARTFR_RXFE_BITS) ? 0x00 : 0x0100) | 0xf000;
	    } else if (last_addr == 0x1fd01006) {
		word = uart_get_hw(UART_ID)->dr << 8;
	    } else if (last_addr == 0x1fd0100c) {
		word = rom_pages << 8;
	    } else {
		word = 0xdead;
	    }
	    pio_sm_put(pio, 0, swap8(word));
	    last_addr += 2;
	} else if (addr & 0x1) {
	    // WRITE
	    if (last_addr == 0x1fd01006) {
		uart_get_hw(UART_ID)->dr = (addr >> 16) & 0xff;
	    } else if (last_addr == 0x1fd0100a) {
		gpio_put(LED_PIN, (addr >> 16) & 0x01);
	    } else if (last_addr == 0x1fd0100e) {
		int page = (addr >> 16);
		if (page < rom_pages) {
		    rom_file_16 = (uint16_t *) (0x10000000 + rom_start[page]);
		    flash_set_ea_reg(page);
		}
	    }

	    last_addr += 2;
	} else {
	    last_addr = addr;
	}
	addr = pio_sm_get_blocking(pio, 0);
    } while (1);
}
