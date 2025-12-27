#pragma once

#include <QTreeView>
#include <QUrl>

class QMimeData;

class RomfsView : public QTreeView
{
    Q_OBJECT

public:
    explicit RomfsView(QWidget *parent = nullptr);

    void startExternalDrag(const QList<QUrl> &urls, QMimeData *customMime = nullptr);

signals:
    void urlsDropped(const QList<QUrl> &urls);
    void startDragRequested(const QModelIndexList &indexes, Qt::DropActions supportedActions);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;
};
