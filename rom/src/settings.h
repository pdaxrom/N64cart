#pragma once

#include <stdbool.h>

typedef struct boot_settings {
    bool auto_boot;
    bool consumer_mode;
}boot_settings;

bool boot_settings_load(boot_settings* settings);