#include "romfsmodel.h"

#include <QApplication>
#include <QIcon>
#include <QStyle>

RomfsModel::RomfsModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

QModelIndex RomfsModel::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid() || column < 0 || column >= columnCount(QModelIndex())) {
        return QModelIndex();
    }
    if (row < 0 || row >= entries_.size()) {
        return QModelIndex();
    }
    return createIndex(row, column);
}

QModelIndex RomfsModel::parent(const QModelIndex &child) const
{
    Q_UNUSED(child)
    return QModelIndex();
}

int RomfsModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return entries_.size();
}

int RomfsModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return 2;
}

QVariant RomfsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= entries_.size()) {
        return QVariant();
    }
    const RomfsEntry &entry = entries_.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        if (index.column() == 0) {
            return entry.name;
        }
        if (entry.isDirectory) {
            return QStringLiteral("-");
        }
        return QString::number(entry.size);
    case Qt::DecorationRole:
        if (index.column() == 0) {
            QIcon icon = entry.isDirectory ? QIcon::fromTheme(QStringLiteral("folder")) : QIcon::fromTheme(QStringLiteral("text-x-generic"));
            if (icon.isNull() && qApp) {
                icon = qApp->style()->standardIcon(entry.isDirectory ? QStyle::SP_DirIcon : QStyle::SP_FileIcon);
            }
            return icon;
        }
        return QVariant();
    case Qt::UserRole:
        return entry.path;
    case Qt::UserRole + 1:
        return entry.isDirectory;
    case Qt::TextAlignmentRole:
        if (index.column() == 1) {
            return QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
        }
        break;
    default:
        return QVariant();
    }
    return QVariant();
}

QVariant RomfsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractItemModel::headerData(section, orientation, role);
    }

    if (section == 0) {
        return tr("Name");
    }
    if (section == 1) {
        return tr("Size");
    }
    return QVariant();
}

Qt::ItemFlags RomfsModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags base = QAbstractItemModel::flags(index);
    if (!index.isValid()) {
        return base;
    }
    base |= Qt::ItemIsDragEnabled | Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    return base;
}

void RomfsModel::setEntries(const QVector<RomfsEntry> &entries)
{
    beginResetModel();
    entries_ = entries;
    endResetModel();
}

RomfsEntry RomfsModel::entryAt(int row) const
{
    if (row < 0 || row >= entries_.size()) {
        return RomfsEntry{};
    }
    return entries_.at(row);
}
