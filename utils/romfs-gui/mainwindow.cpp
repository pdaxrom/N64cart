#include "mainwindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "romfsiconview.h"
#include "romfsmodel.h"
#include "romfsview.h"
#include "settingsdialog.h"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setupUi();
  setupActions();
  loadSettings();

  connect(&device_, &RomfsDevice::connectionStateChanged, this, [this](bool) {
    updateActions();
    updateStatus();
  });
  connect(&device_, &RomfsDevice::operationProgress, this,
          &MainWindow::handleOperationProgress);

  changePath(currentPath_);
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi() {
  ui_ = std::make_unique<Ui::MainWindow>();
  ui_->setupUi(this);

  model_ = std::make_unique<RomfsModel>(this);

  treeView_ = ui_->treeView;
  treeView_->setModel(model_.get());
  treeView_->setRootIsDecorated(false);
  treeView_->setUniformRowHeights(true);
  treeView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  treeView_->setSelectionBehavior(QAbstractItemView::SelectRows);
  treeView_->setAlternatingRowColors(true);
  treeView_->setContextMenuPolicy(Qt::CustomContextMenu);
  treeView_->header()->setStretchLastSection(true);

  iconView_ = ui_->iconView;
  iconView_->setModel(model_.get());
  iconView_->setViewMode(QListView::IconMode);
  iconView_->setResizeMode(QListView::Adjust);
  iconView_->setSpacing(12);
  iconView_->setWrapping(true);
  iconView_->setContextMenuPolicy(Qt::CustomContextMenu);

  ui_->viewStack->setCurrentWidget(treeView_);
  currentView_ = treeView_;

  pathEdit_ = ui_->pathLineEdit;
  connect(pathEdit_, &QLineEdit::returnPressed, this,
          &MainWindow::onPathEdited);
  connect(ui_->upButton, &QToolButton::clicked, this, &MainWindow::navigateUp);

  auto connectViewSignals = [this](QAbstractItemView *view) {
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QAbstractItemView::doubleClicked, this,
            &MainWindow::onItemActivated);
    connect(view, &QWidget::customContextMenuRequested, this,
            &MainWindow::onContextMenuRequested);
    if (auto *tree = qobject_cast<RomfsView *>(view)) {
      connect(tree, &RomfsView::urlsDropped, this,
              &MainWindow::handleDroppedUrls);
      connect(tree, &RomfsView::startDragRequested, this,
              &MainWindow::handleStartDrag);
    } else if (auto *list = qobject_cast<RomfsIconView *>(view)) {
      connect(list, &RomfsIconView::urlsDropped, this,
              &MainWindow::handleDroppedUrls);
      connect(list, &RomfsIconView::startDragRequested, this,
              &MainWindow::handleStartDrag);
    }
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this, view]() {
              if (view == currentView_) {
                updateActions();
              }
            });
  };

  connectViewSignals(treeView_);
  connectViewSignals(iconView_);

  statusLabel_ = new QLabel(this);
  progressBar_ = new QProgressBar(this);
  progressBar_->setRange(0, 100);
  progressBar_->setValue(0);
  progressBar_->setTextVisible(false);
  progressBar_->setMinimumWidth(160);
  progressBar_->setVisible(false);
  auto *statusContainer = new QWidget(this);
  auto *statusLayout = new QHBoxLayout(statusContainer);
  statusLayout->setContentsMargins(0, 0, 0, 0);
  statusLayout->setSpacing(8);
  statusLayout->addWidget(statusLabel_, 1);
  statusLayout->addWidget(progressBar_);
  ui_->statusbar->addPermanentWidget(statusContainer, 1);
}

void MainWindow::setupActions() {
  connectUsbAction_ = ui_->actionConnectUsb;
  connect(connectUsbAction_, &QAction::triggered, this,
          &MainWindow::connectUsb);

  connectRemoteAction_ = ui_->actionConnectRemote;
  connect(connectRemoteAction_, &QAction::triggered, this,
          &MainWindow::connectRemote);

  disconnectAction_ = ui_->actionDisconnect;
  connect(disconnectAction_, &QAction::triggered, this,
          &MainWindow::disconnectDevice);

  refreshAction_ = ui_->actionRefresh;
  connect(refreshAction_, &QAction::triggered, this,
          &MainWindow::refreshListing);

  uploadAction_ = ui_->actionUpload;
  connect(uploadAction_, &QAction::triggered, this, &MainWindow::uploadFiles);

  downloadAction_ = ui_->actionDownload;
  connect(downloadAction_, &QAction::triggered, this,
          &MainWindow::downloadSelection);

  deleteAction_ = ui_->actionDelete;
  connect(deleteAction_, &QAction::triggered, this,
          &MainWindow::deleteSelection);

  newFolderAction_ = ui_->actionNewFolder;
  connect(newFolderAction_, &QAction::triggered, this,
          &MainWindow::createDirectory);

  renameAction_ = ui_->actionRename;
  connect(renameAction_, &QAction::triggered, this,
          &MainWindow::renameSelection);

  formatAction_ = ui_->actionFormat;
  connect(formatAction_, &QAction::triggered, this, &MainWindow::formatRomfs);

  settingsAction_ = ui_->actionSettings;
  connect(settingsAction_, &QAction::triggered, this,
          &MainWindow::openSettingsDialog);

  usageAction_ = ui_->actionUsage;
  connect(usageAction_, &QAction::triggered, this, &MainWindow::openUsage);

  aboutAction_ = ui_->actionAbout;
  connect(aboutAction_, &QAction::triggered, this, &MainWindow::openAbout);

  rebootAction_ = ui_->actionReboot;
  connect(rebootAction_, &QAction::triggered, this, &MainWindow::rebootCart);

  bootloaderAction_ = ui_->actionBootloader;
  connect(bootloaderAction_, &QAction::triggered, this,
          &MainWindow::enterBootloader);

  iconViewAction_ = ui_->actionIconView;
  listViewAction_ = ui_->actionListView;
  auto *viewGroup = new QActionGroup(this);
  viewGroup->addAction(iconViewAction_);
  viewGroup->addAction(listViewAction_);
  connect(iconViewAction_, &QAction::triggered, this, &MainWindow::setIconMode);
  connect(listViewAction_, &QAction::triggered, this, &MainWindow::setListMode);

  connect(ui_->actionQuit, &QAction::triggered, qApp, &QApplication::quit);

  connectUsbAction_->setIcon(getIcon("connect_usb"));
  connectRemoteAction_->setIcon(getIcon("connect_remote"));
  disconnectAction_->setIcon(getIcon("disconnect"));
  refreshAction_->setIcon(getIcon("refresh"));
  uploadAction_->setIcon(getIcon("upload"));
  downloadAction_->setIcon(getIcon("download"));
  deleteAction_->setIcon(getIcon("delete"));
  newFolderAction_->setIcon(getIcon("new_folder"));
  renameAction_->setIcon(getIcon("rename"));
  iconViewAction_->setIcon(getIcon("view_icon"));
  listViewAction_->setIcon(getIcon("view_list"));
  ui_->upButton->setIcon(getIcon("up"));

  updateActions();
}

void MainWindow::connectUsb() {
  if (!device_.connectUsb()) {
    showError(device_.lastError());
    return;
  }
  refreshListing();
}

void MainWindow::connectRemote() {
  bool ok = false;
  const QString input =
      QInputDialog::getText(this, tr("Remote Address"),
                            tr("Enter host and port (host:port):"),
                            QLineEdit::Normal, settings_.lastRemoteAddress, &ok)
          .trimmed();
  if (!ok || input.isEmpty()) {
    return;
  }

  const int colonIndex = input.lastIndexOf(QLatin1Char(':'));
  if (colonIndex <= 0 || colonIndex == input.size() - 1) {
    showError(tr("Address must be in host:port format."));
    return;
  }

  const QString host = input.left(colonIndex).trimmed();
  const QString portText = input.mid(colonIndex + 1).trimmed();
  bool portOk = false;
  const int port = portText.toInt(&portOk);
  if (!portOk || port < 1 || port > 65535 || host.isEmpty()) {
    showError(tr("Invalid host or port."));
    return;
  }

  QHostAddress address;
  if (!address.setAddress(host)) {
    showError(tr("Invalid IP address."));
    return;
  }

  if (!device_.connectRemote(address, static_cast<quint16>(port))) {
    showError(device_.lastError());
    return;
  }

  settings_.lastRemoteAddress = QStringLiteral("%1:%2").arg(host).arg(port);
  saveSettings();
  refreshListing();
}

void MainWindow::disconnectDevice() {
  device_.disconnect();
  model_->setEntries({});
  updateActions();
  updateStatus();
}

void MainWindow::refreshListing() {
  if (!ensureConnected()) {
    return;
  }
  loadDirectory();
}

void MainWindow::navigateUp() {
  if (currentPath_ == QStringLiteral("/")) {
    return;
  }
  QString dir = currentPath_;
  if (dir.endsWith(QLatin1Char('/'))) {
    dir.chop(1);
  }
  int slash = dir.lastIndexOf(QLatin1Char('/'));
  if (slash <= 0) {
    dir = QStringLiteral("/");
  } else {
    dir = dir.left(slash);
  }
  changePath(dir);
}

void MainWindow::onPathEdited() { changePath(pathEdit_->text()); }

void MainWindow::onItemActivated(const QModelIndex &index) {
  if (!index.isValid()) {
    return;
  }
  RomfsEntry entry = model_->entryAt(index.row());
  if (entry.isDirectory) {
    changePath(entry.path);
  }
}

void MainWindow::onContextMenuRequested(const QPoint &pos) {
  auto *sourceView = qobject_cast<QAbstractItemView *>(sender());
  if (!sourceView || sourceView != currentView_) {
    return;
  }
  QMenu menu(this);
  menu.addAction(uploadAction_);
  menu.addAction(downloadAction_);
  menu.addAction(newFolderAction_);
  menu.addAction(renameAction_);
  menu.addAction(deleteAction_);
  menu.exec(sourceView->viewport()->mapToGlobal(pos));
}

void MainWindow::uploadFiles() {
  if (!ensureConnected()) {
    return;
  }
  QFileDialog dialog(this, tr("Select files or folders to upload"));
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
  dialog.setOption(QFileDialog::ShowDirsOnly, false);
  if (!dialog.exec()) {
    return;
  }
  const QStringList files = dialog.selectedFiles();
  if (files.isEmpty()) {
    return;
  }

  quint64 totalFiles = 0;
  QString countError;
  if (!countLocalSelection(files, &totalFiles, &countError)) {
    showError(countError);
    return;
  }

  auto op = [&](QString &error) {
    std::optional<bool> fixRomDecision;
    for (const QString &file : files) {
      if (!uploadPathRecursive(file, currentPath_, &error, &fixRomDecision)) {
        return false;
      }
    }
    return true;
  };

  if (runWithProgress(tr("Uploading"), op, true, totalFiles)) {
    loadDirectory();
  }
}

void MainWindow::downloadSelection() {
  if (!ensureConnected()) {
    return;
  }
  QVector<RomfsEntry> entries = selectedEntries();
  if (entries.isEmpty()) {
    showInfo(tr("Select at least one file"));
    return;
  }
  QString targetDir = chooseDirectory(tr("Select destination"));
  if (targetDir.isEmpty()) {
    return;
  }

  downloadEntriesToDirectory(entries, targetDir);
}

void MainWindow::deleteSelection() {
  if (!ensureConnected()) {
    return;
  }
  QVector<RomfsEntry> entries = selectedEntries();
  if (entries.isEmpty()) {
    return;
  }
  if (!confirm(tr("Delete"),
               tr("Delete %1 selected entries?").arg(entries.size()))) {
    return;
  }

  QString error;
  for (const RomfsEntry &entry : entries) {
    if (!device_.removeEntry(entry.path, &error)) {
      showError(error);
      break;
    }
  }
  loadDirectory();
}

void MainWindow::createDirectory() {
  if (!ensureConnected()) {
    return;
  }
  QString name = requestText(tr("New Folder"), tr("Folder name:"));
  if (name.isEmpty()) {
    return;
  }
  QString error;
  if (!device_.makeDirectory(childPath(currentPath_, name), &error)) {
    showError(error);
    return;
  }
  loadDirectory();
}

void MainWindow::renameSelection() {
  if (!ensureConnected()) {
    return;
  }
  QVector<RomfsEntry> entries = selectedEntries();
  if (entries.size() != 1) {
    showInfo(tr("Select a single entry to rename"));
    return;
  }
  QString newName =
      requestText(tr("Rename"), tr("New name:"), entries.first().name);
  if (newName.isEmpty()) {
    return;
  }
  QString error;
  if (!device_.renameEntry(entries.first().path,
                           childPath(currentPath_, newName), false, &error)) {
    showError(error);
    return;
  }
  loadDirectory();
}

void MainWindow::formatRomfs() {
  if (!ensureConnected()) {
    return;
  }
  if (!confirm(tr("Format"),
               tr("This will erase the entire cartridge. Continue?"))) {
    return;
  }
  QString error;
  if (!device_.format(&error)) {
    showError(error);
    return;
  }
  loadDirectory();
}

void MainWindow::rebootCart() {
  if (!ensureConnected()) {
    return;
  }
  QString error;
  if (!device_.reboot(&error)) {
    showError(error);
  }
}

void MainWindow::enterBootloader() {
  if (!ensureConnected()) {
    return;
  }
  if (!confirm(tr("Bootloader"), tr("Switch to bootloader mode?"))) {
    return;
  }
  QString error;
  if (!device_.bootloader(&error)) {
    showError(error);
  }
}

void MainWindow::openSettingsDialog() {
  SettingsDialog dialog(this);
  dialog.setFixRomEnabled(settings_.fixRomEnabled);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  if (dialog.resetRequested()) {
    settings_.askFixRom = true;
  }

  settings_.fixRomEnabled = dialog.fixRomEnabled();
  saveSettings();
}

void MainWindow::openUsage() {
  const QString text = tr(
      "<b>ROMFS Manager</b> lets you manage the files stored on your N64cart."
      "<br/><br/><b>Main features:</b><ul>"
      "<li>Browse the cartridge contents as a list or icon grid</li>"
      "<li>Upload files to the cart and download them back to your "
      "computer</li>"
      "<li>Delete, rename, create folders, and format the ROMFS volume</li>"
      "<li>Drag & drop files between the app and your file manager</li>"
      "</ul>Use the File/Tools menus or the item context menu to perform "
      "actions.");
  QMessageBox::information(this, tr("How to Use"), text);
}

void MainWindow::openAbout() {
  const QString version = QCoreApplication::applicationVersion().isEmpty()
                              ? QStringLiteral("1.0")
                              : QCoreApplication::applicationVersion();
  const QString url = QStringLiteral("https://github.com/pdaxrom/N64cart");
  const QString text =
      tr("<center><b>ROMFS Manager</b></center><br/><center>Version "
         "%1</center><br/><center><a href=\"%2\">%2</a></center>")
          .arg(version, url);
  QMessageBox::about(this, tr("About ROMFS Manager"), text);
}

void MainWindow::handleDroppedUrls(const QList<QUrl> &urls) {
  if (!ensureConnected()) {
    return;
  }
  QStringList uploadFiles;
  for (const QUrl &url : urls) {
    if (url.isLocalFile()) {
      QFileInfo info(url.toLocalFile());
      if (info.isDir()) {
        continue;
      }
      uploadFiles << info.absoluteFilePath();
    }
  }
  if (uploadFiles.isEmpty()) {
    return;
  }

  const quint64 totalFiles = static_cast<quint64>(uploadFiles.size());

  auto op = [&](QString &error) {
    std::optional<bool> fixRomDecision;
    for (const QString &file : uploadFiles) {
      QFileInfo info(file);
      if (!uploadFileWithSettings(file,
                                  childPath(currentPath_, info.fileName()),
                                  &error, &fixRomDecision)) {
        return false;
      }
    }
    return true;
  };

  if (runWithProgress(tr("Uploading"), op, true, totalFiles)) {
    loadDirectory();
  }
}

void MainWindow::handleStartDrag(const QModelIndexList &indexes,
                                 Qt::DropActions actions) {
  Q_UNUSED(actions)
  QVector<RomfsEntry> entries;
  QSet<int> rows;
  for (const QModelIndex &index : indexes) {
    if (index.column() == 0 && index.isValid()) {
      rows.insert(index.row());
    }
  }
  for (int row : rows) {
    entries.append(model_->entryAt(row));
  }
  if (entries.isEmpty()) {
    return;
  }

  QString targetDir = chooseDirectory(tr("Select destination"));
  if (targetDir.isEmpty()) {
    return;
  }
  downloadEntriesToDirectory(entries, targetDir);
}

void MainWindow::handleOperationProgress(const QString &description,
                                         quint64 processed, quint64 total) {
  QString statusText = description;
  if (shouldShowFileCountdown()) {
    statusText += tr(" | %1/%2")
                      .arg(static_cast<qulonglong>(fileCountdownRemaining_))
                      .arg(static_cast<qulonglong>(fileCountdownTotal_));
  }
  if (copyProgressActive_) {
    qint64 elapsedMs =
        copyProgressTimer_.isValid() ? copyProgressTimer_.elapsed() : 0;
    if (elapsedMs < 0) {
      elapsedMs = 0;
    }
    statusText += tr(" | Total %1").arg(formatDuration(elapsedMs));
    QString etaText = tr("--:--");
    if (total > 0 && processed > 0 && elapsedMs > 0) {
      const quint64 remainingBytes =
          (processed < total) ? (total - processed) : 0;
      if (remainingBytes == 0) {
        etaText = formatDuration(0);
      } else {
        const long double bytesPerMs = static_cast<long double>(processed) /
                                       static_cast<long double>(elapsedMs);
        if (bytesPerMs > 0) {
          const qint64 remainingMs =
              static_cast<qint64>(remainingBytes / bytesPerMs);
          if (remainingMs >= 0) {
            etaText = formatDuration(remainingMs);
          }
        }
      }
    }
    statusText += tr(" | ETA %1").arg(etaText);
  }
  statusLabel_->setText(statusText);

  if (!progressBar_) {
    return;
  }
  if (!progressBar_->isVisible()) {
    progressBar_->setVisible(true);
  }

  if (total > 0) {
    const int percent =
        static_cast<int>(qMin<quint64>((processed * 100) / total, 100));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(percent);
  } else {
    progressBar_->setRange(0, 0);
  }

  QApplication::processEvents();
}

void MainWindow::setIconMode() {
  iconViewAction_->setChecked(true);
  listViewAction_->setChecked(false);
  ui_->viewStack->setCurrentWidget(iconView_);
  currentView_ = iconView_;
  iconView_->setIconSize(QSize(96, 96));
}

void MainWindow::setListMode() {
  listViewAction_->setChecked(true);
  iconViewAction_->setChecked(false);
  ui_->viewStack->setCurrentWidget(treeView_);
  currentView_ = treeView_;
  treeView_->setIconSize(QSize(24, 24));
  treeView_->setColumnHidden(1, false);
}

void MainWindow::updateActions() {
  const bool connected = device_.isConnected();
  const auto transport = device_.transportType();

  connectUsbAction_->setEnabled(transport == RomfsDevice::TransportType::None);
  connectRemoteAction_->setEnabled(transport ==
                                   RomfsDevice::TransportType::None);
  disconnectAction_->setEnabled(connected);
  refreshAction_->setEnabled(connected);
  uploadAction_->setEnabled(connected);
  downloadAction_->setEnabled(connected && !selectedEntries().isEmpty());
  deleteAction_->setEnabled(connected && !selectedEntries().isEmpty());
  newFolderAction_->setEnabled(connected);
  renameAction_->setEnabled(connected && selectedEntries().size() == 1);
  formatAction_->setEnabled(connected);
  rebootAction_->setEnabled(connected);
  bootloaderAction_->setEnabled(connected);
}

void MainWindow::updateStatus() {
  if (!device_.isConnected()) {
    statusLabel_->setText(tr("Disconnected"));
    return;
  }
  QStringList parts;
  const ack_header &info = device_.cartInfo();
  parts << tr("FW %1.%2").arg(info.info.vers >> 8).arg(info.info.vers & 0xff);
  quint64 freeBytes = device_.freeSpace();
  parts << tr("Free %1 bytes").arg(freeBytes);
  statusLabel_->setText(parts.join(QStringLiteral(" | ")));
}

bool MainWindow::ensureConnected() {
  if (!device_.isConnected()) {
    showInfo(tr("Please connect to a device first."));
    return false;
  }
  return true;
}

void MainWindow::changePath(const QString &path) {
  QString normalized = path.isEmpty() ? QStringLiteral("/") : path;
  if (!normalized.startsWith(QLatin1Char('/'))) {
    normalized.prepend(QLatin1Char('/'));
  }
  if (normalized.length() > 1 && normalized.endsWith(QLatin1Char('/'))) {
    normalized.chop(1);
  }
  currentPath_ = normalized;
  pathEdit_->setText(currentPath_);
  loadDirectory();
}

void MainWindow::loadDirectory() {
  if (!device_.isConnected()) {
    return;
  }
  QString error;
  QVector<RomfsEntry> entries = device_.list(currentPath_, &error);
  if (!error.isEmpty()) {
    showError(error);
    return;
  }
  model_->setEntries(entries);
  updateStatus();
}

QString MainWindow::childPath(const QString &base, const QString &name) const {
  if (base.isEmpty() || base == QStringLiteral("/")) {
    return QStringLiteral("/%1").arg(name);
  }
  if (base.endsWith(QLatin1Char('/'))) {
    return base + name;
  }
  return base + QLatin1Char('/') + name;
}

QVector<RomfsEntry> MainWindow::selectedEntries() const {
  QVector<RomfsEntry> entries;
  QSet<int> rows;
  if (!currentView_) {
    return entries;
  }
  const QModelIndexList selected =
      currentView_->selectionModel()->selectedRows();
  for (const QModelIndex &index : selected) {
    rows.insert(index.row());
  }
  for (int row : rows) {
    entries.append(model_->entryAt(row));
  }
  return entries;
}

QString MainWindow::chooseDirectory(const QString &title,
                                    const QString &dir) const {
  return QFileDialog::getExistingDirectory(const_cast<MainWindow *>(this),
                                           title, dir);
}

QString MainWindow::requestText(const QString &title, const QString &label,
                                const QString &text) const {
  bool ok = false;
  QString value = QInputDialog::getText(const_cast<MainWindow *>(this), title,
                                        label, QLineEdit::Normal, text, &ok);
  return ok ? value.trimmed() : QString();
}

bool MainWindow::confirm(const QString &title, const QString &text) const {
  return QMessageBox::question(const_cast<MainWindow *>(this), title, text) ==
         QMessageBox::Yes;
}

bool MainWindow::confirmOverwriteFile(const QString &path) const {
  QMessageBox box(const_cast<MainWindow *>(this));
  box.setWindowTitle(tr("Overwrite File?"));
  box.setText(tr("File %1 already exists. Overwrite it?").arg(path));
  box.setIcon(QMessageBox::Warning);
  QPushButton *yesButton = box.addButton(QMessageBox::Yes);
  QPushButton *cancelButton = box.addButton(QMessageBox::Cancel);
  box.setDefaultButton(yesButton);
  box.exec();
  return box.clickedButton() == yesButton;
}

void MainWindow::showError(const QString &message) {
  if (message.isEmpty()) {
    return;
  }
  QMessageBox::critical(this, tr("Error"), message);
}

void MainWindow::showInfo(const QString &message) {
  if (message.isEmpty()) {
    return;
  }
  QMessageBox::information(this, tr("Info"), message);
}

void MainWindow::setUiEnabled(bool enabled) {
  if (ui_->centralwidget) {
    ui_->centralwidget->setEnabled(enabled);
  }
  if (ui_->menubar) {
    ui_->menubar->setEnabled(enabled);
  }
  if (ui_->toolBar) {
    ui_->toolBar->setEnabled(enabled);
  }
}

bool MainWindow::runWithProgress(
    const QString &title, const std::function<bool(QString &)> &operation,
    bool showCopyProgress, quint64 totalFiles) {
  const QString previousStatus = statusLabel_->text();
  statusLabel_->setText(title);
  setUiEnabled(false);
  const bool previousCopyProgressState = copyProgressActive_;
  const bool previousCountdownActive = fileCountdownActive_;
  const quint64 previousCountdownTotal = fileCountdownTotal_;
  const quint64 previousCountdownRemaining = fileCountdownRemaining_;
  if (showCopyProgress) {
    copyProgressActive_ = true;
    copyProgressTimer_.start();
  }
  if (totalFiles > 0) {
    fileCountdownActive_ = true;
    fileCountdownTotal_ = totalFiles;
    fileCountdownRemaining_ = totalFiles;
  }

  if (progressBar_) {
    progressBar_->setVisible(true);
    progressBar_->setRange(0, 0);
    progressBar_->setValue(0);
  }

  QString error;
  bool success = operation(error);

  if (showCopyProgress) {
    copyProgressActive_ = previousCopyProgressState;
  }
  fileCountdownActive_ = previousCountdownActive;
  fileCountdownTotal_ = previousCountdownTotal;
  fileCountdownRemaining_ = previousCountdownRemaining;

  setUiEnabled(true);
  statusLabel_->setText(previousStatus);
  progressBar_->setVisible(false);

  if (!success && !error.isEmpty()) {
    showError(error);
  }

  return success;
}

QIcon MainWindow::getIcon(const QString &name) const {
  const bool isDark =
      QApplication::palette().color(QPalette::WindowText).lightness() > 128;
  const QString prefix = isDark ? QStringLiteral("light/") : QStringLiteral("");
  return QIcon(QStringLiteral(":/icons/%1%2.svg").arg(prefix, name));
}

bool MainWindow::uploadPathRecursive(const QString &localPath,
                                     const QString &remoteDir, QString *error,
                                     std::optional<bool> *fixRomDecision) {
  QFileInfo info(localPath);
  if (!info.exists()) {
    if (error) {
      *error = tr("Path does not exist: %1").arg(localPath);
    }
    return false;
  }

  if (info.isDir()) {
    const QString remotePath = childPath(remoteDir, info.fileName());
    if (!ensureRemoteDirectory(remotePath, error)) {
      return false;
    }

    QDir dir(localPath);
    const QFileInfoList children =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &child : children) {
      if (!uploadPathRecursive(child.absoluteFilePath(), remotePath, error,
                               fixRomDecision)) {
        return false;
      }
    }
    return true;
  }

  return uploadFileWithSettings(
      localPath, childPath(remoteDir, info.fileName()), error, fixRomDecision);
}

bool MainWindow::uploadFileWithSettings(const QString &localPath,
                                        const QString &remotePath,
                                        QString *error,
                                        std::optional<bool> *fixRomDecision) {
  bool fixRom = false;
  const bool isRom = isRomFile(localPath);
  if (isRom) {
    if (settings_.fixRomEnabled) {
      fixRom = true;
    } else if (!settings_.askFixRom) {
      fixRom = settings_.fixRomEnabled;
    } else {
      if (fixRomDecision && fixRomDecision->has_value()) {
        fixRom = fixRomDecision->value();
      } else {
        bool dontAskAgain = false;
        const FixRomPromptResult result =
            promptFixRomChoice(QFileInfo(localPath).fileName(), &dontAskAgain);
        if (result == FixRomPromptResult::Cancel) {
          if (error) {
            *error = tr("Upload canceled");
          }
          return false;
        }
        fixRom = (result == FixRomPromptResult::Fix);
        if (fixRomDecision) {
          *fixRomDecision = fixRom;
        }
        if (dontAskAgain) {
          settings_.askFixRom = false;
          settings_.fixRomEnabled = fixRom;
          saveSettings();
        }
      }
    }
  }

  bool ok = device_.uploadFile(localPath, remotePath, fixRom, -1, error);
  if (ok) {
    handleFileTransferCompleted();
  }
  return ok;
}

bool MainWindow::isRomFile(const QString &path) const {
  QFileInfo info(path);
  const QString suffix = info.suffix().toLower();
  return suffix == QLatin1String("z64") || suffix == QLatin1String("n64") ||
         suffix == QLatin1String("v64");
}

MainWindow::FixRomPromptResult
MainWindow::promptFixRomChoice(const QString &fileName, bool *dontAskAgain) {
  QMessageBox box(this);
  box.setWindowTitle(tr("Fix ROM byte order?"));
  box.setText(
      tr("File %1 appears to be a ROM. Convert to Z64 before uploading?")
          .arg(fileName));
  box.setIcon(QMessageBox::Question);
  QPushButton *fixButton =
      box.addButton(tr("Fix ROM"), QMessageBox::AcceptRole);
  QPushButton *skipButton =
      box.addButton(tr("Leave As-Is"), QMessageBox::DestructiveRole);
  Q_UNUSED(skipButton)
  QPushButton *cancelButton = box.addButton(QMessageBox::Cancel);
  auto *checkBox = new QCheckBox(tr("Don't ask again"), &box);
  box.setCheckBox(checkBox);
  box.exec();

  if (dontAskAgain) {
    *dontAskAgain = checkBox->isChecked();
  }

  if (box.clickedButton() == cancelButton) {
    return FixRomPromptResult::Cancel;
  }
  if (box.clickedButton() == fixButton) {
    return FixRomPromptResult::Fix;
  }
  return FixRomPromptResult::Skip;
}

void MainWindow::loadSettings() {
  QSettings settings;
  settings_.fixRomEnabled =
      settings.value(QStringLiteral("fixRomEnabled"), settings_.fixRomEnabled)
          .toBool();
  settings_.askFixRom =
      settings.value(QStringLiteral("askFixRom"), settings_.askFixRom).toBool();
  settings_.lastRemoteAddress = settings
                                    .value(QStringLiteral("lastRemoteAddress"),
                                           settings_.lastRemoteAddress)
                                    .toString();
}

void MainWindow::saveSettings() const {
  QSettings settings;
  settings.setValue(QStringLiteral("fixRomEnabled"), settings_.fixRomEnabled);
  settings.setValue(QStringLiteral("askFixRom"), settings_.askFixRom);
  settings.setValue(QStringLiteral("lastRemoteAddress"),
                    settings_.lastRemoteAddress);
}

QString MainWindow::formatDuration(qint64 milliseconds) const {
  if (milliseconds < 0) {
    milliseconds = 0;
  }

  qint64 totalSeconds = milliseconds / 1000;
  qint64 hours = totalSeconds / 3600;
  qint64 minutes = (totalSeconds % 3600) / 60;
  qint64 seconds = totalSeconds % 60;

  if (hours > 0) {
    return QStringLiteral("%1:%2:%3")
        .arg(hours)
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
  }
  return QStringLiteral("%1:%2")
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(seconds, 2, 10, QLatin1Char('0'));
}

bool MainWindow::countLocalSelection(const QStringList &paths,
                                     quint64 *totalFiles,
                                     QString *error) const {
  if (!totalFiles) {
    return true;
  }
  quint64 total = 0;
  for (const QString &path : paths) {
    if (!countLocalFilesRecursive(path, &total, error)) {
      return false;
    }
  }
  *totalFiles = total;
  return true;
}

bool MainWindow::countLocalFilesRecursive(const QString &path,
                                          quint64 *totalFiles,
                                          QString *error) const {
  if (!totalFiles) {
    return true;
  }
  QFileInfo info(path);
  if (!info.exists()) {
    if (error) {
      *error = tr("Path does not exist: %1").arg(path);
    }
    return false;
  }
  if (info.isDir()) {
    QDirIterator it(path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
      it.next();
      if (it.fileInfo().isDir()) {
        continue;
      }
      ++(*totalFiles);
    }
  } else {
    ++(*totalFiles);
  }
  return true;
}

bool MainWindow::countRemoteEntries(const QVector<RomfsEntry> &entries,
                                    quint64 *totalFiles, QString *error) {
  if (!totalFiles) {
    return true;
  }
  quint64 total = 0;
  for (const RomfsEntry &entry : entries) {
    if (!countRemoteEntryRecursive(entry, &total, error)) {
      return false;
    }
  }
  *totalFiles = total;
  return true;
}

bool MainWindow::countRemoteEntryRecursive(const RomfsEntry &entry,
                                           quint64 *totalFiles,
                                           QString *error) {
  if (!totalFiles) {
    return true;
  }
  if (entry.isDirectory) {
    QString listError;
    QVector<RomfsEntry> children = device_.list(entry.path, &listError);
    if (!listError.isEmpty()) {
      if (error) {
        *error = listError;
      }
      return false;
    }
    for (const RomfsEntry &child : children) {
      if (!countRemoteEntryRecursive(child, totalFiles, error)) {
        return false;
      }
    }
    return true;
  }
  ++(*totalFiles);
  return true;
}

void MainWindow::beginFileCountdown(quint64 totalFiles) {
  fileCountdownActive_ = true;
  fileCountdownTotal_ = totalFiles;
  fileCountdownRemaining_ = totalFiles;
}

void MainWindow::finishFileCountdown() {
  fileCountdownActive_ = false;
  fileCountdownTotal_ = 0;
  fileCountdownRemaining_ = 0;
}

void MainWindow::handleFileTransferCompleted() {
  if (fileCountdownActive_ && fileCountdownRemaining_ > 0) {
    --fileCountdownRemaining_;
  }
  if (fileCountdownActive_) {
    loadDirectory();
  }
}

bool MainWindow::shouldShowFileCountdown() const {
  return fileCountdownActive_ && fileCountdownTotal_ > 1;
}

bool MainWindow::ensureRemoteDirectory(const QString &remotePath,
                                       QString *error) {
  QString tempError;
  if (device_.makeDirectory(remotePath, &tempError)) {
    return true;
  }

  QString checkError;
  device_.list(remotePath, &checkError);
  if (!checkError.isEmpty()) {
    if (error) {
      *error = tempError.isEmpty() ? checkError : tempError;
    }
    return false;
  }
  return true;
}

bool MainWindow::downloadEntryRecursive(const RomfsEntry &entry,
                                        const QString &targetDir,
                                        QString *error) {
  QDir dir(targetDir);
  if (entry.isDirectory) {
    const QString localDir = dir.filePath(entry.name);
    if (!QDir().mkpath(localDir)) {
      if (error) {
        *error = tr("Cannot create directory %1").arg(localDir);
      }
      return false;
    }

    QString listError;
    QVector<RomfsEntry> children = device_.list(entry.path, &listError);
    if (!listError.isEmpty()) {
      if (error) {
        *error = listError;
      }
      return false;
    }

    for (const RomfsEntry &child : children) {
      if (!downloadEntryRecursive(child, localDir, error)) {
        return false;
      }
    }
    return true;
  }

  const QString localFile = dir.filePath(entry.name);
  if (QFileInfo::exists(localFile)) {
    if (!confirmOverwriteFile(localFile)) {
      if (error) {
        *error = tr("Download canceled");
      }
      return false;
    }
    if (!QFile::remove(localFile)) {
      if (error) {
        *error = tr("Cannot remove existing file %1").arg(localFile);
      }
      return false;
    }
  }
  bool ok = device_.downloadFile(entry.path, localFile, error);
  if (ok) {
    handleFileTransferCompleted();
  }
  return ok;
}

bool MainWindow::downloadEntriesToDirectory(const QVector<RomfsEntry> &entries,
                                            const QString &targetDir) {
  quint64 totalFiles = 0;
  QString countError;
  if (!countRemoteEntries(entries, &totalFiles, &countError)) {
    if (!countError.isEmpty()) {
      showError(countError);
    }
    return false;
  }

  auto op = [&](QString &error) {
    for (const RomfsEntry &entry : entries) {
      if (!downloadEntryRecursive(entry, targetDir, &error)) {
        return false;
      }
    }
    return true;
  };
  return runWithProgress(tr("Downloading"), op, true, totalFiles);
}
