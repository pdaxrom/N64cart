#pragma once

#include <QHostAddress>

#include "romfstransport.h"

#include "../simple-connection-lib/src/tcp.h"

class RemoteTransport : public RomfsTransport
{
    Q_OBJECT

public:
    RemoteTransport(const QHostAddress &address, quint16 port, QObject *parent = nullptr);
    ~RemoteTransport() override;

    bool connectDevice(QString *errorString = nullptr) override;
    void disconnectDevice() override;

    bool sendCommand(uint16_t type, ack_header *ack, QString *errorString = nullptr) override;
    bool eraseSector(uint32_t offset, QString *errorString = nullptr) override;
    bool writeSector(uint32_t offset, const uint8_t *buffer, QString *errorString = nullptr) override;
    bool readSector(uint32_t offset, uint8_t *buffer, uint32_t length, QString *errorString = nullptr) override;

private:
    bool writeAll(const void *data, size_t length, QString *errorString);
    bool readAll(void *data, size_t length, QString *errorString);

    QHostAddress address_;
    quint16 port_ = 0;
    tcp_channel *channel_ = nullptr;
};
