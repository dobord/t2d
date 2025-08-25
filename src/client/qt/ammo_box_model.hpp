// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "game.pb.h"

#include <mutex>
#include <vector>

#include <QtCore/QAbstractListModel>

struct QtAmmoBoxRow
{
    uint32_t id{};
    float x{};
    float y{};
    bool active{};
};

class AmmoBoxModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles
    {
        IdRole = Qt::UserRole + 1,
        XRole,
        YRole,
        ActiveRole
    };

    explicit AmmoBoxModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return (int)rows_.size();
    }

    Q_INVOKABLE int count() const { return rowCount(); }

    Q_INVOKABLE QVariant get(int row) const
    {
        std::scoped_lock lk(m_);
        if (row < 0 || (size_t)row >= rows_.size())
            return {};
        const auto &r = rows_[row];
        QVariantMap m;
        m.insert("boxId", QVariant::fromValue((uint)r.id));
        m.insert("x", r.x);
        m.insert("y", r.y);
        m.insert("active", r.active);
        return m;
    }

    QVariant data(const QModelIndex &idx, int role) const override
    {
        if (!idx.isValid())
            return {};
        std::scoped_lock lk(m_);
        if (idx.row() < 0 || (size_t)idx.row() >= rows_.size())
            return {};
        const auto &r = rows_[idx.row()];
        switch (role) {
            case IdRole:
                return QVariant::fromValue((uint)r.id);
            case XRole:
                return r.x;
            case YRole:
                return r.y;
            case ActiveRole:
                return r.active;
        }
        return {};
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return {{IdRole, "boxId"}, {XRole, "x"}, {YRole, "y"}, {ActiveRole, "active"}};
    }

    void applyFull(const t2d::StateSnapshot &snap)
    {
        std::vector<QtAmmoBoxRow> newRows;
        newRows.reserve(snap.ammo_boxes_size());
        for (auto &b : snap.ammo_boxes()) {
            newRows.push_back({b.box_id(), b.x(), b.y(), b.active()});
        }
        std::scoped_lock lk(m_);
        beginResetModel();
        rows_.swap(newRows);
        endResetModel();
    }

private:
    mutable std::mutex m_;
    std::vector<QtAmmoBoxRow> rows_;
};
