#include "romfsdevice.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <cstring>
#include <utility>

#include "romfs.h"

#include "remotetransport.h"
#include "romfsbridge.h"
#include "usbtransport.h"

namespace
{
static QByteArray toPathBytes(const QString &path)
{
    QString normalized = path;
    if (normalized.isEmpty()) {
        normalized = QStringLiteral("/");
    }
    if (normalized != QStringLiteral("/") && normalized.endsWith(QLatin1Char('/'))) {
        normalized.chop(1);
    }
    return normalized.toUtf8();
}

static QString buildChildPath(const QString &parent, const QString &name)
{
    if (parent.isEmpty() || parent == QLatin1String("/")) {
        return QStringLiteral("/%1").arg(name);
    }
    if (parent.endsWith(QLatin1Char('/'))) {
        return parent + name;
    }
    return parent + QLatin1Char('/') + name;
}
}

RomfsDevice::RomfsDevice(QObject *parent)
    : QObject(parent)
{
    flashBuffer_.resize(ROMFS_FLASH_SECTOR);
}

RomfsDevice::~RomfsDevice()
{
    disconnect();
}

bool RomfsDevice::connectUsb()
{
    disconnect();
    auto transport = std::make_unique<UsbTransport>(this);
    QString error;
    if (!transport->connectDevice(&error)) {
        setError(error);
        return false;
    }

    transport_ = std::move(transport);
    currentTransport_ = TransportType::Usb;
    registerRomfsTransport(transport_.get());

    if (!transport_->sendCommand(CART_INFO, &cartInfo_, &error)) {
        setError(error);
        disconnect();
        return false;
    }

    emit connectionStateChanged(true);
    return true;
}

bool RomfsDevice::connectRemote(const QHostAddress &address, quint16 port)
{
    disconnect();
    auto transport = std::make_unique<RemoteTransport>(address, port, this);
    QString error;
    if (!transport->connectDevice(&error)) {
        setError(error);
        return false;
    }

    transport_ = std::move(transport);
    currentTransport_ = TransportType::Remote;
    registerRomfsTransport(transport_.get());

    if (!transport_->sendCommand(CART_INFO, &cartInfo_, &error)) {
        setError(error);
        disconnect();
        return false;
    }

    emit connectionStateChanged(true);
    return true;
}

void RomfsDevice::disconnect()
{
    if (currentTransport_ == TransportType::None) {
        return;
    }

    leaveSpiMode();
    if (transport_) {
        transport_->disconnectDevice();
    }
    transport_.reset();
    registerRomfsTransport(nullptr);
    currentTransport_ = TransportType::None;
    flashInSpiMode_ = false;
    emit connectionStateChanged(false);
}

bool RomfsDevice::isConnected() const
{
    return currentTransport_ != TransportType::None;
}

RomfsDevice::TransportType RomfsDevice::transportType() const
{
    return currentTransport_;
}

QString RomfsDevice::lastError() const
{
    return lastError_;
}

QVector<RomfsEntry> RomfsDevice::list(const QString &path, QString *errorString)
{
    QVector<RomfsEntry> entries;

    bool ok = runRomfsOperation([&](QString *err) {
        entries = readDirectory(path, err);
        return err == nullptr || err->isEmpty();
    }, errorString);

    if (!ok && errorString && errorString->isEmpty()) {
        *errorString = lastError_;
    }
    return entries;
}

bool RomfsDevice::uploadFile(const QString &localPath, const QString &remotePath, bool fixRom, int piBusSpeed, QString *errorString)
{
    QFileInfo info(localPath);
    if (!info.exists()) {
        setError(QStringLiteral("Local file does not exist: %1").arg(localPath), errorString);
        return false;
    }

    return runRomfsOperation([&](QString *err) {
        QFile in(localPath);
        if (!in.open(QIODevice::ReadOnly)) {
            setError(QStringLiteral("Cannot open %1 for reading").arg(localPath), err);
            return false;
        }

        QByteArray remoteBytes = toPathBytes(remotePath);
        romfs_file romFile;
        uint32_t res = romfs_create_path(remoteBytes.constData(), &romFile, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, reinterpret_cast<uint8_t *>(flashBuffer_.data()), true);
        if (res != ROMFS_NOERR) {
            setError(QStringLiteral("ROMFS create failed: %1").arg(QString::fromUtf8(romfs_strerror(res))), err);
            return false;
        }

        QByteArray chunk(ROMFS_FLASH_SECTOR, Qt::Uninitialized);
        qint64 total = 0;
        qint64 fileSize = info.size();
        int romType = -1;
        bool fixPiFreq = (piBusSpeed >= 0);
        while (true) {
            qint64 read = in.read(chunk.data(), chunk.size());
            if (read <= 0) {
                break;
            }

            if (fixRom) {
                if (romType == -1) {
                    const unsigned char *data = reinterpret_cast<const unsigned char *>(chunk.constData());
                    if (read >= 4) {
                        if (data[0] == 0x80 && data[1] == 0x37 && data[2] == 0x12 && data[3] == 0x40) {
                            romType = 0; // Z64
                        } else if (data[0] == 0x40 && data[1] == 0x12 && data[2] == 0x37 && data[3] == 0x80) {
                            romType = 1; // N64
                        } else if (data[0] == 0x37 && data[1] == 0x80 && data[2] == 0x40 && data[3] == 0x12) {
                            romType = 2; // V64
                        } else {
                            setError(QStringLiteral("Unknown ROM byte order"), err);
                            break;
                        }
                    }
                }

                if (read % 4 != 0) {
                    setError(QStringLiteral("Unaligned ROM data chunk"), err);
                    break;
                }

                if (romType == 1 || romType == 2) {
                    uint8_t *data = reinterpret_cast<uint8_t *>(chunk.data());
                    for (qint64 i = 0; i < read; i += 4) {
                        if (romType == 1) {
                            std::swap(data[i + 0], data[i + 3]);
                            std::swap(data[i + 1], data[i + 2]);
                        } else {
                            std::swap(data[i + 0], data[i + 1]);
                            std::swap(data[i + 2], data[i + 3]);
                        }
                    }
                }
            }

            if (fixPiFreq) {
                uint8_t *data = reinterpret_cast<uint8_t *>(chunk.data());
                if (read >= 4 && data[0] == 0x80 && data[1] == 0x37 && data[3] == 0x40) {
                    data[2] = static_cast<uint8_t>(piBusSpeed & 0xff);
                    fixPiFreq = false;
                } else {
                    setError(QStringLiteral("PI bus fix requires Z64 byte order"), err);
                    break;
                }
            }

            if (romfs_write_file(chunk.constData(), static_cast<uint32_t>(read), &romFile) == 0) {
                break;
            }
            total += read;
            QString description = tr("Uploading %1").arg(info.fileName());
            if (fixRom && romType >= 0 && romType <= 2) {
                QString romLabel;
                switch (romType) {
                case 0:
                    romLabel = QStringLiteral("Z64");
                    break;
                case 1:
                    romLabel = QStringLiteral("N64");
                    break;
                case 2:
                    romLabel = QStringLiteral("V64");
                    break;
                default:
                    break;
                }
                if (!romLabel.isEmpty()) {
                    description += tr(" (%1)").arg(romLabel);
                }
            }
            emit operationProgress(description, total, fileSize);
            QCoreApplication::processEvents();
        }

        if (romFile.err != ROMFS_NOERR && romFile.err != ROMFS_ERR_EOF) {
            setError(QStringLiteral("Write failed: %1").arg(QString::fromUtf8(romfs_strerror(romFile.err))), err);
            romfs_close_file(&romFile);
            return false;
        }

        if (romfs_close_file(&romFile) != ROMFS_NOERR) {
            setError(QStringLiteral("Unable to close remote file"), err);
            return false;
        }
        return true;
    }, errorString);
}

bool RomfsDevice::downloadFile(const QString &remotePath, const QString &localPath, QString *errorString)
{
    return runRomfsOperation([&](QString *err) {
        QFile out(localPath);
        if (!out.open(QIODevice::WriteOnly)) {
            setError(QStringLiteral("Cannot open %1 for writing").arg(localPath), err);
            return false;
        }

        QByteArray remoteBytes = toPathBytes(remotePath);
        romfs_file romFile;
        if (romfs_open_path(remoteBytes.constData(), &romFile, reinterpret_cast<uint8_t *>(flashBuffer_.data())) != ROMFS_NOERR) {
            setError(QStringLiteral("Cannot open %1: %2").arg(remotePath, QString::fromUtf8(romfs_strerror(romFile.err))), err);
            return false;
        }

        QByteArray chunk(ROMFS_FLASH_SECTOR, Qt::Uninitialized);
        while (true) {
            int read = romfs_read_file(chunk.data(), chunk.size(), &romFile);
            if (read <= 0) {
                break;
            }
            out.write(chunk.constData(), read);
            emit operationProgress(tr("Downloading %1").arg(remotePath), romFile.read_offset, romFile.entry.size);
            QCoreApplication::processEvents();
        }

        if (romFile.err != ROMFS_NOERR && romFile.err != ROMFS_ERR_EOF) {
            setError(QStringLiteral("Read failed: %1").arg(QString::fromUtf8(romfs_strerror(romFile.err))), err);
            romfs_close_file(&romFile);
            return false;
        }

        romfs_close_file(&romFile);
        return true;
    }, errorString);
}

bool RomfsDevice::removeEntry(const QString &remotePath, QString *errorString)
{
    return runRomfsOperation([&](QString *err) {
        QByteArray path = toPathBytes(remotePath);
        uint32_t rc = romfs_delete_path(path.constData());
        if (rc != ROMFS_NOERR) {
            setError(QStringLiteral("Delete failed: %1").arg(QString::fromUtf8(romfs_strerror(rc))), err);
            return false;
        }
        return true;
    }, errorString);
}

bool RomfsDevice::makeDirectory(const QString &remotePath, QString *errorString)
{
    return runRomfsOperation([&](QString *err) {
        QByteArray path = toPathBytes(remotePath);
        romfs_dir dir;
        uint32_t rc = romfs_mkdir_path(path.constData(), true, &dir);
        if (rc != ROMFS_NOERR) {
            setError(QStringLiteral("mkdir failed: %1").arg(QString::fromUtf8(romfs_strerror(rc))), err);
            return false;
        }
        return true;
    }, errorString);
}

bool RomfsDevice::removeDirectory(const QString &remotePath, QString *errorString)
{
    return runRomfsOperation([&](QString *err) {
        QByteArray path = toPathBytes(remotePath);
        uint32_t rc = romfs_rmdir_path(path.constData());
        if (rc != ROMFS_NOERR) {
            setError(QStringLiteral("rmdir failed: %1").arg(QString::fromUtf8(romfs_strerror(rc))), err);
            return false;
        }
        return true;
    }, errorString);
}

bool RomfsDevice::renameEntry(const QString &oldPath, const QString &newPath, bool createDirs, QString *errorString)
{
    return runRomfsOperation([&](QString *err) {
        QByteArray src = toPathBytes(oldPath);
        QByteArray dst = toPathBytes(newPath);
        uint32_t rc = romfs_rename_path(src.constData(), dst.constData(), createDirs);
        if (rc != ROMFS_NOERR) {
            setError(QStringLiteral("rename failed: %1").arg(QString::fromUtf8(romfs_strerror(rc))), err);
            return false;
        }
        return true;
    }, errorString);
}

bool RomfsDevice::format(QString *errorString)
{
    return runRomfsOperation([&](QString *err) {
        if (!romfs_format()) {
            setError(QStringLiteral("Format failed"), err);
            return false;
        }
        return true;
    }, errorString);
}

quint64 RomfsDevice::freeSpace(QString *errorString)
{
    quint64 freeBytes = 0;
    runRomfsOperation([&](QString *err) {
        Q_UNUSED(err)
        freeBytes = romfs_free();
        return true;
    }, errorString);
    return freeBytes;
}

bool RomfsDevice::reboot(QString *errorString)
{
    if (!ensureTransport(errorString)) {
        return false;
    }
    if (!transport_->sendCommand(CART_REBOOT, nullptr, errorString)) {
        return false;
    }
    return true;
}

bool RomfsDevice::bootloader(QString *errorString)
{
    if (!ensureTransport(errorString)) {
        return false;
    }
    if (!transport_->sendCommand(BOOTLOADER_MODE, nullptr, errorString)) {
        return false;
    }
    return true;
}

bool RomfsDevice::ensureTransport(QString *errorString)
{
    if (!transport_) {
        setError(QStringLiteral("No active transport"), errorString);
        return false;
    }
    return true;
}

bool RomfsDevice::enterSpiMode(QString *errorString)
{
    if (flashInSpiMode_) {
        return true;
    }

    if (!transport_->sendCommand(FLASH_SPI_MODE, nullptr, errorString)) {
        return false;
    }

    if (!restartRomfs(errorString)) {
        return false;
    }

    flashInSpiMode_ = true;
    return true;
}

bool RomfsDevice::leaveSpiMode()
{
    if (!transport_ || !flashInSpiMode_) {
        return true;
    }

    QString error;
    bool ok = transport_->sendCommand(FLASH_QUAD_MODE, nullptr, &error);
    if (!ok) {
        qWarning() << "Failed to switch flash back to quad mode:" << error;
    }
    flashInSpiMode_ = false;
    return ok;
}

bool RomfsDevice::restartRomfs(QString *errorString)
{
    uint32_t mapSize = 0;
    uint32_t listSize = 0;
    romfs_get_buffers_sizes(cartInfo_.info.size, &mapSize, &listSize);
    if (flashMap_.size() != static_cast<int>(mapSize)) {
        flashMap_.resize(mapSize);
    }
    if (flashList_.size() != static_cast<int>(listSize)) {
        flashList_.resize(listSize);
    }

    auto *map = reinterpret_cast<uint16_t *>(flashMap_.data());
    auto *list = reinterpret_cast<uint8_t *>(flashList_.data());
    if (!romfs_start(cartInfo_.info.start, cartInfo_.info.size, map, list)) {
        setError(QStringLiteral("Cannot start ROMFS"), errorString);
        return false;
    }
    return true;
}

bool RomfsDevice::runRomfsOperation(const std::function<bool(QString *)> &operation, QString *errorString)
{
    if (!ensureTransport(errorString)) {
        return false;
    }

    if (!enterSpiMode(errorString)) {
        return false;
    }

    QString opError;
    bool ok = operation(errorString ? errorString : &opError);

    leaveSpiMode();

    if (!ok && errorString && errorString->isEmpty()) {
        *errorString = opError;
    }

    return ok;
}

QVector<RomfsEntry> RomfsDevice::readDirectory(const QString &path, QString *errorString)
{
    QVector<RomfsEntry> entries;
    romfs_dir dir;
    uint32_t err = ROMFS_NOERR;
    if (path.isEmpty() || path == QLatin1String("/")) {
        err = romfs_dir_root(&dir);
    } else {
        QByteArray bytes = toPathBytes(path);
        err = romfs_dir_open_path(bytes.constData(), &dir);
    }

    if (err != ROMFS_NOERR) {
        setError(QStringLiteral("Cannot open %1: %2").arg(path, QString::fromUtf8(romfs_strerror(err))), errorString);
        return entries;
    }

    romfs_file file = {};
    uint32_t listErr = romfs_list_dir(&file, true, &dir, true);
    if (listErr == ROMFS_ERR_NO_FREE_ENTRIES) {
        return entries;
    }

    if (listErr != ROMFS_NOERR) {
        setError(QStringLiteral("List failed: %1").arg(QString::fromUtf8(romfs_strerror(listErr))), errorString);
        return entries;
    }

    do {
        RomfsEntry entry;
        entry.name = QString::fromUtf8(file.entry.name);
        entry.path = buildChildPath(path, entry.name);
        entry.isDirectory = (file.entry.attr.names.type == ROMFS_TYPE_DIR);
        entry.size = file.entry.size;
        entry.mode = file.entry.attr.names.mode;
        entry.type = file.entry.attr.names.type;
        entries.append(entry);
    } while (romfs_list_dir(&file, false, &dir, true) == ROMFS_NOERR);

    return entries;
}

void RomfsDevice::setError(const QString &message, QString *errorString)
{
    lastError_ = message;
    if (errorString) {
        *errorString = message;
    }
}
