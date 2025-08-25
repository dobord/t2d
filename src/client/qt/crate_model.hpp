// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "game.pb.h"

#include <mutex>
#include <vector>

#include <QtCore/QAbstractListModel>

struct QtCrateRow
{
    uint32_t id{};
    float x{};
    float y{};
    float angle{}; // degrees
};

class CrateModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles
    {
        IdRole = Qt::UserRole + 1,
        XRole,
        YRole,
        AngleRole
    };

    explicit CrateModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

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
        m.insert("crateId", QVariant::fromValue((uint)r.id));
        m.insert("x", r.x);
        m.insert("y", r.y);
        m.insert("angle", r.angle);
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
            case AngleRole:
                return r.angle;
        }
        return {};
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return {{IdRole, "crateId"}, {XRole, "x"}, {YRole, "y"}, {AngleRole, "angle"}};
    }

    void applyFull(const t2d::StateSnapshot &snap)
    {
        std::vector<QtCrateRow> newRows;
        newRows.reserve(snap.crates_size());
        for (auto &c : snap.crates()) {
            newRows.push_back({c.crate_id(), c.x(), c.y(), c.angle()});
        }
        std::scoped_lock lk(m_);
        beginResetModel();
        rows_.swap(newRows);
        endResetModel();
    }

    void applyDelta(const t2d::DeltaSnapshot &d)
    {
        std::scoped_lock lk(m_);
        // removals
        for (auto rid : d.removed_crates()) {
            for (size_t i = 0; i < rows_.size(); ++i) {
                if (rows_[i].id == rid) {
                    beginRemoveRows({}, (int)i, (int)i);
                    rows_.erase(rows_.begin() + i);
                    endRemoveRows();
                    break;
                }
            }
        }
        // updates / additions
        for (auto &c : d.crates()) {
            bool found = false;
            for (size_t i = 0; i < rows_.size(); ++i) {
                if (rows_[i].id == c.crate_id()) {
                    rows_[i].x = c.x();
                    rows_[i].y = c.y();
                    rows_[i].angle = c.angle();
                    auto ix = index((int)i);
                    emit dataChanged(ix, ix);
                    found = true;
                    break;
                }
            }
            if (!found) {
                beginInsertRows({}, (int)rows_.size(), (int)rows_.size());
                rows_.push_back({c.crate_id(), c.x(), c.y(), c.angle()});
                endInsertRows();
            }
        }
    }

private:
    mutable std::mutex m_;
    std::vector<QtCrateRow> rows_;
};
