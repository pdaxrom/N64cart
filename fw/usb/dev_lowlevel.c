/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "dev_lowlevel.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../../utils/utils2.h"
#include "../main.h"
#include "../n64.h"
#include "../romfs/romfs.h"
#include "flashrom.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/regs/usb.h"
#include "hardware/resets.h"
#include "hardware/structs/usb.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "usb_common.h"

//#define USB_DEBUG

#define usb_hw_set hw_set_alias(usb_hw)
#define usb_hw_clear hw_clear_alias(usb_hw)

// Function prototypes for our device specific endpoint handlers defined
// later on
void ep0_in_handler(uint8_t * buf, uint16_t len);
void ep0_out_handler(uint8_t * buf, uint16_t len);
void ep1_out_handler(uint8_t * buf, uint16_t len);
void ep2_in_handler(uint8_t * buf, uint16_t len);

// Global device address
static bool should_set_address = false;
static uint8_t dev_addr = 0;
static volatile bool configured = false;

// Global data buffer for EP0
static uint8_t ep0_buf[64];

// Struct defining the device configuration
static struct usb_device_configuration dev_config = {
    .device_descriptor = &device_descriptor,
    .interface_descriptor = &interface_descriptor,
    .config_descriptor = &config_descriptor,
    .lang_descriptor = lang_descriptor,
    .descriptor_strings = descriptor_strings,
    .ms_os_desc_string = ms_os_desc_string,
    .ms_winusb_feature_descriptor = ms_winusb_feature_descriptor,
    .endpoints = {
        {
            .descriptor = &ep0_out,
            .handler = &ep0_out_handler,
            .endpoint_control = NULL,   // NA for EP0
            .buffer_control = &usb_dpram->ep_buf_ctrl[0].out,
            // EP0 in and out share a data buffer
            .data_buffer = &usb_dpram->ep0_buf_a[0],
        },
        {
            .descriptor = &ep0_in,
            .handler = &ep0_in_handler,
            .endpoint_control = NULL,    // NA for EP0,
            .buffer_control = &usb_dpram->ep_buf_ctrl[0].in,
            // EP0 in and out share a data buffer
            .data_buffer = &usb_dpram->ep0_buf_a[0],
        },
        {
            .descriptor = &ep1_out,
            .handler = &ep1_out_handler,
            // EP1 starts at offset 0 for endpoint control
            .endpoint_control = &usb_dpram->ep_ctrl[0].out,
            .buffer_control = &usb_dpram->ep_buf_ctrl[1].out,
            // First free EPX buffer
            .data_buffer = &usb_dpram->epx_data[0 * 64],
        },
        {
            .descriptor = &ep2_in,
            .handler = &ep2_in_handler,
            .endpoint_control = &usb_dpram->ep_ctrl[1].in,
            .buffer_control = &usb_dpram->ep_buf_ctrl[2].in,
            // Second free EPX buffer
            .data_buffer = &usb_dpram->epx_data[1 * 64],
        }
    }
};

/**
 * @brief Given an endpoint address, return the usb_endpoint_configuration of that endpoint. Returns NULL
 * if an endpoint of that address is not found.
 *
 * @param addr
 * @return struct usb_endpoint_configuration*
 */
struct usb_endpoint_configuration *usb_get_endpoint_configuration(uint8_t addr)
{
    struct usb_endpoint_configuration *endpoints = dev_config.endpoints;
    for (int i = 0; i < USB_NUM_ENDPOINTS; i++) {
        if (endpoints[i].descriptor && (endpoints[i].descriptor->bEndpointAddress == addr)) {
            return &endpoints[i];
        }
    }
    return NULL;
}

/**
 * @brief Given a C string, fill the EP0 data buf with a USB string descriptor for that string.
 *
 * @param C string you would like to send to the USB host
 * @return the length of the string descriptor in EP0 buf
 */
uint8_t usb_prepare_string_descriptor(const unsigned char *str)
{
    // 2 for bLength + bDescriptorType + strlen * 2 because string is unicode. i.e. other byte will be 0
    uint8_t bLength = 2 + (strlen((const char *)str) * 2);
    static const uint8_t bDescriptorType = 0x03;

    volatile uint8_t *buf = &ep0_buf[0];
    *buf++ = bLength;
    *buf++ = bDescriptorType;

    uint8_t c;

    do {
        c = *str++;
        *buf++ = c;
        *buf++ = 0;
    } while (c != '\0');

    return bLength;
}

/**
 * @brief Take a buffer pointer located in the USB RAM and return as an offset of the RAM.
 *
 * @param buf
 * @return uint32_t
 */
static inline uint32_t usb_buffer_offset(volatile uint8_t *buf)
{
    return (uint32_t) buf ^ (uint32_t) usb_dpram;
}

/**
 * @brief Set up the endpoint control register for an endpoint (if applicable. Not valid for EP0).
 *
 * @param ep
 */
void usb_setup_endpoint(const struct usb_endpoint_configuration *ep)
{
#ifdef USB_DEBUG
    printf("Set up endpoint 0x%x with buffer address 0x%p\n", ep->descriptor->bEndpointAddress, ep->data_buffer);
#endif

    // EP0 doesn't have one so return if that is the case
    if (!ep->endpoint_control) {
        return;
    }
    // Get the data buffer as an offset of the USB controller's DPRAM
    uint32_t dpram_offset = usb_buffer_offset(ep->data_buffer);
    uint32_t reg = EP_CTRL_ENABLE_BITS | EP_CTRL_INTERRUPT_PER_BUFFER | (ep->descriptor->bmAttributes << EP_CTRL_BUFFER_TYPE_LSB) | dpram_offset;
    *ep->endpoint_control = reg;
}

/**
 * @brief Set up the endpoint control register for each endpoint.
 *
 */
void usb_setup_endpoints()
{
    const struct usb_endpoint_configuration *endpoints = dev_config.endpoints;
    for (int i = 0; i < USB_NUM_ENDPOINTS; i++) {
        if (endpoints[i].descriptor && endpoints[i].handler) {
            usb_setup_endpoint(&endpoints[i]);
        }
    }
}

/**
 * @brief Set up the USB controller in device mode, clearing any previous state.
 *
 */
void usb_device_init()
{
    // Disable USB interrupt at processor
    irq_set_enabled(USBCTRL_IRQ, false);

    // Reset usb controller
    reset_block(RESETS_RESET_USBCTRL_BITS);
    unreset_block_wait(RESETS_RESET_USBCTRL_BITS);

    // Clear any previous state in dpram just in case
    memset(usb_dpram, 0, sizeof(*usb_dpram));   // <1>

    // Enable USB interrupt at processor
    irq_set_enabled(USBCTRL_IRQ, true);

    // Mux the controller to the onboard usb phy
    usb_hw->muxing = USB_USB_MUXING_TO_PHY_BITS | USB_USB_MUXING_SOFTCON_BITS;

    // Force VBUS detect so the device thinks it is plugged into a host
    usb_hw->pwr = USB_USB_PWR_VBUS_DETECT_BITS | USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS;

    // Enable the USB controller in device mode.
    usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS;

    // Enable an interrupt per EP0 transaction
    usb_hw->sie_ctrl = USB_SIE_CTRL_EP0_INT_1BUF_BITS;  // <2>

    // Enable interrupts for when a buffer is done, when the bus is reset,
    // and when a setup packet is received
    usb_hw->inte = USB_INTS_BUFF_STATUS_BITS | USB_INTS_BUS_RESET_BITS | USB_INTS_SETUP_REQ_BITS;

    // Set up endpoints (endpoint control registers)
    // described by device configuration
    usb_setup_endpoints();

    // Present full speed device by enabling pull up on DP
    usb_hw_set->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;
}

/**
 * @brief Disable the USB controller.
 *
 */
void usb_device_finish(void)
{
    // Disable USB interrupt at processor
    irq_set_enabled(USBCTRL_IRQ, false);

    // Reset usb controller
    reset_block(RESETS_RESET_USBCTRL_BITS);
    unreset_block_wait(RESETS_RESET_USBCTRL_BITS);
}

/**
 * @brief Given an endpoint configuration, returns true if the endpoint
 * is transmitting data to the host (i.e. is an IN endpoint)
 *
 * @param ep, the endpoint configuration
 * @return true
 * @return false
 */
static inline bool ep_is_tx(struct usb_endpoint_configuration *ep)
{
    return ep->descriptor->bEndpointAddress & USB_DIR_IN;
}

/**
 * @brief Starts a transfer on a given endpoint.
 *
 * @param ep, the endpoint configuration.
 * @param buf, the data buffer to send. Only applicable if the endpoint is TX
 * @param len, the length of the data in buf (this example limits max len to one packet - 64 bytes)
 */
void usb_start_transfer(struct usb_endpoint_configuration *ep, uint8_t *buf, uint16_t len)
{
    // We are asserting that the length is <= 64 bytes for simplicity of the example.
    // For multi packet transfers see the tinyusb port.
    assert(len <= 64);

#ifdef USB_DEBUG
    printf("Start transfer of len %d on ep addr 0x%x\n", len, ep->descriptor->bEndpointAddress);
#endif

    // Prepare buffer control register value
    uint32_t val = len | USB_BUF_CTRL_AVAIL;

    if (ep_is_tx(ep)) {
        // Need to copy the data from the user buffer to the usb memory
        memcpy((void *)ep->data_buffer, (void *)buf, len);
        // Mark as full
        val |= USB_BUF_CTRL_FULL;
    }
    // Set pid and flip for next transfer
    val |= ep->next_pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID;
    ep->next_pid ^= 1u;

    *ep->buffer_control = val;
}

/**
 * @brief Send device descriptor to host
 *
 */
void usb_handle_device_descriptor(volatile struct usb_setup_packet *pkt)
{
    const struct usb_device_descriptor *d = dev_config.device_descriptor;
    // EP0 in
    struct usb_endpoint_configuration *ep = usb_get_endpoint_configuration(EP0_IN_ADDR);
    // Always respond with pid 1
    ep->next_pid = 1;
    usb_start_transfer(ep, (uint8_t *) d, MIN(sizeof(struct usb_device_descriptor), pkt->wLength));
}

/**
 * @brief Send the configuration descriptor (and potentially the configuration and endpoint descriptors) to the host.
 *
 * @param pkt, the setup packet received from the host.
 */
void usb_handle_config_descriptor(volatile struct usb_setup_packet *pkt)
{
#ifdef USB_DEBUG
    printf("config %02X %02X wIndex=%04X wValue=%04X wLength=%04X\n", pkt->bRequest, pkt->bmRequestType, pkt->wIndex, pkt->wValue, pkt->wLength);
#endif

    uint8_t *buf = &ep0_buf[0];

    // First request will want just the config descriptor
    const struct usb_configuration_descriptor *d = dev_config.config_descriptor;
    memcpy((void *)buf, d, sizeof(struct usb_configuration_descriptor));
    buf += sizeof(struct usb_configuration_descriptor);

    // If we more than just the config descriptor copy it all
    if (pkt->wLength >= d->wTotalLength) {
        memcpy((void *)buf, dev_config.interface_descriptor, sizeof(struct usb_interface_descriptor));
        buf += sizeof(struct usb_interface_descriptor);
        const struct usb_endpoint_configuration *ep = dev_config.endpoints;

        // Copy all the endpoint descriptors starting from EP1
        for (uint i = 2; i < USB_NUM_ENDPOINTS; i++) {
            if (ep[i].descriptor) {
                memcpy((void *)buf, ep[i].descriptor, sizeof(struct usb_endpoint_descriptor));
                buf += sizeof(struct usb_endpoint_descriptor);
            }
        }
    }
    // Send data
    // Get len by working out end of buffer subtract start of buffer
    uint32_t len = (uint32_t) buf - (uint32_t) & ep0_buf[0];
    usb_start_transfer(usb_get_endpoint_configuration(EP0_IN_ADDR), &ep0_buf[0], MIN(len, pkt->wLength));
}

/**
 * @brief Handle a BUS RESET from the host by setting the device address back to 0.
 *
 */
void usb_bus_reset(void)
{
    // Set address back to 0
    dev_addr = 0;
    should_set_address = false;
    usb_hw->dev_addr_ctrl = 0;
    configured = false;
}

/**
 * @brief Send the requested string descriptor to the host.
 *
 * @param pkt, the setup packet from the host.
 */
void usb_handle_string_descriptor(volatile struct usb_setup_packet *pkt)
{
    uint8_t i = pkt->wValue & 0xff;
    uint8_t len = 0;

#ifdef USB_DEBUG
    printf("string %02X %02X wIndex=%04X wValue=%04X wLength=%04X\n", pkt->bRequest, pkt->bmRequestType, pkt->wIndex, pkt->wValue, pkt->wLength);
#endif

    if (i == 0) {
        len = 4;
        memcpy(&ep0_buf[0], dev_config.lang_descriptor, len);
    } else if (i == MS_OS_DESC_STRING_INDEX) {
        len = dev_config.ms_os_desc_string[0];
        memcpy(&ep0_buf[0], dev_config.ms_os_desc_string, len);
    } else {
        // Prepare fills in ep0_buf
        len = usb_prepare_string_descriptor(dev_config.descriptor_strings[i - 1]);
    }

    usb_start_transfer(usb_get_endpoint_configuration(EP0_IN_ADDR), &ep0_buf[0], MIN(len, pkt->wLength));
}

/**
 * @brief
 *
 * @param
 */
void usb_handle_ms_compat_id_features_descriptor(volatile struct usb_setup_packet *pkt)
{
    uint8_t len = 0;
    if (pkt->bRequest == dev_config.ms_os_desc_string[MS_OS_DESC_VENDOR_CODE_OFFSET] && pkt->wIndex == 0x04) {
        len = dev_config.ms_winusb_feature_descriptor[0];
#ifdef USB_DEBUG
        printf("ms compat id featires descriptor %d!!!\n", len);
#endif
        memcpy(&ep0_buf[0], dev_config.ms_winusb_feature_descriptor, len);
    }
    usb_start_transfer(usb_get_endpoint_configuration(EP0_IN_ADDR), &ep0_buf[0], MIN(len, pkt->wLength));
}

/**
 * @brief Sends a zero length status packet back to the host.
 */
void usb_acknowledge_out_request(void)
{
    usb_start_transfer(usb_get_endpoint_configuration(EP0_IN_ADDR), NULL, 0);
}

/**
 * @brief Handles a SET_ADDR request from the host. The actual setting of the device address in
 * hardware is done in ep0_in_handler. This is because we have to acknowledge the request first
 * as a device with address zero.
 *
 * @param pkt, the setup packet from the host.
 */
void usb_set_device_address(volatile struct usb_setup_packet *pkt)
{
    // Set address is a bit of a strange case because we have to send a 0 length status packet first with
    // address 0
    dev_addr = (pkt->wValue & 0xff);
#ifdef USB_DEBUG
    printf("Set address %d\r\n", dev_addr);
#endif
    // Will set address in the callback phase
    should_set_address = true;
    usb_acknowledge_out_request();
}

/**
 * @brief Handles a SET_CONFIGRUATION request from the host. Assumes one configuration so simply
 * sends a zero length status packet back to the host.
 *
 * @param pkt, the setup packet from the host.
 */
void usb_set_device_configuration(volatile struct usb_setup_packet *pkt)
{
    // Only one configuration so just acknowledge the request
#ifdef USB_DEBUG
    printf("Device Enumerated\r\n");
#endif
    usb_acknowledge_out_request();
    configured = true;

// Get ready to rx from host
#ifdef DEBUG_INFO
    printf("USB Device configured\n");
#endif
    usb_start_transfer(usb_get_endpoint_configuration(EP1_OUT_ADDR), NULL, 64);
}

/**
 * @brief Respond to a setup packet from the host.
 *
 */
void usb_handle_setup_packet(void)
{
    volatile struct usb_setup_packet *pkt = (volatile struct usb_setup_packet *)&usb_dpram->setup_packet;
    uint8_t req_direction = pkt->bmRequestType;
    uint8_t req = pkt->bRequest;

#ifdef USB_DEBUG
    uint8_t *tp = (uint8_t *) pkt;
    printf("pkt: %02X %02X %02X %02X %02X %02X %02X %02X\n", tp[0], tp[1], tp[2], tp[3], tp[4], tp[5], tp[6], tp[7]);
    printf("req_direction %d, req %d\n", req_direction, req);
#endif

    // Reset PID to 1 for EP0 IN
    usb_get_endpoint_configuration(EP0_IN_ADDR)->next_pid = 1u;

    if (req_direction == USB_DIR_OUT) {
        if (req == USB_REQUEST_SET_ADDRESS) {
            usb_set_device_address(pkt);
        } else if (req == USB_REQUEST_SET_CONFIGURATION) {
            usb_set_device_configuration(pkt);
        } else {
            usb_acknowledge_out_request();
#ifdef USB_DEBUG
            printf("Other OUT request (0x%x)\r\n", pkt->bRequest);
#endif
        }
    } else if (req_direction == USB_DIR_IN) {
        if (req == USB_REQUEST_GET_DESCRIPTOR) {
            uint16_t descriptor_type = pkt->wValue >> 8;

            switch (descriptor_type) {
            case USB_DT_DEVICE:
                usb_handle_device_descriptor(pkt);
#ifdef USB_DEBUG
                printf("GET DEVICE DESCRIPTOR (len = %d)\r\n", pkt->wLength);
#endif
                break;

            case USB_DT_CONFIG:
                usb_handle_config_descriptor(pkt);
#ifdef USB_DEBUG
                printf("GET CONFIG DESCRIPTOR (len = %d)\r\n", pkt->wLength);
#endif
                break;

            case USB_DT_STRING:
                usb_handle_string_descriptor(pkt);
#ifdef USB_DEBUG
                printf("GET STRING DESCRIPTOR (len = %d)\r\n", pkt->wLength);
#endif
                break;

#ifdef USB_DEBUG
            default:
                printf("Unhandled GET_DESCRIPTOR type 0x%x\r\n", descriptor_type);
#endif
            }
        } else {
#ifdef USB_DEBUG
            printf("Other IN request (0x%x)\r\n", pkt->bRequest);
#endif
        }
    } else if (req_direction == 0xc0) {
        usb_handle_ms_compat_id_features_descriptor(pkt);
    }
}

/**
 * @brief Notify an endpoint that a transfer has completed.
 *
 * @param ep, the endpoint to notify.
 */
static void usb_handle_ep_buff_done(struct usb_endpoint_configuration *ep)
{
    uint32_t buffer_control = *ep->buffer_control;
    // Get the transfer length for this endpoint
    uint16_t len = buffer_control & USB_BUF_CTRL_LEN_MASK;

    // Call that endpoints buffer done handler
    ep->handler((uint8_t *) ep->data_buffer, len);
}

/**
 * @brief Find the endpoint configuration for a specified endpoint number and
 * direction and notify it that a transfer has completed.
 *
 * @param ep_num
 * @param in
 */
static void usb_handle_buff_done(uint ep_num, bool in)
{
    uint8_t ep_addr = ep_num | (in ? USB_DIR_IN : 0);
#ifdef USB_DEBUG
    printf("EP %d (in = %d) done\n", ep_num, in);
#endif
    for (uint i = 0; i < USB_NUM_ENDPOINTS; i++) {
        struct usb_endpoint_configuration *ep = &dev_config.endpoints[i];
        if (ep->descriptor && ep->handler) {
            if (ep->descriptor->bEndpointAddress == ep_addr) {
                usb_handle_ep_buff_done(ep);
                return;
            }
        }
    }
}

/**
 * @brief Handle a "buffer status" irq. This means that one or more
 * buffers have been sent / received. Notify each endpoint where this
 * is the case.
 */
static void usb_handle_buff_status()
{
    uint32_t buffers = usb_hw->buf_status;
    uint32_t remaining_buffers = buffers;

    uint bit = 1u;
    for (uint i = 0; remaining_buffers && i < USB_NUM_ENDPOINTS * 2; i++) {
        if (remaining_buffers & bit) {
            // clear this in advance
            usb_hw_clear->buf_status = bit;
            // IN transfer for even i, OUT transfer for odd i
            usb_handle_buff_done(i >> 1u, !(i & 1u));
            remaining_buffers &= ~bit;
        }
        bit <<= 1u;
    }
}

/**
 * @brief USB interrupt handler
 *
 */
/// \tag::isr_setup_packet[]
void isr_usbctrl(void)
{
#ifdef PI_USBCTRL
    if (gpio_get(N64_COLD_RESET)) {
        *((io_rw_32 *) (PPB_BASE + M0PLUS_NVIC_ICER_OFFSET)) = 1 << USBCTRL_IRQ;

        usb64_ctrl_reg |= 0x8000;
        if (usb64_ctrl_reg & 0x0010) {
            gpio_put(N64_INT, 0);
        }

        return;
    }
#endif

    // USB interrupt handler
    uint32_t status = usb_hw->ints;
    uint32_t handled = 0;

#ifdef USB_DEBUG
    printf("usb irq status %08lx\n", status);
#endif

    // Setup packet received
    if (status & USB_INTS_SETUP_REQ_BITS) {
        handled |= USB_INTS_SETUP_REQ_BITS;
        usb_hw_clear->sie_status = USB_SIE_STATUS_SETUP_REC_BITS;
        usb_handle_setup_packet();
    }
    /// \end::isr_setup_packet[]

    // Buffer status, one or more buffers have completed
    if (status & USB_INTS_BUFF_STATUS_BITS) {
        handled |= USB_INTS_BUFF_STATUS_BITS;
        usb_handle_buff_status();
    }
    // Bus is reset
    if (status & USB_INTS_BUS_RESET_BITS) {
#ifdef USB_DEBUG
        printf("BUS RESET\n");
#endif
        handled |= USB_INTS_BUS_RESET_BITS;
        usb_hw_clear->sie_status = USB_SIE_STATUS_BUS_RESET_BITS;
        usb_bus_reset();
    }

    if (status ^ handled) {
        panic("Unhandled IRQ 0x%x\n", (uint) (status ^ handled));
    }
}

/**
 * @brief EP0 in transfer complete. Either finish the SET_ADDRESS process, or receive a zero
 * length status packet from the host.
 *
 * @param buf the data that was sent
 * @param len the length that was sent
 */
void ep0_in_handler(uint8_t *buf, uint16_t len)
{
    if (should_set_address) {
        // Set actual device address in hardware
        usb_hw->dev_addr_ctrl = dev_addr;
        should_set_address = false;
    } else {
        // Receive a zero length status packet from the host on EP0 OUT
        struct usb_endpoint_configuration *ep = usb_get_endpoint_configuration(EP0_OUT_ADDR);
        usb_start_transfer(ep, NULL, 0);
    }
}

void ep0_out_handler(uint8_t *buf, uint16_t len)
{;
}

static uint8_t *sector_buffer = pi_sram;
static int sector_buffer_pos;
static uint32_t rw_sector_offset;

static struct ack_header ackn;

static int current_req;
static int flash_stage;

// Device specific functions
void ep1_out_handler(uint8_t *buf, uint16_t len)
{
    struct usb_endpoint_configuration *ep_out = usb_get_endpoint_configuration(EP2_IN_ADDR);
    //    printf("RX %d bytes from host\n", len);
    ackn.type = ACK_ERROR;

    struct req_header *req = (struct req_header *)buf;
    if (current_req != CART_WRITE_SEC) {
        if (len != sizeof(struct req_header)) {
            printf("Wrong header size %d, must be %d\n", len, sizeof(struct req_header));
            usb_start_transfer(usb_get_endpoint_configuration(EP1_OUT_ADDR), NULL, 64);
            return;
        }
    }
    // printf("flash_stage = %d, req->type = %04X\n", flash_stage, req->type);

    if (flash_stage == 0) {
        current_req = req->type;
        if (req->type == CART_INFO) {
            uintptr_t fw_binary_end = (uintptr_t) & __flash_binary_end;
            const struct flash_chip *flash_chip = get_flash_info();
            ackn.type = ACK_NOERROR;
            ackn.info.start = fw_binary_end - XIP_BASE;
            ackn.info.size = flash_chip->rom_size * 1024 * 1024;
            ackn.info.vers = FIRMWARE_VERSION;
            usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));
            return;
        } else if (req->type == FLASH_SPI_MODE || req->type == FLASH_QUAD_MODE || req->type == BOOTLOADER_MODE || req->type == CART_REBOOT) {
            if (req->type == FLASH_SPI_MODE) {
                flash_quad_exit_cont_read_mode();
                flash_spi_mode();
            } else if (req->type == FLASH_QUAD_MODE) {
                flash_quad_cont_read_mode();
            }
            ackn.type = ACK_NOERROR;
            usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));

            if (req->type == BOOTLOADER_MODE) {
                printf("reset to bootloader ...\n");
#ifdef PICO_DEFAULT_LED_PIN
                reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 0);
#else
                reset_usb_boot(0, 0);
#endif
            } else if (req->type == CART_REBOOT) {
                printf("reboot ...\n");
                watchdog_reboot(0, 0, 100);
            }
            return;
        } else if (req->type == CART_READ_SEC || req->type == CART_READ_SEC_CONT) {
            uint8_t tmp[64];
            if (req->type == CART_READ_SEC) {
                rw_sector_offset = req->offset;
                req->offset = 0;
            }
            flash_read(rw_sector_offset + req->offset, tmp, 64);
            usb_start_transfer(ep_out, tmp, sizeof(tmp));
            return;
        } else if (req->type == CART_WRITE_SEC) {
            flash_stage = 1;
            sector_buffer_pos = 0;
            rw_sector_offset = req->offset;
            ackn.type = ACK_NOERROR;
            usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));
            return;
        } else if (req->type == CART_ERASE_SEC) {
            romfs_flash_sector_erase(req->offset);
            ackn.type = ACK_NOERROR;
            usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));
            return;
        }
        current_req = 0;
    } else if (flash_stage == 1) {
        if (current_req == CART_WRITE_SEC) {
            if (len != 64) {
                printf("write transfer size error\n");
            } else {
                memmove(&sector_buffer[sector_buffer_pos], buf, 64);
                sector_buffer_pos += 64;
                if (sector_buffer_pos == ROMFS_FLASH_SECTOR) {
                    romfs_flash_sector_write(rw_sector_offset, sector_buffer);
                    flash_stage = 0;
                    current_req = 0;
                }
                ackn.type = ACK_NOERROR;
                usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));
                return;
            }
        }
        flash_stage = 0;
        current_req = 0;
    }

    usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));
}

void ep2_in_handler(uint8_t *buf, uint16_t len)
{
    //    printf("Sent %d bytes to host\n", len);
    // Get ready to rx again from host
    usb_start_transfer(usb_get_endpoint_configuration(EP1_OUT_ADDR), NULL, 64);
}

void usbd_start(void)
{
#ifdef DEBUG_INFO
    printf("USB Device Low-Level hardware example\n");
#endif
    usb_device_init();

    current_req = 0;
    flash_stage = 0;
}

void usbd_finish(void)
{
    usb_device_finish();
}
