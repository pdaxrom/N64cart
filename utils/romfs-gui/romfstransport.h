#pragma once

#include <QObject>
#include <QString>

extern "C" {
#include "../utils2.h"
}

class RomfsTransport : public QObject
{
    Q_OBJECT

public:
    explicit RomfsTransport(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    virtual bool connectDevice(QString *errorString = nullptr) = 0;
    virtual void disconnectDevice() = 0;

    virtual bool sendCommand(uint16_t type, ack_header *ack, QString *errorString = nullptr) = 0;
    virtual bool eraseSector(uint32_t offset, QString *errorString = nullptr) = 0;
    virtual bool writeSector(uint32_t offset, const uint8_t *buffer, QString *errorString = nullptr) = 0;
    virtual bool readSector(uint32_t offset, uint8_t *buffer, uint32_t length, QString *errorString = nullptr) = 0;

    QString lastError() const
    {
        return lastError_;
    }

protected:
    void setLastError(const QString &message)
    {
        lastError_ = message;
    }

private:
    QString lastError_;
};
