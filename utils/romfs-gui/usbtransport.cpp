#include "usbtransport.h"

#include <cstring>

#include <QDebug>

#include "romfs.h"

namespace
{
constexpr int kRetryMax = 50;
constexpr unsigned char kOutEndpoint = 0x01;
constexpr unsigned char kInEndpoint = 0x82;
}

UsbTransport::UsbTransport(QObject *parent)
    : RomfsTransport(parent)
{
}

UsbTransport::~UsbTransport()
{
    disconnectDevice();
}

bool UsbTransport::connectDevice(QString *errorString)
{
    if (ctx_) {
        if (errorString) {
            *errorString = QStringLiteral("USB context already initialised");
        }
        return true;
    }

    int ret = libusb_init(&ctx_);
    if (ret < 0) {
        setLastError(QStringLiteral("libusb_init failed (%1)").arg(ret));
        if (errorString) {
            *errorString = lastError();
        }
        ctx_ = nullptr;
        return false;
    }

#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000106)
    libusb_set_option(ctx_, LIBUSB_OPTION_LOG_LEVEL, 3);
#else
    libusb_set_debug(ctx_, 3);
#endif

    handle_ = libusb_open_device_with_vid_pid(ctx_, 0x1209, 0x6800);
    if (!handle_) {
        setLastError(QStringLiteral("Cannot open N64cart USB device"));
        if (errorString) {
            *errorString = lastError();
        }
        libusb_exit(ctx_);
        ctx_ = nullptr;
        return false;
    }

    if (libusb_kernel_driver_active(handle_, 0) == 1) {
        libusb_detach_kernel_driver(handle_, 0);
    }

    int claim = libusb_claim_interface(handle_, 0);
    if (claim != 0) {
        setLastError(QStringLiteral("Cannot claim USB interface (%1)").arg(claim));
        if (errorString) {
            *errorString = lastError();
        }
        libusb_close(handle_);
        handle_ = nullptr;
        libusb_exit(ctx_);
        ctx_ = nullptr;
        return false;
    }
    interfaceClaimed_ = true;
    return true;
}

void UsbTransport::disconnectDevice()
{
    if (handle_ && interfaceClaimed_) {
        libusb_release_interface(handle_, 0);
        interfaceClaimed_ = false;
    }
    if (handle_) {
        libusb_close(handle_);
        handle_ = nullptr;
    }
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

int UsbTransport::bulkTransfer(unsigned char endpoint, unsigned char *data, int length, int *transferred, unsigned int timeout)
{
    if (!handle_) {
        return LIBUSB_ERROR_NO_DEVICE;
    }

    int ret;
    int attempt = 0;
    do {
        ret = libusb_bulk_transfer(handle_, endpoint, data, length, transferred, timeout);
        if (ret == LIBUSB_ERROR_PIPE) {
            libusb_clear_halt(handle_, endpoint);
        }
        attempt++;
    } while (ret == LIBUSB_ERROR_PIPE && attempt < kRetryMax);
    return ret;
}

bool UsbTransport::ensureConnected(QString *errorString)
{
    if (handle_) {
        return true;
    }
    return connectDevice(errorString);
}

bool UsbTransport::sendCommand(uint16_t type, ack_header *ack, QString *errorString)
{
    if (!ensureConnected(errorString)) {
        return false;
    }

    req_header req = {};
    req.type = type;
    req.offset = 0;

    int actual = 0;
    int ret = bulkTransfer(kOutEndpoint, reinterpret_cast<unsigned char *>(&req), sizeof(req), &actual, 5000);
    if (ret != 0 || actual != static_cast<int>(sizeof(req))) {
        setLastError(QStringLiteral("USB command write failed (%1)").arg(ret));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }

    ack_header localAck;
    ack_header *target = ack ? ack : &localAck;
    ret = bulkTransfer(kInEndpoint, reinterpret_cast<unsigned char *>(target), sizeof(*target), &actual, 5000);
    if (ret != 0 || actual != static_cast<int>(sizeof(*target))) {
        setLastError(QStringLiteral("USB command read failed (%1)").arg(ret));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }

    if (target->type != ACK_NOERROR) {
        setLastError(QStringLiteral("Device reported error (0x%1)").arg(target->type, 0, 16));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }

    return true;
}

bool UsbTransport::eraseSector(uint32_t offset, QString *errorString)
{
    if (!ensureConnected(errorString)) {
        return false;
    }

    req_header req = {};
    req.type = CART_ERASE_SEC;
    req.offset = offset;

    int actual = 0;
    int ret = bulkTransfer(kOutEndpoint, reinterpret_cast<unsigned char *>(&req), sizeof(req), &actual, 5000);
    if (ret != 0 || actual != static_cast<int>(sizeof(req))) {
        setLastError(QStringLiteral("Flash erase request failed (%1)").arg(ret));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }

    ack_header ack;
    ret = bulkTransfer(kInEndpoint, reinterpret_cast<unsigned char *>(&ack), sizeof(ack), &actual, 5000);
    if (ret != 0 || actual != static_cast<int>(sizeof(ack))) {
        setLastError(QStringLiteral("Flash erase reply failed (%1)").arg(ret));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }

    if (ack.type != ACK_NOERROR) {
        setLastError(QStringLiteral("Flash erase returned error"));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }
    return true;
}

bool UsbTransport::writeSector(uint32_t offset, const uint8_t *buffer, QString *errorString)
{
    if (!ensureConnected(errorString)) {
        return false;
    }

    req_header req = {};
    req.type = CART_WRITE_SEC;
    req.offset = offset;

    int actual = 0;
    int ret = bulkTransfer(kOutEndpoint, reinterpret_cast<unsigned char *>(&req), sizeof(req), &actual, 5000);
    if (ret != 0 || actual != static_cast<int>(sizeof(req))) {
        setLastError(QStringLiteral("Flash write header failed (%1)").arg(ret));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }

    ack_header ack;
    ret = bulkTransfer(kInEndpoint, reinterpret_cast<unsigned char *>(&ack), sizeof(ack), &actual, 5000);
    if (ret != 0 || actual != static_cast<int>(sizeof(ack))) {
        setLastError(QStringLiteral("Flash write ack failed (%1)").arg(ret));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }

    if (ack.type != ACK_NOERROR) {
        setLastError(QStringLiteral("Flash write rejected"));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }

    for (int i = 0; i < ROMFS_FLASH_SECTOR; i += 64) {
        uint8_t chunk[64];
        std::memcpy(chunk, buffer + i, sizeof(chunk));
        ret = bulkTransfer(kOutEndpoint, chunk, sizeof(chunk), &actual, 5000);
        if (ret != 0 || actual != static_cast<int>(sizeof(chunk))) {
            setLastError(QStringLiteral("Data chunk write failed (%1)").arg(ret));
            if (errorString) {
                *errorString = lastError();
            }
            return false;
        }

        ret = bulkTransfer(kInEndpoint, reinterpret_cast<unsigned char *>(&ack), sizeof(ack), &actual, 5000);
        if (ret != 0 || actual != static_cast<int>(sizeof(ack))) {
            setLastError(QStringLiteral("Chunk ack failed (%1)").arg(ret));
            if (errorString) {
                *errorString = lastError();
            }
            return false;
        }

        if (ack.type != ACK_NOERROR) {
            setLastError(QStringLiteral("Chunk ack error"));
            if (errorString) {
                *errorString = lastError();
            }
            return false;
        }
    }

    return true;
}

bool UsbTransport::readSector(uint32_t offset, uint8_t *buffer, uint32_t length, QString *errorString)
{
    if (!ensureConnected(errorString)) {
        return false;
    }

    req_header req = {};
    req.type = CART_READ_SEC;
    req.offset = offset;

    int actual = 0;
    int ret = bulkTransfer(kOutEndpoint, reinterpret_cast<unsigned char *>(&req), sizeof(req), &actual, 5000);
    if (ret != 0 || actual != static_cast<int>(sizeof(req))) {
        setLastError(QStringLiteral("Flash read header failed (%1)").arg(ret));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }

    uint32_t remaining = length;
    uint32_t position = 0;

    while (remaining > 0) {
        uint8_t chunk[64];
        ret = bulkTransfer(kInEndpoint, chunk, sizeof(chunk), &actual, 5000);
        if (ret != 0 || actual != static_cast<int>(sizeof(chunk))) {
            setLastError(QStringLiteral("Flash read reply failed (%1)").arg(ret));
            if (errorString) {
                *errorString = lastError();
            }
            return false;
        }

        const uint32_t copySize = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        std::memcpy(buffer + position, chunk, copySize);

        remaining -= copySize;
        position += copySize;

        if (remaining == 0) {
            break;
        }

        req.type = CART_READ_SEC_CONT;
        req.offset = position;
        ret = bulkTransfer(kOutEndpoint, reinterpret_cast<unsigned char *>(&req), sizeof(req), &actual, 5000);
        if (ret != 0 || actual != static_cast<int>(sizeof(req))) {
            setLastError(QStringLiteral("Flash read continuation failed (%1)").arg(ret));
            if (errorString) {
                *errorString = lastError();
            }
            return false;
        }
    }

    return true;
}

