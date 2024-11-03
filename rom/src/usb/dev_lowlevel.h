/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef DEV_LOWLEVEL_H_
#define DEV_LOWLEVEL_H_

#include "usb_common.h"

// Struct in which we keep the endpoint configuration
typedef void (*usb_ep_handler)(uint8_t * buf, uint16_t len);
struct usb_endpoint_configuration {
    const struct usb_endpoint_descriptor *descriptor;
    usb_ep_handler handler;

    // Pointers to endpoint + buffer control registers
    // in the USB controller DPSRAM
    volatile uint32_t *endpoint_control;
    volatile uint32_t *buffer_control;
    volatile uint8_t *data_buffer;

    // Toggle after each packet (unless replying to a SETUP)
    uint8_t next_pid;
};

// Struct in which we keep the device configuration
struct usb_device_configuration {
    const struct usb_device_descriptor *device_descriptor;
    const struct usb_interface_descriptor *interface_descriptor;
    const struct usb_configuration_descriptor *config_descriptor;
    const unsigned char *lang_descriptor;
    const unsigned char **descriptor_strings;
    const uint8_t *ms_os_desc_string;
    const uint8_t *ms_winusb_feature_descriptor;
    // USB num endpoints is 16
    struct usb_endpoint_configuration endpoints[USB_NUM_ENDPOINTS];
};

#define EP0_IN_ADDR  (USB_DIR_IN  | 0)
#define EP0_OUT_ADDR (USB_DIR_OUT | 0)
#define EP1_OUT_ADDR (USB_DIR_OUT | 1)
#define EP2_IN_ADDR  (USB_DIR_IN  | 2)

// EP0 IN and OUT
static const struct usb_endpoint_descriptor ep0_out = {
    .bLength = sizeof(struct usb_endpoint_descriptor),
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = EP0_OUT_ADDR,   // EP number 0, OUT from host (rx to device)
    .bmAttributes = USB_TRANSFER_TYPE_CONTROL,
    .wMaxPacketSize = REVERSER16(64),
    .bInterval = 0
};

static const struct usb_endpoint_descriptor ep0_in = {
    .bLength = sizeof(struct usb_endpoint_descriptor),
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = EP0_IN_ADDR,    // EP number 0, OUT from host (rx to device)
    .bmAttributes = USB_TRANSFER_TYPE_CONTROL,
    .wMaxPacketSize = REVERSER16(64),
    .bInterval = 0
};

// Descriptors
static const struct usb_device_descriptor device_descriptor = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = REVERSER16(0x0200),       // USB 2.0 device (WCID requirement)
    .bDeviceClass = 0,          // Specified in interface descriptor
    .bDeviceSubClass = 0,       // No subclass
    .bDeviceProtocol = 0,       // No protocol
    .bMaxPacketSize0 = 64,      // Max packet size for ep0
    .idVendor = REVERSER16(0x1209),     // Your vendor id
    .idProduct = REVERSER16(0x6800),    // Your product ID
    .bcdDevice = REVERSER16(0), // No device revision number
    .iManufacturer = 1,         // Manufacturer string index
    .iProduct = 2,              // Product string index
    .iSerialNumber = 0,         // No serial number
    .bNumConfigurations = 1     // One configuration
};

static const struct usb_interface_descriptor interface_descriptor = {
    .bLength = sizeof(struct usb_interface_descriptor),
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,         // Interface has 2 endpoints
    .bInterfaceClass = 0xff,    // Vendor specific endpoint
    .bInterfaceSubClass = 0,
    .bInterfaceProtocol = 0,
    .iInterface = 0
};

static const struct usb_endpoint_descriptor ep1_out = {
    .bLength = sizeof(struct usb_endpoint_descriptor),
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = EP1_OUT_ADDR,   // EP number 1, OUT from host (rx to device)
    .bmAttributes = USB_TRANSFER_TYPE_BULK,
    .wMaxPacketSize = REVERSER16(64),
    .bInterval = 0
};

static const struct usb_endpoint_descriptor ep2_in = {
    .bLength = sizeof(struct usb_endpoint_descriptor),
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = EP2_IN_ADDR,    // EP number 2, IN from host (tx from device)
    .bmAttributes = USB_TRANSFER_TYPE_BULK,
    .wMaxPacketSize = REVERSER16(64),
    .bInterval = 0
};

static const struct usb_configuration_descriptor config_descriptor = {
    .bLength = sizeof(struct usb_configuration_descriptor),
    .bDescriptorType = USB_DT_CONFIG,
    .wTotalLength = REVERSER16(sizeof(config_descriptor) + sizeof(interface_descriptor) + sizeof(ep1_out) + sizeof(ep2_in)),
    .bNumInterfaces = 1,
    .bConfigurationValue = 1,   // Configuration 1
    .iConfiguration = 0,        // No string
    .bmAttributes = 0xc0,       // attributes: self powered, no remote wakeup
    .bMaxPower = 0x32           // 100ma
};

static const unsigned char lang_descriptor[] = {
    4,                          // bLength
    0x03,                       // bDescriptorType == String Descriptor
    0x09, 0x04                  // language id = us english
};

static const unsigned char *descriptor_strings[] = {
    (unsigned char *)"pdaXrom.org",     // Vendor
    (unsigned char *)"N64cart"  // Product
};

// Microsoft OS Descriptor
#define MS_OS_DESC_STRING_INDEX		0xEE
#define MS_OS_DESC_STRING_LENGTH	0x12
#define MS_OS_DESC_VENDOR_CODE_OFFSET	0x10
static const uint8_t ms_os_desc_string[] = {
    MS_OS_DESC_STRING_LENGTH,
    USB_DT_STRING,
    'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0,
    0x69, 0x00
};

static const uint8_t ms_winusb_feature_descriptor[] = {
    0x28, 0x00, 0x00, 0x00, // DWORD (LE)       Descriptor length (40 bytes)
    0x00, 0x01,             // BCD WORD (LE)	Version ('1.0')
    0x04, 0x00,             // WORD (LE)        Compatibility ID Descriptor index (0x0004)
    0x01,                   // BYTE             Number of sections (1)
    0x00, 0x00, 0x00, 0x00, // 7 BYTES          Reserved
    0x00, 0x00, 0x00,       //
    0x00,                   // BYTE             Interface Number (Interface #0)
    0x01,                   // BYTE             Reserved
    'W', 'I', 'N', 'U',     // 8 BYTES          Compatible ID
    'S', 'B', 0x00, 0x00,   // ASCII String     ("WINUSB\0\0")
    0x00, 0x00, 0x00, 0x00, // 8 BYTES          Sub-Compatible ID
    0x00, 0x00, 0x00, 0x00, // ASCII String     (unused)
    0x00, 0x00, 0x00, 0x00, // 6 BYTES          Reserved
    0x00, 0x00              //
};
#endif
