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
#if defined(PICO_DEFAULT_LED_PIN) && (PICO_LED_WS2812 == 1)
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

    enum {
        FLASH_CMD_NONE = 0,
        FLASH_CMD_CHIP_ERASE = 0x3c,
        FLASH_CMD_SECTOR_ERASE = 0x4b,
        FLASH_CMD_EXECUTE_ERASE = 0x78,
        FLASH_CMD_PROGRAM_PAGE = 0xa5,
        FLASH_CMD_PAGE_PROGRAM = 0xb4,
        FLASH_CMD_STATUS = 0xd2,
        FLASH_CMD_ID = 0xe1,
        FLASH_CMD_READ_ARRAY = 0xf0
    };

    uint16_t flash_id[4] = { 0x1111, 0x8001, 0x00c2, 0x001e };
    uint16_t flash_buffer[64];

    uint16_t fram_mode = 0;
    uint16_t fram_mode_tmp = 0;
    uint16_t fram_page = 0;
    uint32_t fram_erase_counter = 0;

    uint32_t fram_status = 0;
    uint32_t fram_word = 0;

    uint32_t flags;
    uint16_t ctrl_reg;
    static const uintptr_t fw_binary_end = (uintptr_t) & __flash_binary_end;

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

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = word;
                } else if (!(addr & 1)) {
                    break;
                }
                last_addr += 2;
            } while (true);
#if PI_SRAM
        } else if (last_addr >= 0x08000000 && last_addr <= 0x0FFFFFFF) {
            if (!(sys64_ctrl_reg & 0x200)) {
                sram_address = resolve_sram_address(last_addr) >> 1;
                do {
                    while ((pio->fstat & 0x100) != 0) {
                    }
                    addr = pio->rxf[0];

                    if (addr == 0) {
                        pio->txf[0] = sram_16[sram_address++];
                    } else if (addr & 1) {
                        sram_16[sram_address++] = addr >> 16;
                    } else {
                        break;
                    }
                } while (true);
            } else if (!(last_addr & 0x00010000)) {
                if (fram_mode == FLASH_CMD_READ_ARRAY) {
                    sram_address = last_addr & 0xffff;
                    do {
                        while ((pio->fstat & 0x100) != 0) {
                        }
                        addr = pio->rxf[0];

                        if (addr == 0) {
                            pio->txf[0] = sram_16[sram_address++];
                        } else if (addr & 1) {
                            //sram_16[sram_address++] = addr >> 16;
                        } else {
                            break;
                        }
                        sram_address &= 0xffff;
                    } while (true);
                } else if (fram_mode == FLASH_CMD_PAGE_PROGRAM) {
                    sram_address = (last_addr & 0x3f) >> 1;
                    do {
                        while ((pio->fstat & 0x100) != 0) {
                        }
                        addr = pio->rxf[0];

                        if (addr == 0) {
                            pio->txf[0] = 0;
                        } else if (addr & 1) {
                            flash_buffer[sram_address++] = addr >> 16;
                        } else {
                            break;
                        }
                        sram_address &= 0x3f;
                    } while (true);
                } else if (fram_mode == FLASH_CMD_ID) {
                    sram_address = 0;
                    do {
                        while ((pio->fstat & 0x100) != 0) {
                        }
                        addr = pio->rxf[0];

                        if (addr == 0) {
                            pio->txf[0] = flash_id[sram_address++];
                        } else if (addr & 1) {
                            //flash_buffer[sram_address++] = addr >> 16;
                        } else {
                            break;
                        }
                        sram_address &= 0x3;
                    } while (true);
                } else {
                    fram_word = fram_status;
                    do {
                        while ((pio->fstat & 0x100) != 0) {
                        }
                        addr = pio->rxf[0];

                        if (addr == 0) {
                            pio->txf[0] = fram_word >> 16;
                        } else if (addr & 1) {
                            if (fram_mode == FLASH_CMD_STATUS) {
                                fram_status = addr & 0xffff0000;
                            }
                        } else {
                            break;
                        }

                        while ((pio->fstat & 0x100) != 0) {
                        }
                        addr = pio->rxf[0];

                        if (addr == 0) {
                            pio->txf[0] = fram_word & 0xffff;
                            if (fram_erase_counter) {
                                memset(&sram_16[fram_erase_counter], 0xff, 0x1000);
                                fram_erase_counter += 0x800;
                                if (fram_erase_counter == 0x10000) {
                                    fram_erase_counter = 0;
                                    fram_status = 0x08;
                                }
                            }
                        } else if (addr & 1) {
                            if (fram_mode == FLASH_CMD_STATUS) {
                                fram_status = (addr >> 16) | fram_status;
                            }
                        } else {
                            break;
                        }
                    } while (true);
                }
            } else {
                do {
                    while ((pio->fstat & 0x100) != 0) {
                    }
                    addr = pio->rxf[0];

                    if (addr == 0) {
                        pio->txf[0] = fram_word >> 16;
                    } else if (addr & 1) {
                        fram_word = addr & 0xffff0000;
                    } else {
                        break;
                    }

                    while ((pio->fstat & 0x100) != 0) {
                    }
                    addr = pio->rxf[0];

                    if (addr == 0) {
                        pio->txf[0] = fram_word & 0xffff;
                    } else if (addr & 1) {
                        fram_word = (addr >> 16) | fram_word;
                        fram_mode_tmp = fram_word >> 24;

                        if (fram_mode_tmp == FLASH_CMD_EXECUTE_ERASE) {
                            if (fram_mode == FLASH_CMD_CHIP_ERASE) {
                                memset(sram_16, 0xff, 0x1000);
                                fram_erase_counter = 0x800;
                                fram_status = 0x02;
                            } else if (fram_mode == FLASH_CMD_SECTOR_ERASE) {
                                memset(&sram_16[fram_page << 6], 0xff, 128);
                                fram_status = 0x08;
                            }
                            fram_mode = fram_mode_tmp;
                            continue;
                        }

                        fram_mode = fram_mode_tmp;

                        if (fram_word == FLASH_CMD_ID) {
                            continue;
                        } else if (fram_mode == FLASH_CMD_SECTOR_ERASE) {
                            fram_page = fram_word & 0x3ff;
                            continue;
                        } else if (fram_mode == FLASH_CMD_PROGRAM_PAGE) {
                            memmove(&sram_16[(fram_word & 0x3ff) << 6], flash_buffer, 128);
                            fram_status = 0x04;
                            continue;
                        }
                        continue;
                    } else {
                        break;
                    }
                } while (true);
                continue;
            }
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
        } else if (last_addr == 0x1fd01000) {
            do {
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = 0;
                } else if (addr & 1) {
                    word = addr & 0xffff0000;
                } else {
                    break;
                }

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = ((uart_get_hw(UART_ID)->fr & UART_UARTFR_TXFF_BITS) ? 0x00 : 0x02) | ((uart_get_hw(UART_ID)->fr & UART_UARTFR_RXFE_BITS) ? 0x00 : 0x01) | 0x00f0;
                } else if (addr & 1) {
                    word = (addr >> 16) | word;
                } else {
                    break;
                }
            } while (true);
        } else if (last_addr == 0x1fd01004) {
            do {
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = 0;
                } else if (addr & 1) {
                    word = addr & 0xffff0000;
                } else {
                    break;
                }

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = uart_get_hw(UART_ID)->dr;
                } else if (addr & 1) {
                    uart_get_hw(UART_ID)->dr = (addr >> 16) & 0xff;
                } else {
                    break;
                }
            } while (true);
        } else if (last_addr == 0x1fd01008) {
            static uint32_t led_reg = 0;
            do {
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = led_reg >> 16;
                } else if (addr & 1) {
                    led_reg = addr & 0xffff0000;
                } else {
                    break;
                }

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = led_reg & 0xffff;
                } else if (addr & 1) {
                    led_reg = (addr >> 16) | led_reg;
#ifdef PICO_DEFAULT_LED_PIN
#if PICO_LED_WS2812 == 1
                    set_rgb_led(led_reg);
#else
                    gpio_put(PICO_DEFAULT_LED_PIN, led_reg & 0x01);
#endif
#endif
                } else {
                    break;
                }
            } while (true);
        } else if (last_addr == 0x1fd0100c) {
            do {
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = 0;
                } else if (addr & 1) {
                    word = addr & 0xffff0000;
                } else {
                    break;
                }

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = sys64_ctrl_reg;
                } else if (addr & 1) {
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
                } else {
                    break;
                }
            } while (true);
        } else if (last_addr == 0x1fd01010) {
            do {
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = 0;
                } else if (addr & 1) {
                    word = addr & 0xffff0000;
                } else {
                    break;
                }

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    flags = ssi_hw->sr;
                    pio->txf[0] = ((flags & SSI_SR_TFNF_BITS) ? 0x01 : 0x00) | ((flags & SSI_SR_RFNE_BITS) ? 0x02 : 0x00);
                } else if (addr & 1) {
                    word = (addr >> 16) | word;
                } else {
                    break;
                }
            } while (true);
        } else if (last_addr == 0x1fd01014) {
            do {
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = 0;
                } else if (addr & 1) {
                    word = addr & 0xffff0000;
                } else {
                    break;
                }

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] =  ssi_hw->dr0;
                } else if (addr & 1) {
                    ssi_hw->dr0 = addr >> 16;
                } else {
                    break;
                }
            } while (true);
        } else if (last_addr == 0x1fd01018) {
            static const uint32_t fw_size = fw_binary_end - XIP_BASE;
            do {
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = fw_size >> 16;
                } else if (addr & 1) {
                    word = addr & 0xffff0000;
                } else {
                    break;
                }

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = fw_size & 0xffff;
                } else if (addr & 1) {
                    word = (addr >> 16) | word;
                } else {
                    break;
                }
            } while (true);
        } else if (last_addr == 0x1fd01020) {
            do {
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = 0;
                } else if (addr & 1) {
                    word = addr & 0xffff0000;
                } else {
                    break;
                }

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = usb64_ctrl_reg;
#if PI_USBCTRL
                    gpio_put(N64_INT, 1);
                    usb64_ctrl_reg &= ~0x8000;
                    if (usb64_ctrl_reg & 0x10) {
                        *((io_rw_32 *) (PPB_BASE + M0PLUS_NVIC_ISER_OFFSET)) = 1 << USBCTRL_IRQ;
                    }
#endif
                } else if (addr & 1) {
                    ctrl_reg = addr >> 16;

#if PI_USBCTRL
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
#endif
                    usb64_ctrl_reg = ctrl_reg;
                } else {
                    break;
                }
            } while (true);
        } else {
            do {
                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

                if (addr == 0) {
                    pio->txf[0] = 0xdead;
                } else if (addr & 1) {
                    word = addr & 0xffff0000;
                } else {
                    break;
                }

                while ((pio->fstat & 0x100) != 0) {
                }
                addr = pio->rxf[0];

#ifdef DEBUG_PI
                set_rgb_led(0xff0000);
                printf("%08X\n", last_addr);
#endif
                if (addr == 0) {
                    pio->txf[0] = 0xbeef;
                } else if (addr & 1) {
                    word = (addr >> 16) | word;
                } else {
                    break;
                }
            } while (true);
        }
    } while (1);
}
