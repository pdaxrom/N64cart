#include "remotetransport.h"

#include <QByteArray>
#include <QtEndian>

#include "romfs.h"

#include "../proxy-romfs.h"
#include "../simple-connection-lib/src/tcp.h"

struct RemoteSectorCommand {
    uint16_t command;
    sector_info info;
} __attribute__((packed));

struct CommandPacket {
    uint16_t command;
    req_header header;
} __attribute__((packed));

RemoteTransport::RemoteTransport(const QHostAddress &address, quint16 port, QObject *parent)
    : RomfsTransport(parent)
    , address_(address)
    , port_(port)
{
}

RemoteTransport::~RemoteTransport()
{
    disconnectDevice();
}

bool RemoteTransport::connectDevice(QString *errorString)
{
    if (channel_) {
        return true;
    }

    QByteArray hostBytes = address_.toString().toLatin1();
    channel_ = tcp_open(TCP_CLIENT, hostBytes.constData(), port_, nullptr, nullptr);
    if (!channel_) {
        setLastError(QStringLiteral("Cannot connect to ROMFS proxy at %1:%2").arg(address_.toString()).arg(port_));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }
    return true;
}

void RemoteTransport::disconnectDevice()
{
    if (channel_) {
        tcp_close(channel_);
        channel_ = nullptr;
    }
}

bool RemoteTransport::writeAll(const void *data, size_t length, QString *errorString)
{
    if (!channel_) {
        if (errorString) {
            *errorString = QStringLiteral("Remote channel is not connected");
        }
        return false;
    }

    size_t written = 0;
    const char *ptr = static_cast<const char *>(data);
    while (written < length) {
        int ret = tcp_write(channel_, const_cast<char *>(ptr + written), length - written);
        if (ret <= 0) {
            setLastError(QStringLiteral("TCP write error"));
            if (errorString) {
                *errorString = lastError();
            }
            return false;
        }
        written += static_cast<size_t>(ret);
    }
    return true;
}

bool RemoteTransport::readAll(void *data, size_t length, QString *errorString)
{
    if (!channel_) {
        if (errorString) {
            *errorString = QStringLiteral("Remote channel is not connected");
        }
        return false;
    }

    size_t readBytes = 0;
    char *ptr = static_cast<char *>(data);
    while (readBytes < length) {
        int ret = tcp_read(channel_, ptr + readBytes, length - readBytes);
        if (ret <= 0) {
            setLastError(QStringLiteral("TCP read error"));
            if (errorString) {
                *errorString = lastError();
            }
            return false;
        }
        readBytes += static_cast<size_t>(ret);
    }
    return true;
}

bool RemoteTransport::sendCommand(uint16_t type, ack_header *ack, QString *errorString)
{
    if (!connectDevice(errorString)) {
        return false;
    }

    CommandPacket packet;
    packet.command = qToBigEndian<uint16_t>(USB_CMD);
    packet.header.type = qToBigEndian<uint16_t>(type);
    packet.header.offset = 0;

    if (!writeAll(&packet, sizeof(CommandPacket), errorString)) {
        return false;
    }

    ack_header reply;
    ack_header *target = ack ? ack : &reply;
    if (!readAll(target, sizeof(ack_header), errorString)) {
        return false;
    }

    target->type = qFromBigEndian<uint16_t>(target->type);
    target->info.start = qFromBigEndian<uint32_t>(target->info.start);
    target->info.size = qFromBigEndian<uint32_t>(target->info.size);
    target->info.vers = qFromBigEndian<uint32_t>(target->info.vers);

    if (target->type != ACK_NOERROR) {
        setLastError(QStringLiteral("Remote command failed (0x%1)").arg(target->type, 0, 16));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }

    return true;
}

bool RemoteTransport::eraseSector(uint32_t offset, QString *errorString)
{
    if (!connectDevice(errorString)) {
        return false;
    }

    RemoteSectorCommand command;
    command.command = qToBigEndian<uint16_t>(USB_ERASE_SECTOR);
    command.info.offset = qToBigEndian<uint32_t>(offset);
    command.info.length = 0;

    if (!writeAll(&command, sizeof(command), errorString)) {
        return false;
    }

    uint8_t ok = 0;
    if (!readAll(&ok, sizeof(ok), errorString)) {
        return false;
    }

    if (!ok) {
        setLastError(QStringLiteral("Remote erase command failed"));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }

    return true;
}

bool RemoteTransport::writeSector(uint32_t offset, const uint8_t *buffer, QString *errorString)
{
    if (!connectDevice(errorString)) {
        return false;
    }

    RemoteSectorCommand command;
    command.command = qToBigEndian<uint16_t>(USB_WRITE_SECTOR);
    command.info.offset = qToBigEndian<uint32_t>(offset);
    command.info.length = qToBigEndian<uint32_t>(ROMFS_FLASH_SECTOR);

    if (!writeAll(&command, sizeof(command), errorString)) {
        return false;
    }

    if (!writeAll(buffer, ROMFS_FLASH_SECTOR, errorString)) {
        return false;
    }

    uint8_t ok = 0;
    if (!readAll(&ok, sizeof(ok), errorString)) {
        return false;
    }

    if (!ok) {
        setLastError(QStringLiteral("Remote write command failed"));
        if (errorString) {
            *errorString = lastError();
        }
        return false;
    }
    return true;
}

bool RemoteTransport::readSector(uint32_t offset, uint8_t *buffer, uint32_t length, QString *errorString)
{
    if (!connectDevice(errorString)) {
        return false;
    }

    RemoteSectorCommand command;
    command.command = qToBigEndian<uint16_t>(USB_READ_SECTOR);
    command.info.offset = qToBigEndian<uint32_t>(offset);
    command.info.length = qToBigEndian<uint32_t>(length);

    if (!writeAll(&command, sizeof(command), errorString)) {
        return false;
    }

    if (!readAll(buffer, length, errorString)) {
        return false;
    }

    return true;
}
