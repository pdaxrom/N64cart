/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "dev_lowlevel.h"

#include <libdragon.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../../fw/romfs/romfs.h"
#include "../../utils/utils2.h"
#include "../main.h"
#include "../n64cart.h"
#include "../syslog.h"
#include "usb_common.h"

// #define USB_DEBUG 1

#define usb_hw_set hw_set_alias(usb_hw)
#define usb_hw_clear hw_clear_alias(usb_hw)

// Function prototypes for our device specific endpoint handlers defined
// later on
static void ep0_in_handler(uint8_t * buf, uint16_t len);
static void ep0_out_handler(uint8_t * buf, uint16_t len);
static void ep1_out_handler(uint8_t * buf, uint16_t len);
static void ep2_in_handler(uint8_t * buf, uint16_t len);

//
static void isr_usbctrl(void);

//
static bool usbd_enabled = false;

// Global device address
static bool should_set_address = false;
static uint8_t dev_addr = 0;
static volatile bool configured = false;

// Global data buffer for EP0
static uint8_t ep0_buf[64];

// Struct defining the device configuration
static struct usb_device_configuration dev_config = {.device_descriptor = &device_descriptor,
    .interface_descriptor = &interface_descriptor,
    .config_descriptor = &config_descriptor,
    .lang_descriptor = lang_descriptor,
    .descriptor_strings = descriptor_strings,
    .endpoints = { {
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
                   } }
};

/**
 *
 */
static inline uint32_t reverser32(uint32_t x)
{
    return REVERSER32(x);
}

static inline uint16_t reverser16(uint16_t x)
{
    return REVERSER16(x);
}

static inline void pi_io_write_buf(uint32_t pi_address, void *buf, size_t len)
{
    volatile uint32_t *uncached_address = (uint32_t *) (pi_address | 0xa0000000);

    for (size_t i = 0; i < len; i++) {
        dma_wait();
        MEMORY_BARRIER();
        uncached_address[i] = ((uint32_t *) buf)[i];
        MEMORY_BARRIER();
    }
}

static inline void pi_io_read_buf(uint32_t pi_address, void *buf, size_t len)
{
    volatile uint32_t *uncached_address = (uint32_t *) (pi_address | 0xa0000000);

    for (size_t i = 0; i < len; i++) {
        dma_wait();
        MEMORY_BARRIER();
        ((uint32_t *) buf)[i] = uncached_address[i];
    }
}

/**
 * @brief Given an endpoint address, return the usb_endpoint_configuration of that endpoint. Returns NULL
 * if an endpoint of that address is not found.
 *
 * @param addr
 * @return struct usb_endpoint_configuration*
 */
static struct usb_endpoint_configuration *usb_get_endpoint_configuration(uint8_t addr)
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
static uint8_t usb_prepare_string_descriptor(const unsigned char *str)
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
static void usb_setup_endpoint(const struct usb_endpoint_configuration *ep)
{
#ifdef USB_DEBUG
    syslog(LOG_DEBUG, "Set up endpoint 0x%x with buffer address 0x%p", ep->descriptor->bEndpointAddress, ep->data_buffer);
#endif

    // EP0 doesn't have one so return if that is the case
    if (!ep->endpoint_control) {
        return;
    }
    // Get the data buffer as an offset of the USB controller's DPRAM
    uint32_t dpram_offset = usb_buffer_offset(ep->data_buffer);
    uint32_t reg = EP_CTRL_ENABLE_BITS | EP_CTRL_INTERRUPT_PER_BUFFER | (ep->descriptor->bmAttributes << EP_CTRL_BUFFER_TYPE_LSB) | dpram_offset;
    pi_io_write((uintptr_t) ep->endpoint_control, reg);
}

/**
 * @brief Set up the endpoint control register for each endpoint.
 *
 */
static void usb_setup_endpoints()
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
static void usb_device_init()
{
    // Reset usb controller
    io_write(N64CART_USBCFG, 0);
    io_write(N64CART_USBCFG, N64CART_USB_RESET);
    io_write(N64CART_USBCFG, 0);

    for (int i = 0; i < sizeof(*usb_dpram); i += 4) {
        io_write(((uintptr_t) usb_dpram) + i, 0);
    }

    // Enable USB interrupt at processor
    disable_interrupts();
    set_CART_interrupt(1);
    register_CART_handler(isr_usbctrl);
    enable_interrupts();

    // Mux the controller to the onboard usb phy
    io_write((uintptr_t) & usb_hw->muxing, USB_USB_MUXING_TO_PHY_BITS | USB_USB_MUXING_SOFTCON_BITS);

    // Force VBUS detect so the device thinks it is plugged into a host
    io_write((uintptr_t) & usb_hw->pwr, USB_USB_PWR_VBUS_DETECT_BITS | USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS);

    // Enable the USB controller in device mode.
    io_write((uintptr_t) & usb_hw->main_ctrl, USB_MAIN_CTRL_CONTROLLER_EN_BITS);

    // Enable an interrupt per EP0 transaction
    io_write((uintptr_t) & usb_hw->sie_ctrl, USB_SIE_CTRL_EP0_INT_1BUF_BITS);   // <2>

    // Enable interrupts for when a buffer is done, when the bus is reset,
    // and when a setup packet is received
    io_write((uintptr_t) & usb_hw->inte, USB_INTS_BUFF_STATUS_BITS | USB_INTS_BUS_RESET_BITS | USB_INTS_SETUP_REQ_BITS);

    // Set up endpoints (endpoint control registers)
    // described by device configuration
    disable_interrupts();
    usb_setup_endpoints();
    enable_interrupts();

    // Present full speed device by enabling pull up on DP
    io_write((uintptr_t) & usb_hw_set->sie_ctrl, USB_SIE_CTRL_PULLUP_EN_BITS);

    io_write(N64CART_USBCFG, N64CART_USB_IRQ_ENABLE);
}

/**
 * @brief Disable the USB controller.
 *
 */
static void usb_device_finish(void)
{
    // Enable USB interrupt at processor
    disable_interrupts();

    pi_io_write(N64CART_USBCFG, 0);
    pi_io_write(N64CART_USBCFG, N64CART_USB_RESET);
    pi_io_write(N64CART_USBCFG, 0);

    set_CART_interrupt(0);
    unregister_CART_handler(isr_usbctrl);

    enable_interrupts();
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
static void usb_start_transfer(struct usb_endpoint_configuration *ep, uint8_t *buf, uint16_t len)
{
    // We are asserting that the length is <= 64 bytes for simplicity of the example.
    // For multi packet transfers see the tinyusb port.
    assert(len <= 64);

#ifdef USB_DEBUG
    syslog(LOG_DEBUG, "Start transfer of len %d on ep addr 0x%x", len, ep->descriptor->bEndpointAddress);
#endif

    // Prepare buffer control register value
    uint32_t val = len | USB_BUF_CTRL_AVAIL;

    if (ep_is_tx(ep)) {
        if (len > 0) {
            pi_io_write(N64CART_USBCFG, N64CART_USB_BSWAP32 | N64CART_USB_IRQ_ENABLE);
            pi_io_write_buf((uint32_t) ep->data_buffer, buf, (len + 3) >> 2);
            pi_io_write(N64CART_USBCFG, N64CART_USB_IRQ_ENABLE);
        }
        // Mark as full
        val |= USB_BUF_CTRL_FULL;
    }
    // Set pid and flip for next transfer
    val |= ep->next_pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID;
    ep->next_pid ^= 1u;

    pi_io_write((uintptr_t) ep->buffer_control, val);
}

/**
 * @brief Send device descriptor to host
 *
 */
static void usb_handle_device_descriptor(volatile struct usb_setup_packet *pkt)
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
static void usb_handle_config_descriptor(volatile struct usb_setup_packet *pkt)
{
    uint8_t *buf = &ep0_buf[0];

    // First request will want just the config descriptor
    const struct usb_configuration_descriptor *d = dev_config.config_descriptor;
    memcpy((void *)buf, d, sizeof(struct usb_configuration_descriptor));
    buf += sizeof(struct usb_configuration_descriptor);

    // If we more than just the config descriptor copy it all
    if (pkt->wLength >= reverser16(d->wTotalLength)) {
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
static void usb_bus_reset(void)
{
    // Set address back to 0
    dev_addr = 0;
    should_set_address = false;
    pi_io_write((uintptr_t) & usb_hw->dev_addr_ctrl, 0);
    configured = false;
}

/**
 * @brief Send the requested string descriptor to the host.
 *
 * @param pkt, the setup packet from the host.
 */
static void usb_handle_string_descriptor(volatile struct usb_setup_packet *pkt)
{
    uint8_t i = pkt->wValue & 0xff;
    uint8_t len = 0;

    if (i == 0) {
        len = 4;
        memcpy(&ep0_buf[0], dev_config.lang_descriptor, len);
    } else {
        // Prepare fills in ep0_buf
        len = usb_prepare_string_descriptor(dev_config.descriptor_strings[i - 1]);
    }

    usb_start_transfer(usb_get_endpoint_configuration(EP0_IN_ADDR), &ep0_buf[0], MIN(len, pkt->wLength));
}

/**
 * @brief Sends a zero length status packet back to the host.
 */
static void usb_acknowledge_out_request(void)
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
static void usb_set_device_address(volatile struct usb_setup_packet *pkt)
{
    // Set address is a bit of a strange case because we have to send a 0 length status packet first with
    // address 0
    dev_addr = (pkt->wValue & 0xff);
#ifdef USB_DEBUG
    syslog(LOG_DEBUG, "Set address %d", dev_addr);
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
static void usb_set_device_configuration(volatile struct usb_setup_packet *pkt)
{
    // Only one configuration so just acknowledge the request
#ifdef USB_DEBUG
    syslog(LOG_DEBUG, "Device Enumerated");
#endif
    usb_acknowledge_out_request();
    configured = true;

    // Get ready to rx from host
#ifdef DEBUG_INFO
    syslog(LOG_DEBUG, "USB Device configured");
#endif
    usb_start_transfer(usb_get_endpoint_configuration(EP1_OUT_ADDR), NULL, 64);
}

/**
 * @brief Respond to a setup packet from the host.
 *
 */
static void usb_handle_setup_packet(void)
{
    // volatile struct usb_setup_packet *pkt = (volatile struct usb_setup_packet *)&usb_dpram->setup_packet;
    struct usb_setup_packet pkt_buf;
    struct usb_setup_packet *pkt = &pkt_buf;

    pi_io_write(N64CART_USBCFG, N64CART_USB_BSWAP32 | N64CART_USB_IRQ_ENABLE);
    pi_io_read_buf((uint32_t) usb_dpram->setup_packet, pkt, 2);
    pi_io_write(N64CART_USBCFG, N64CART_USB_IRQ_ENABLE);

    pkt->wIndex = reverser16(pkt->wIndex);
    pkt->wLength = reverser16(pkt->wLength);
    pkt->wValue = reverser16(pkt->wValue);

    uint8_t req_direction = pkt->bmRequestType;
    uint8_t req = pkt->bRequest;

#ifdef USB_DEBUG
    uint8_t *tp = (uint8_t *) pkt;
    syslog(LOG_DEBUG, "pkt: %02X %02X %02X %02X %02X %02X %02X %02X", tp[0], tp[1], tp[2], tp[3], tp[4], tp[5], tp[6], tp[7]);
    syslog(LOG_DEBUG, "%08X %08X", ((uint32_t *) pkt)[0], ((uint32_t *) pkt)[1]);
    syslog(LOG_DEBUG, "req_direction %d, req %d", req_direction, req);
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
            syslog(LOG_DEBUG, "Other OUT request (0x%x)", pkt->bRequest);
#endif
        }
    } else if (req_direction == USB_DIR_IN) {
        if (req == USB_REQUEST_GET_DESCRIPTOR) {
            uint16_t descriptor_type = pkt->wValue >> 8;

            switch (descriptor_type) {
            case USB_DT_DEVICE:
                usb_handle_device_descriptor(pkt);
#ifdef USB_DEBUG
                syslog(LOG_DEBUG, "GET DEVICE DESCRIPTOR (len = %d)", pkt->wLength);
#endif
                break;

            case USB_DT_CONFIG:
                usb_handle_config_descriptor(pkt);
#ifdef USB_DEBUG
                syslog(LOG_DEBUG, "GET CONFIG DESCRIPTOR (len = %d)", pkt->wLength);
#endif
                break;

            case USB_DT_STRING:
                usb_handle_string_descriptor(pkt);
#ifdef USB_DEBUG
                syslog(LOG_DEBUG, "GET STRING DESCRIPTOR (len = %d)", pkt->wLength);
#endif
                break;

#ifdef USB_DEBUG
            default:
                syslog(LOG_DEBUG, "Unhandled GET_DESCRIPTOR type 0x%x", descriptor_type);
#endif
            }
        } else {
#ifdef USB_DEBUG
            syslog(LOG_DEBUG, "Other IN request (0x%x)", pkt->bRequest);
#endif
        }
    }
}

/**
 * @brief Notify an endpoint that a transfer has completed.
 *
 * @param ep, the endpoint to notify.
 */
static void usb_handle_ep_buff_done(struct usb_endpoint_configuration *ep)
{
    uint32_t buffer_control = pi_io_read((uintptr_t) ep->buffer_control);
    // Get the transfer length for this endpoint
    uint16_t len = buffer_control & USB_BUF_CTRL_LEN_MASK;

    uint8_t buf[((len + 3) >> 2) << 2];
    if (len > 0) {
        pi_io_write(N64CART_USBCFG, N64CART_USB_BSWAP32 | N64CART_USB_IRQ_ENABLE);
        pi_io_read_buf((uint32_t) ep->data_buffer, buf, (len + 3) >> 2);
        pi_io_write(N64CART_USBCFG, N64CART_USB_IRQ_ENABLE);
    }
    // Call that endpoints buffer done handler
    ep->handler(buf, len);
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
    syslog(LOG_DEBUG, "EP %d (in = %d) done", ep_num, in);
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
    uint32_t buffers = pi_io_read((uintptr_t) & usb_hw->buf_status);
    uint32_t remaining_buffers = buffers;

    uint bit = 1u;
    for (uint i = 0; remaining_buffers && i < USB_NUM_ENDPOINTS * 2; i++) {
        if (remaining_buffers & bit) {
            // clear this in advance
            pi_io_write((uintptr_t) & usb_hw_clear->buf_status, bit);
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
static void isr_usbctrl(void)
{
    // USB interrupt handler
    uint32_t status = pi_io_read((uintptr_t) & usb_hw->ints);
    uint32_t handled = 0;

#ifdef USB_DEBUG
    syslog(LOG_INFO, "usb irq status %08lx", status);
#endif

    // Setup packet received
    if (status & USB_INTS_SETUP_REQ_BITS) {
        handled |= USB_INTS_SETUP_REQ_BITS;
        pi_io_write((uintptr_t) & usb_hw_clear->sie_status, USB_SIE_STATUS_SETUP_REC_BITS);
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
        syslog(LOG_DEBUG, "BUS RESET");
#endif
        handled |= USB_INTS_BUS_RESET_BITS;
        pi_io_write((uintptr_t) & usb_hw_clear->sie_status, USB_SIE_STATUS_BUS_RESET_BITS);
        usb_bus_reset();
    }

    if (status ^ handled) {
        syslog(LOG_ERR, "Unhandled IRQ 0x%x", (uint) (status ^ handled));
    }

    (void)pi_io_read(N64CART_USBCFG);
}

/**
 * @brief EP0 in transfer complete. Either finish the SET_ADDRESS process, or receive a zero
 * length status packet from the host.
 *
 * @param buf the data that was sent
 * @param len the length that was sent
 */
static void ep0_in_handler(uint8_t *buf, uint16_t len)
{
    if (should_set_address) {
        // Set actual device address in hardware
        pi_io_write((uintptr_t) & usb_hw->dev_addr_ctrl, dev_addr);
        should_set_address = false;
    } else {
        // Receive a zero length status packet from the host on EP0 OUT
        struct usb_endpoint_configuration *ep = usb_get_endpoint_configuration(EP0_OUT_ADDR);
        usb_start_transfer(ep, NULL, 0);
    }
}

static void ep0_out_handler(uint8_t *buf, uint16_t len)
{;
}

static uint8_t sector_buffer[ROMFS_FLASH_SECTOR];
static int sector_buffer_pos;
static uint32_t rw_sector_offset;

static struct ack_header ackn;

static int current_req;
static int flash_stage;

// Device specific functions
static void ep1_out_handler(uint8_t *buf, uint16_t len)
{
    struct usb_endpoint_configuration *ep_out = usb_get_endpoint_configuration(EP2_IN_ADDR);
    // syslog(LOG_DEBUG, "RX %d bytes from host", len);
    ackn.type = ACK_ERROR;

    struct req_header *req = (struct req_header *)buf;
    if (current_req != CART_WRITE_SEC) {
        if (len != sizeof(struct req_header)) {
            syslog(LOG_ERR, "Wrong header size %d, must be %d", len, sizeof(struct req_header));
            usb_start_transfer(usb_get_endpoint_configuration(EP1_OUT_ADDR), NULL, 64);
            return;
        }
    }
    // syslog(LOG_DEBUG, "flash_stage = %d, req->type = %04X", flash_stage, req->type);

    if (flash_stage == 0) {
        current_req = reverser16(req->type);
        if (current_req == CART_INFO) {
            const struct flash_chip *flash_chip = get_flash_info();
            ackn.type = reverser16(ACK_NOERROR);
            ackn.info.start = reverser32(pi_io_read(N64CART_FW_SIZE));
            ackn.info.size = reverser32(flash_chip->rom_size * 1024 * 1024);
            ackn.info.vers = reverser32(FIRMWARE_VERSION);
            usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));
            return;
        } else if (current_req == FLASH_SPI_MODE || current_req == FLASH_QUAD_MODE || current_req == BOOTLOADER_MODE || current_req == CART_REBOOT) {
            if (current_req == FLASH_SPI_MODE) {
                flash_mode(false);
            } else if (current_req == FLASH_QUAD_MODE) {
                flash_mode(true);
            }
            ackn.type = reverser16(ACK_NOERROR);
            usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));

            if (current_req == BOOTLOADER_MODE) {
                syslog(LOG_ERR, "Unsupported command: reset to bootloader ...");
            } else if (current_req == CART_REBOOT) {
                syslog(LOG_ERR, "Unsupported command: reboot ...");
            }
            return;
        } else if (current_req == CART_READ_SEC || current_req == CART_READ_SEC_CONT) {
            uint8_t tmp[64];
            if (current_req == CART_READ_SEC) {
                rw_sector_offset = reverser32(req->offset);
                flash_read(rw_sector_offset, sector_buffer, ROMFS_FLASH_SECTOR);
                req->offset = 0;
            }
            memmove(tmp, &sector_buffer[reverser32(req->offset)], 64);
            // syslog(LOG_DEBUG, "read offset = %08X", req->offset);

            usb_start_transfer(ep_out, tmp, sizeof(tmp));
            return;
        } else if (current_req == CART_WRITE_SEC) {
            flash_stage = 1;
            sector_buffer_pos = 0;
            rw_sector_offset = reverser32(req->offset);
            ackn.type = reverser16(ACK_NOERROR);
            usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));
            return;
        } else if (current_req == CART_ERASE_SEC) {
            romfs_flash_sector_erase(reverser32(req->offset));
            ackn.type = reverser16(ACK_NOERROR);
            usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));
            return;
        }
        current_req = 0;
    } else if (flash_stage == 1) {
        if (current_req == CART_WRITE_SEC) {
            if (len != 64) {
                syslog(LOG_ERR, "write transfer checksum error");
            } else {
                memmove(&sector_buffer[sector_buffer_pos], buf, 64);
                sector_buffer_pos += 64;
                if (sector_buffer_pos == ROMFS_FLASH_SECTOR) {
                    romfs_flash_sector_write(rw_sector_offset, sector_buffer);
                    flash_stage = 0;
                    current_req = 0;
                }
                ackn.type = reverser16(ACK_NOERROR);
                usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));
                return;
            }
        }
        flash_stage = 0;
        current_req = 0;
    }

    usb_start_transfer(ep_out, (uint8_t *) & ackn, sizeof(struct ack_header));
}

static void ep2_in_handler(uint8_t *buf, uint16_t len)
{
    // syslog(LOG_DEBUG, "Sent %d bytes to host", len);
    // Get ready to rx again from host
    usb_start_transfer(usb_get_endpoint_configuration(EP1_OUT_ADDR), NULL, 64);
}

void usbd_start(void)
{
#ifdef DEBUG_INFO
    syslog(LOG_DEBUG, "USB Device Low-Level hardware example");
#endif
    usbd_enabled = true;

    usb_device_init();

    current_req = 0;
    flash_stage = 0;
}

void usbd_finish(void)
{
    usb_device_finish();

    usbd_enabled = false;
}
