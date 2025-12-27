#include "romfsiconview.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QMimeData>

RomfsIconView::RomfsIconView(QWidget *parent)
    : QListView(parent)
{
    setAcceptDrops(true);
    setDragEnabled(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
}

void RomfsIconView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        QListView::dragEnterEvent(event);
    }
}

void RomfsIconView::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        QListView::dragMoveEvent(event);
    }
}

void RomfsIconView::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        emit urlsDropped(event->mimeData()->urls());
        event->acceptProposedAction();
        return;
    }
    QListView::dropEvent(event);
}

void RomfsIconView::startDrag(Qt::DropActions supportedActions)
{
    emit startDragRequested(selectedIndexes(), supportedActions);
}

