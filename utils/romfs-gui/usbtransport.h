#pragma once

#include "romfstransport.h"

#include <libusb.h>

class UsbTransport : public RomfsTransport
{
    Q_OBJECT

public:
    explicit UsbTransport(QObject *parent = nullptr);
    ~UsbTransport() override;

    bool connectDevice(QString *errorString = nullptr) override;
    void disconnectDevice() override;

    bool sendCommand(uint16_t type, ack_header *ack, QString *errorString = nullptr) override;
    bool eraseSector(uint32_t offset, QString *errorString = nullptr) override;
    bool writeSector(uint32_t offset, const uint8_t *buffer, QString *errorString = nullptr) override;
    bool readSector(uint32_t offset, uint8_t *buffer, uint32_t length, QString *errorString = nullptr) override;

private:
    int bulkTransfer(unsigned char endpoint, unsigned char *data, int length, int *transferred, unsigned int timeout);
    bool ensureConnected(QString *errorString);

    libusb_context *ctx_ = nullptr;
    libusb_device_handle *handle_ = nullptr;
    bool interfaceClaimed_ = false;
};

