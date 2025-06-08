#pragma once

#define TCP_PORT	6464

enum {
    USB_CMD = 0,
    USB_ERASE_SECTOR,
    USB_READ_SECTOR,
    USB_WRITE_SECTOR,
};

struct __attribute__((__packed__)) sector_info {
    uint32_t offset;
    uint32_t length;
};
