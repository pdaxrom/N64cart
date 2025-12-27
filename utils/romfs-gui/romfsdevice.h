#pragma once

#include <QObject>
#include <QHostAddress>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

#include "../utils2.h"
#include "romfstransport.h"

struct RomfsEntry {
    QString name;
    QString path;
    bool isDirectory = false;
    quint64 size = 0;
    quint16 mode = 0;
    quint16 type = 0;
};

class RomfsDevice : public QObject
{
    Q_OBJECT

public:
    enum class TransportType {
        None,
        Usb,
        Remote
    };

    explicit RomfsDevice(QObject *parent = nullptr);
    ~RomfsDevice() override;

    bool connectUsb();
    bool connectRemote(const QHostAddress &address, quint16 port);
    void disconnect();

    bool isConnected() const;
    TransportType transportType() const;

    QVector<RomfsEntry> list(const QString &path, QString *errorString = nullptr);
    bool uploadFile(const QString &localPath, const QString &remotePath, bool fixRom, int piBusSpeed, QString *errorString = nullptr);
    bool downloadFile(const QString &remotePath, const QString &localPath, QString *errorString = nullptr);
    bool removeEntry(const QString &remotePath, QString *errorString = nullptr);
    bool makeDirectory(const QString &remotePath, QString *errorString = nullptr);
    bool removeDirectory(const QString &remotePath, QString *errorString = nullptr);
    bool renameEntry(const QString &oldPath, const QString &newPath, bool createDirs, QString *errorString = nullptr);
    bool format(QString *errorString = nullptr);
    quint64 freeSpace(QString *errorString = nullptr);
    bool reboot(QString *errorString = nullptr);
    bool bootloader(QString *errorString = nullptr);

    QString lastError() const;
    const ack_header &cartInfo() const
    {
        return cartInfo_;
    }

signals:
    void connectionStateChanged(bool connected);
    void operationProgress(const QString &description, quint64 processed, quint64 total);

private:
    bool ensureTransport(QString *errorString);
    bool enterSpiMode(QString *errorString);
    bool leaveSpiMode();
    bool restartRomfs(QString *errorString);
    bool runRomfsOperation(const std::function<bool(QString *)> &operation, QString *errorString);
    QVector<RomfsEntry> readDirectory(const QString &path, QString *errorString);
    QByteArray normalizePath(const QString &path) const;
    void setError(const QString &message, QString *errorString = nullptr);

    TransportType currentTransport_ = TransportType::None;
    std::unique_ptr<RomfsTransport> transport_;
    ack_header cartInfo_ = {};
    QByteArray flashMap_;
    QByteArray flashList_;
    QByteArray flashBuffer_;
    bool flashInSpiMode_ = false;
    QString lastError_;
};
