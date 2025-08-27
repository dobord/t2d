// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "game.pb.h"

#include <mutex>
#include <unordered_map>
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
        index_.clear();
        index_.reserve(rows_.size());
        for (int i = 0; i < (int)rows_.size(); ++i)
            index_.emplace(rows_[i].id, i);
        endResetModel();
    }

    void applyDelta(const t2d::DeltaSnapshot &d)
    {
        std::scoped_lock lk(m_);
        std::vector<int> removeIdx;
        removeIdx.reserve(d.removed_projectiles_size());
        for (auto rid : d.removed_projectiles()) {
            auto it = index_.find(rid);
            if (it != index_.end())
                removeIdx.push_back(it->second);
        }
        if (!removeIdx.empty()) {
            std::sort(removeIdx.begin(), removeIdx.end());
            removeIdx.erase(std::unique(removeIdx.begin(), removeIdx.end()), removeIdx.end());
            for (auto it = removeIdx.rbegin(); it != removeIdx.rend(); ++it) {
                int r = *it;
                beginRemoveRows({}, r, r);
                rows_.erase(rows_.begin() + r);
                endRemoveRows();
            }
            index_.clear();
            index_.reserve(rows_.size());
            for (int i = 0; i < (int)rows_.size(); ++i)
                index_.emplace(rows_[i].id, i);
        }
        for (const auto &p : d.projectiles()) {
            auto it = index_.find(p.projectile_id());
            if (it != index_.end()) {
                int i = it->second;
                auto &row = rows_[i];
                row.prev_x = row.x;
                row.prev_y = row.y;
                row.x = p.x();
                row.y = p.y();
                auto ix = index(i);
                emit dataChanged(ix, ix);
            } else {
                beginInsertRows({}, (int)rows_.size(), (int)rows_.size());
                rows_.push_back(QtProjectileRow{p.projectile_id(), p.x(), p.y(), p.x(), p.y()});
                endInsertRows();
                index_.emplace(p.projectile_id(), (int)rows_.size() - 1);
            }
        }
    }

private:
    mutable std::mutex m_;
    std::vector<QtProjectileRow> rows_;
    std::unordered_map<uint32_t, int> index_;
};
