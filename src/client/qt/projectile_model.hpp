// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "game.pb.h"

#include <mutex>
#include <vector>

#include <QtCore/QAbstractListModel>

struct QtProjectileRow
{
    uint32_t id{};
    float x{};
    float y{};
    float prev_x{};
    float prev_y{};
};

class ProjectileModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles
    {
        IdRole = Qt::UserRole + 1,
        XRole,
        YRole,
        PrevXRole,
        PrevYRole
    };

    ProjectileModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    Q_INVOKABLE int count() const { return (int)rows_.size(); }

    Q_INVOKABLE float interpX(int row, float alpha) const
    {
        std::scoped_lock lk(m_);
        if (row < 0 || (size_t)row >= rows_.size())
            return 0.f;
        const auto &r = rows_[row];
        return r.prev_x + (r.x - r.prev_x) * alpha;
    }

    Q_INVOKABLE float interpY(int row, float alpha) const
    {
        std::scoped_lock lk(m_);
        if (row < 0 || (size_t)row >= rows_.size())
            return 0.f;
        const auto &r = rows_[row];
        return r.prev_y + (r.y - r.prev_y) * alpha;
    }

    // Interpolated velocity components (simple difference). Alpha currently unused but kept for API symmetry.
    // Returned values are frame-to-frame deltas; magnitude scaling is not needed for orientation when drawing.
    Q_INVOKABLE float interpVx(int row, float /*alpha*/) const
    {
        std::scoped_lock lk(m_);
        if (row < 0 || (size_t)row >= rows_.size())
            return 1.f; // default direction to avoid zero vector ambiguity
        const auto &r = rows_[row];
        return (r.x - r.prev_x);
    }

    Q_INVOKABLE float interpVy(int row, float /*alpha*/) const
    {
        std::scoped_lock lk(m_);
        if (row < 0 || (size_t)row >= rows_.size())
            return 0.f;
        const auto &r = rows_[row];
        return (r.y - r.prev_y);
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return (int)rows_.size();
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
            case PrevXRole:
                return r.prev_x;
            case PrevYRole:
                return r.prev_y;
        }
        return {};
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return {{IdRole, "projId"}, {XRole, "x"}, {YRole, "y"}, {PrevXRole, "prevX"}, {PrevYRole, "prevY"}};
    }

    void applyFull(const t2d::StateSnapshot &snap)
    {
        std::vector<QtProjectileRow> newRows;
        newRows.reserve(snap.projectiles_size());
        for (const auto &p : snap.projectiles())
            newRows.push_back(QtProjectileRow{p.projectile_id(), p.x(), p.y(), p.x(), p.y()});
        std::scoped_lock lk(m_);
        beginResetModel();
        rows_.swap(newRows);
        endResetModel();
    }

    void applyDelta(const t2d::DeltaSnapshot &d)
    {
        std::scoped_lock lk(m_);
        for (auto rid : d.removed_projectiles()) {
            for (size_t i = 0; i < rows_.size(); ++i)
                if (rows_[i].id == rid) {
                    beginRemoveRows({}, (int)i, (int)i);
                    rows_.erase(rows_.begin() + i);
                    endRemoveRows();
                    break;
                }
        }
        for (const auto &p : d.projectiles()) {
            bool found = false;
            for (size_t i = 0; i < rows_.size(); ++i)
                if (rows_[i].id == p.projectile_id()) {
                    rows_[i].prev_x = rows_[i].x;
                    rows_[i].prev_y = rows_[i].y;
                    rows_[i].x = p.x();
                    rows_[i].y = p.y();
                    auto ix = index((int)i);
                    emit dataChanged(ix, ix);
                    found = true;
                    break;
                }
            if (!found) {
                beginInsertRows({}, (int)rows_.size(), (int)rows_.size());
                rows_.push_back(QtProjectileRow{p.projectile_id(), p.x(), p.y(), p.x(), p.y()});
                endInsertRows();
            }
        }
    }

private:
    mutable std::mutex m_;
    std::vector<QtProjectileRow> rows_;
};
