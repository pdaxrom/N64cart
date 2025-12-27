#pragma once

#include <QMainWindow>
#include <QAbstractItemView>
#include <QList>
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QUrl>
#include <QString>
#include <QElapsedTimer>
#include <functional>
#include <memory>
#include <optional>

#include "romfsdevice.h"
#include "romfsmodel.h"

namespace Ui
{
class MainWindow;
}

class QLabel;
class QLineEdit;
class QProgressBar;
class RomfsView;
class RomfsIconView;
class RomfsModel;
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void connectUsb();
    void connectRemote();
    void disconnectDevice();
    void refreshListing();
    void navigateUp();
    void onPathEdited();
    void onItemActivated(const QModelIndex &index);
    void onContextMenuRequested(const QPoint &pos);
    void uploadFiles();
    void downloadSelection();
    void deleteSelection();
    void createDirectory();
    void renameSelection();
    void formatRomfs();
    void rebootCart();
    void enterBootloader();
    void openSettingsDialog();
    void openUsage();
    void handleDroppedUrls(const QList<QUrl> &urls);
    void handleStartDrag(const QModelIndexList &indexes, Qt::DropActions actions);
    void handleOperationProgress(const QString &description, quint64 processed, quint64 total);
    void setIconMode();
    void setListMode();
    void openAbout();

private:
    void setupUi();
    void setupActions();
    void updateActions();
    void updateStatus();
    bool ensureConnected();
    void changePath(const QString &path);
    void loadDirectory();
    QString childPath(const QString &base, const QString &name) const;
    QVector<RomfsEntry> selectedEntries() const;
    QString chooseDirectory(const QString &title, const QString &dir = QString()) const;
    QString requestText(const QString &title, const QString &label, const QString &text = QString()) const;
    bool confirm(const QString &title, const QString &text) const;
    void showError(const QString &message);
    void showInfo(const QString &message);
    bool runWithProgress(const QString &title, const std::function<bool(QString &)> &operation, bool showCopyProgress = false, quint64 totalFiles = 0);
    QIcon getIcon(const QString &name) const;
    bool uploadPathRecursive(const QString &localPath, const QString &remoteDir, QString *error, std::optional<bool> *fixRomDecision);
    bool ensureRemoteDirectory(const QString &remotePath, QString *error);
    bool downloadEntryRecursive(const RomfsEntry &entry, const QString &targetDir, QString *error);
    bool downloadEntriesToDirectory(const QVector<RomfsEntry> &entries, const QString &targetDir);
    void setUiEnabled(bool enabled);
    bool uploadFileWithSettings(const QString &localPath, const QString &remotePath, QString *error, std::optional<bool> *fixRomDecision);
    bool isRomFile(const QString &path) const;
    enum class FixRomPromptResult {
        Fix,
        Skip,
        Cancel
    };
    FixRomPromptResult promptFixRomChoice(const QString &fileName, bool *dontAskAgain);
    void loadSettings();
    void saveSettings() const;
    QString formatDuration(qint64 milliseconds) const;
    bool confirmOverwriteFile(const QString &path) const;
    bool countLocalSelection(const QStringList &paths, quint64 *totalFiles, QString *error) const;
    bool countLocalFilesRecursive(const QString &path, quint64 *totalFiles, QString *error) const;
    bool countRemoteEntries(const QVector<RomfsEntry> &entries, quint64 *totalFiles, QString *error);
    bool countRemoteEntryRecursive(const RomfsEntry &entry, quint64 *totalFiles, QString *error);
    void beginFileCountdown(quint64 totalFiles);
    void finishFileCountdown();
    void handleFileTransferCompleted();
    bool shouldShowFileCountdown() const;

    struct AppSettings {
        bool fixRomEnabled = false;
        bool askFixRom = true;
        QString lastRemoteAddress = QStringLiteral("127.0.0.1:6464");
    };

    std::unique_ptr<Ui::MainWindow> ui_;
    RomfsDevice device_;
    std::unique_ptr<RomfsModel> model_;
    RomfsView *treeView_ = nullptr;
    RomfsIconView *iconView_ = nullptr;
    QAbstractItemView *currentView_ = nullptr;
    QLineEdit *pathEdit_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QProgressBar *progressBar_ = nullptr;
    QString currentPath_ = QStringLiteral("/");

    QAction *connectUsbAction_ = nullptr;
    QAction *connectRemoteAction_ = nullptr;
    QAction *disconnectAction_ = nullptr;
    QAction *refreshAction_ = nullptr;
    QAction *uploadAction_ = nullptr;
    QAction *downloadAction_ = nullptr;
    QAction *deleteAction_ = nullptr;
    QAction *newFolderAction_ = nullptr;
    QAction *renameAction_ = nullptr;
    QAction *formatAction_ = nullptr;
    QAction *rebootAction_ = nullptr;
    QAction *bootloaderAction_ = nullptr;
    QAction *iconViewAction_ = nullptr;
    QAction *listViewAction_ = nullptr;
    QAction *settingsAction_ = nullptr;
    QAction *usageAction_ = nullptr;
    QAction *aboutAction_ = nullptr;
    AppSettings settings_;
    bool copyProgressActive_ = false;
    QElapsedTimer copyProgressTimer_;
    bool fileCountdownActive_ = false;
    quint64 fileCountdownTotal_ = 0;
    quint64 fileCountdownRemaining_ = 0;

};
