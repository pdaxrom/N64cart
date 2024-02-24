#pragma once

int get_cic_save(char *cartid, int *cic, int *save);

void simulate_boot(uint32_t cic_chip, uint8_t gBootCic);
