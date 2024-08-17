/**
 * Copyright (c) 2022-2024 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>

#include "flashrom.h"
#include "hardware/resets.h"
#include "hardware/structs/ssi.h"
#include "main.h"
#include "n64.h"
#include "n64_pi.pio.h"
#include "pico/multicore.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "romfs/romfs.h"
#ifdef RGB_LED
#include "rgb_led.h"
#endif

//
// 0x0001 - force ssi cs
// 0x0010 - enable qspi
// 0x0100 - unlock sram access
// 0x1000 - enable 16Kbit eeprom
//
uint16_t sys64_ctrl_reg = 0x11;

//
// 0x0001 - reset usb block
// 0x0010 - enable usb interrupt
// 0x0080 - enable bswap32
// 0x8000 - interrupt flag
//
uint16_t usb64_ctrl_reg = 0x0000;

uint8_t pi_sram[SRAM_1MBIT_SIZE + ROMFS_FLASH_SECTOR * 4 * 2 * 2 + 2048 + 512];
uint16_t *pi_rom_lookup = (uint16_t *) & pi_sram[SRAM_1MBIT_SIZE];
uint8_t *si_eeprom = &pi_sram[SRAM_1MBIT_SIZE + ROMFS_FLASH_SECTOR * 4 * 2 * 2];

static uint16_t *sram_16 = (uint16_t *) pi_sram;
static const uint16_t *rom_lookup = (uint16_t *) & pi_sram[SRAM_1MBIT_SIZE];

void backup_rom_lookup(void)
{
    memmove(&pi_rom_lookup[ROMFS_FLASH_SECTOR * 4], pi_rom_lookup, ROMFS_FLASH_SECTOR * 4 * 2);
}

void restore_rom_lookup(void)
{
    memmove(pi_rom_lookup, &pi_rom_lookup[ROMFS_FLASH_SECTOR * 4], ROMFS_FLASH_SECTOR * 4 * 2);
    __dmb();
}

static inline uint32_t resolve_sram_address(uint32_t address)
{
    if (sys64_ctrl_reg & 0x100) {
        return address & 0x3ffff;
    }

    uint32_t bank = (address >> 18) & 0x3;
    uint32_t resolved_address;

    if (bank) {
        resolved_address = address & (SRAM_256KBIT_SIZE - 1);
        resolved_address |= bank << 15;
    } else {
        resolved_address = address & (SRAM_1MBIT_SIZE - 1);
    }

    return resolved_address;
}

void n64_pi(void)
{
    PIO pio = pio0;
    pio_clear_instruction_memory(pio);
    uint offset = pio_add_program(pio, &pi_program);
    pi_program_init(pio, 0, offset);
    pio_sm_set_enabled(pio, 0, true);

    // Wait for reset to be released
    while (gpio_get(N64_COLD_RESET) == 0) {
        tight_loop_contents();
    }

    uint32_t last_addr = 0;
    uint32_t word;

    uint32_t mapped_addr;
    uint32_t sram_address;

    uint32_t flags;
    uint16_t ctrl_reg;
    uint8_t byte;
    uintptr_t fw_binary_end = (uintptr_t) & __flash_binary_end;

    // uint32_t addr = pio_sm_get_blocking(pio, 0);
    while ((pio->fstat & 0x100) != 0) {
    }
    uint32_t addr = pio->rxf[0];

    do {
        last_addr = addr;

        if (last_addr >= 0x10000000 && last_addr <= 0x1FBFFFFF) {
            do {
                mapped_addr = (rom_lookup[(last_addr & 0x3ffffff) >> 12]) << 12 | (last_addr & 0xfff);
                word = flash_quad_read16(mapped_addr);

                // addr = pio_sm_get_blocking(pio, 0);
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    // pio_sm_put(pio, 0, word);
                    pio->txf[0] = word;
                } else if (!(addr & 1)) {
                    break;
                }
                last_addr += 2;
            } while (true);
#if PI_SRAM
        } else if (last_addr >= 0x08000000 && last_addr <= 0x0FFFFFFF) {
            sram_address = resolve_sram_address(last_addr) >> 1;
            do {
                // addr = pio_sm_get_blocking(pio, 0);
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    // pio_sm_put(pio, 0, sram_16[sram_address++]);
                    pio->txf[0] = sram_16[sram_address++];
                } else if (addr & 1) {
                    sram_16[sram_address++] = addr >> 16;
                } else {
                    break;
                }
                last_addr += 2;
            } while (true);
#endif
#if PI_USBCTRL
        } else if (last_addr >= 0x1fe00000 && last_addr <= 0x1fefffff) {
            sram_address = (last_addr & 0xfffff) | USBCTRL_BASE;
            do {
                word = *((uint32_t *) sram_address);
                if (usb64_ctrl_reg & 0x80) {
                    word = __builtin_bswap32(word);
                }

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = word >> 16;
                } else if (addr & 1) {
                    word = addr & 0xffff0000;
                } else {
                    break;
                }

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = word & 0xffff;
                } else if (addr & 1) {
                    word = (addr >> 16) | word;
                    if (usb64_ctrl_reg & 0x80) {
                        word = __builtin_bswap32(word);
                    }
                    *((uint32_t *) sram_address) = word;
                } else {
                    break;
                }

                sram_address += 4;
            } while (true);
#endif
        } else {
            do {
                // addr = pio_sm_get_blocking(pio, 0);
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    if (last_addr == 0x1fd01002) {
                        word = ((uart_get_hw(UART_ID)->fr & UART_UARTFR_TXFF_BITS) ? 0x00 : 0x02) | ((uart_get_hw(UART_ID)->fr & UART_UARTFR_RXFE_BITS) ? 0x00 : 0x01) | 0x00f0;
                    } else if (last_addr == 0x1fd01006) {
                        word = uart_get_hw(UART_ID)->dr;
                    } else if (last_addr == 0x1fd0100e) {
                        word = sys64_ctrl_reg;
                    } else if (last_addr == 0x1fd01012) {
                        flags = ssi_hw->sr;
                        word = ((flags & SSI_SR_TFNF_BITS) ? 0x01 : 0x00) | ((flags & SSI_SR_RFNE_BITS) ? 0x02 : 0x00);
                    } else if (last_addr == 0x1fd01016) {
                        byte = (uint8_t) ssi_hw->dr0;
                        word = byte;
                    } else if (last_addr == 0x1fd0101a) {
                        word = (((fw_binary_end - XIP_BASE) + 4095) & ~4095) >> 12;
#if PI_USBCTRL
                    } else if (last_addr == 0x1fd01022) {
                        word = usb64_ctrl_reg;
                        gpio_put(N64_INT, 1);
                        usb64_ctrl_reg &= ~0x8000;
                        if (usb64_ctrl_reg & 0x10) {
                            *((io_rw_32 *) (PPB_BASE + M0PLUS_NVIC_ISER_OFFSET)) = 1 << USBCTRL_IRQ;
                        }
#endif
                    } else {
                        word = 0xdead;
                    }
                    // pio_sm_put(pio, 0, word);
                    pio->txf[0] = word;
                    last_addr += 2;
                } else if (addr & 1) {
                    if (last_addr == 0x1fd01006) {
                        uart_get_hw(UART_ID)->dr = (addr >> 16) & 0xff;
#ifdef PICO_DEFAULT_LED_PIN
#if PICO_LED_WS2812 == 1
                    } else if (last_addr == 0x1fd01008) {
                        sram_address = addr & 0xffff0000;
                    } else if (last_addr == 0x1fd0100a) {
                        set_rgb_led(sram_address | (addr >> 16));
#else
                    } else if (last_addr == 0x1fd0100a) {
                        gpio_put(LED_PIN, (addr >> 16) & 0x01);
#endif
#endif
                    } else if (last_addr == 0x1fd0100e) {
                        ctrl_reg = addr >> 16;

                        if (ctrl_reg & 0x01) {
                            if (!(sys64_ctrl_reg & 0x01)) {
                                flash_cs_force(1);
                            }
                        } else {
                            if ((sys64_ctrl_reg & 0x01)) {
                                flash_cs_force(0);
                            }
                        }

                        if (ctrl_reg & 0x10) {
                            if (!(sys64_ctrl_reg & 0x10)) {
                                flash_quad_cont_read_mode();
                            }
                        } else {
                            if (sys64_ctrl_reg & 0x10) {
                                flash_quad_exit_cont_read_mode();
                                flash_spi_mode();
                            }
                        }

                        sys64_ctrl_reg = ctrl_reg;
                    } else if (last_addr == 0x1fd01016) {
                        byte = (addr >> 16) & 0xff;
                        ssi_hw->dr0 = byte;
#if PI_USBCTRL
                    } else if (last_addr == 0x1fd01022) {
                        ctrl_reg = addr >> 16;

                        if (ctrl_reg & 0x0001) {
                            if (!(usb64_ctrl_reg & 0x0001)) {
                                reset_block(RESETS_RESET_USBCTRL_BITS);
                            }
                        } else {
                            if (usb64_ctrl_reg & 0x0001) {
                                unreset_block_wait(RESETS_RESET_USBCTRL_BITS);
                            }
                        }

                        if (ctrl_reg & 0x0010) {
                            if (!(usb64_ctrl_reg & 0x0010)) {
                                // irq_set_mask_enabled(1 << USBCTRL_IRQ, true);
                                *((io_rw_32 *) (PPB_BASE + M0PLUS_NVIC_ICPR_OFFSET)) = 1 << USBCTRL_IRQ;
                                *((io_rw_32 *) (PPB_BASE + M0PLUS_NVIC_ISER_OFFSET)) = 1 << USBCTRL_IRQ;
                            }
                        } else {
                            if (usb64_ctrl_reg & 0x0010) {
                                // irq_set_mask_enabled(1 << USBCTRL_IRQ, false);
                                *((io_rw_32 *) (PPB_BASE + M0PLUS_NVIC_ICER_OFFSET)) = 1 << USBCTRL_IRQ;
                            }
                        }

                        usb64_ctrl_reg = ctrl_reg;
#endif
                    }

                    last_addr += 2;
                } else {
                    break;
                }
            } while (true);
        }
    } while (1);
}
