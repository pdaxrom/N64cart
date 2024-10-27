#pragma once

int get_cic_save(char *cartid, int *cic, int *save);

void set_force_tv(short int i);

void simulate_boot(uint32_t cic_chip, uint8_t gBootCic);
