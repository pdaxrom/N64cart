#pragma once

#include <QAbstractItemModel>
#include <QVector>

#include "romfsdevice.h"

class RomfsModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit RomfsModel(QObject *parent = nullptr);

    QModelIndex index(int row, int column, const QModelIndex &parent) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent) const override;
    int columnCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void setEntries(const QVector<RomfsEntry> &entries);
    RomfsEntry entryAt(int row) const;

private:
    QVector<RomfsEntry> entries_;
};
