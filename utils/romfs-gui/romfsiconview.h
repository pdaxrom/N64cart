#pragma once

#include <QListView>

class RomfsIconView : public QListView
{
    Q_OBJECT

public:
    explicit RomfsIconView(QWidget *parent = nullptr);

signals:
    void urlsDropped(const QList<QUrl> &urls);
    void startDragRequested(const QModelIndexList &indexes, Qt::DropActions supportedActions);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;
};

