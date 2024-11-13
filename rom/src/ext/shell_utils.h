#pragma once

/** @brief OS Information Structure. */
typedef struct {
    uint32_t tv_type;
    uint32_t device_type;
    uint32_t device_base;
    uint32_t reset_type;
    uint32_t cic_id;
    uint32_t version;
    uint32_t mem_size;
    uint8_t app_nmi_buffer[64];
    uint32_t __reserved_1[37];
    uint32_t mem_size_6105;
} os_info_t;

#define OS_INFO_BASE                (0x80000300UL)
#define OS_INFO                     ((os_info_t *) OS_INFO_BASE)

/** @brief The Console was powered on using the power switch. */
#define OS_INFO_RESET_TYPE_COLD     (0)
/** @brief The Console was reset using the reset button. */
#define OS_INFO_RESET_TYPE_NMI      (1)

int get_cic_save(char *cartid, int *cic, int *save);

void set_force_tv(short int i);

void simulate_boot(uint32_t cic_chip, uint8_t gBootCic);
