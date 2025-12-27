#include "romfsview.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QStyle>

RomfsView::RomfsView(QWidget *parent)
    : QTreeView(parent)
{
    setAcceptDrops(true);
    setDragEnabled(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
}

void RomfsView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        QTreeView::dragEnterEvent(event);
    }
}

void RomfsView::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        QTreeView::dragMoveEvent(event);
    }
}

void RomfsView::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        emit urlsDropped(event->mimeData()->urls());
        event->acceptProposedAction();
        return;
    }
    QTreeView::dropEvent(event);
}

void RomfsView::startDrag(Qt::DropActions supportedActions)
{
    emit startDragRequested(selectedIndexes(), supportedActions);
}

void RomfsView::startExternalDrag(const QList<QUrl> &urls, QMimeData *customMime)
{
    if (urls.isEmpty() && !customMime) {
        return;
    }
    auto *drag = new QDrag(this);
    if (customMime) {
        drag->setMimeData(customMime);
    } else {
        auto *mimeData = new QMimeData;
        mimeData->setUrls(urls);
        drag->setMimeData(mimeData);
    }
    drag->setPixmap(style()->standardIcon(QStyle::SP_FileIcon).pixmap(48, 48));
    drag->exec(Qt::CopyAction);
}
