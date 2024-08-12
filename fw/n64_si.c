#include "n64_si.h"

#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "main.h"
#include "n64.h"
#include "n64_pi.h"
#include "pico/stdlib.h"

static int si_pulse_counter;
static int si_out_pulses;
static unsigned char si_data_bits[324];
static unsigned char si_data_byte[16];

static void cic_dclk_callback(void)
{
    io_irq_ctrl_hw_t *irq_ctrl_base = get_core_num()? &iobank0_hw->proc1_irq_ctrl : &iobank0_hw->proc0_irq_ctrl;

    uint gpio = N64_SI_CLK;
    io_ro_32 *status_reg = &irq_ctrl_base->ints[gpio / 8];
    uint events = (*status_reg >> 4 * (gpio % 8)) & 0xf;

    if (events) {
        static bool skip_pulse = true;
        skip_pulse = !skip_pulse;

        if (si_out_pulses) {
            if (!skip_pulse) {
                if (si_data_bits[si_pulse_counter++]) {
                    gpio_set_dir(N64_SI_DATA, GPIO_IN);
                } else {
                    gpio_set_dir(N64_SI_DATA, GPIO_OUT);
                    gpio_put(N64_SI_DATA, 0);
                }

                si_out_pulses--;
                if (!si_out_pulses) {
                    si_pulse_counter = 0;
                    gpio_set_dir(N64_SI_DATA, GPIO_IN);
                }
            }
        } else {
            if (!skip_pulse) {
                si_data_bits[si_pulse_counter++] = gpio_get(N64_SI_DATA) ? 1 : 0;
                if (si_pulse_counter % 4 == 1 && si_data_bits[si_pulse_counter - 1] == 1) {
                    if (si_pulse_counter > 1) {
                        si_pulse_counter--;
                        if (si_pulse_counter % 32 == 4) {
                            if (*((uint32_t *) & si_data_bits[si_pulse_counter - 4]) == 0x01010100) {
                                //                              printf("Stop bit detected\n");
                                int bit_counter = 0;
                                int byte_counter = 0;
                                unsigned char byte = 0;

                                for (int i = 0; i < si_pulse_counter - 4; i += 4) {
                                    if (*((uint32_t *) & si_data_bits[i]) == 0x01000000) {
                                        byte <<= 1;
                                    } else if (*((uint32_t *) & si_data_bits[i]) == 0x01010100) {
                                        byte = (byte << 1) | 1;
                                    }
                                    bit_counter++;
                                    if (bit_counter == 8) {
                                        si_data_byte[byte_counter++] = byte;
                                        bit_counter = 0;
                                    }
                                }

                                si_out_pulses = 0;

                                // printf("%02X\n", si_data_byte[0]);
                                if (si_data_byte[0] == 0x00 || si_data_byte[0] == 0xff) {
                                    si_data_byte[0] = 0x00;
                                    si_data_byte[1] = (sys64_ctrl_reg & 0x1000) ? 0xc0 : 0x80;
                                    si_data_byte[2] = 0x00;
                                    byte_counter = 3;
                                } else if (si_data_byte[0] == 0x04) {
                                    memmove(&si_data_byte[0], &si_eeprom[si_data_byte[1] << 3], 8);
                                    byte_counter = 8;
                                } else if (si_data_byte[0] == 0x05) {
                                    memmove(&si_eeprom[si_data_byte[1] << 3], &si_data_byte[2], 8);
                                    si_data_byte[0] = 0x00;
                                    byte_counter = 1;
                                } else {
                                    goto cmd_error;
                                }

                                for (int i = 0; i < byte_counter; i++) {
                                    for (int j = 0; j < 8; j++) {
                                        if (si_data_byte[i] & 0x80) {
                                            *((uint32_t *) & si_data_bits[si_out_pulses]) = 0x01010100;
                                        } else {
                                            *((uint32_t *) & si_data_bits[si_out_pulses]) = 0x01000000;
                                        }
                                        si_out_pulses += 4;
                                        si_data_byte[i] <<= 1;
                                    }
                                }
                                *((uint32_t *) & si_data_bits[si_out_pulses]) = 0x01010000;
                                si_out_pulses += 4;
 cmd_error:
                            }
                        }
                    }
                    si_pulse_counter = 0;
                }
            }
        }

        iobank0_hw->intr[gpio / 8] = events << (4 * (gpio % 8));
    }
}

void si_main(void)
{
#ifndef DISABLE_PINS_23_24
    si_pulse_counter = 0;
    si_out_pulses = 0;

    gpio_set_irq_enabled(N64_SI_CLK, GPIO_IRQ_EDGE_RISE, true);

    irq_set_exclusive_handler(IO_IRQ_BANK0, cic_dclk_callback);
    irq_set_enabled(IO_IRQ_BANK0, true);
#endif
}
