/**
 * Copyright (c) 2022-2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

void n64_pi(void);
void set_pi_bus_freq(uint16_t freq);
uint16_t get_pi_bus_freq(void);

void backup_rom_lookup(void);
void restore_rom_lookup(void);
