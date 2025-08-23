// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "game.pb.h"

#include <mutex>
#include <unordered_map>
#include <vector>

#include <QtCore/QAbstractListModel>

struct QtTankRow
{
    uint32_t id{};
    float x{};
    float y{};
    float prev_x{};
    float prev_y{};
    float hull_angle{}; // degrees
    float turret_angle{}; // degrees
    float prev_hull_angle{};
    float prev_turret_angle{};
    float hp{};
    float ammo{};
};

class EntityModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles
    {
        IdRole = Qt::UserRole + 1,
        XRole,
        YRole,
        PrevXRole,
        PrevYRole,
        HullAngleRole,
        TurretAngleRole,
        HPRole,
        AmmoRole
    };

    EntityModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    Q_INVOKABLE int count() const { return static_cast<int>(rows_.size()); }

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

    Q_INVOKABLE float interpHullAngle(int row, float alpha) const
    {
        std::scoped_lock lk(m_);
        if (row < 0 || (size_t)row >= rows_.size())
            return 0.f;
        const auto &r = rows_[row];
        float a0 = r.prev_hull_angle;
        float a1 = r.hull_angle;
        // Shortest angular interpolation (degrees)
        float diff = std::fmod(a1 - a0 + 540.f, 360.f) - 180.f;
        return a0 + diff * alpha;
    }

    Q_INVOKABLE float interpTurretAngle(int row, float alpha) const
    {
        std::scoped_lock lk(m_);
        if (row < 0 || (size_t)row >= rows_.size())
            return 0.f;
        const auto &r = rows_[row];
        float a0 = r.prev_turret_angle;
        float a1 = r.turret_angle;
        float diff = std::fmod(a1 - a0 + 540.f, 360.f) - 180.f;
        return a0 + diff * alpha;
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return static_cast<int>(rows_.size());
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid())
            return {};
        std::scoped_lock lk(m_);
        if (index.row() < 0 || static_cast<size_t>(index.row()) >= rows_.size())
            return {};
        const auto &r = rows_[index.row()];
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
            case HullAngleRole:
                return r.hull_angle;
            case TurretAngleRole:
                return r.turret_angle;
            case HPRole:
                return r.hp;
            case AmmoRole:
                return r.ammo;
        }
        return {};
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return {
            {IdRole, "entityId"},
            {XRole, "x"},
            {YRole, "y"},
            {PrevXRole, "prevX"},
            {PrevYRole, "prevY"},
            {HullAngleRole, "hullAngle"},
            {TurretAngleRole, "turretAngle"},
            {HPRole, "hp"},
            {AmmoRole, "ammo"}};
    }

    void applyFull(const t2d::StateSnapshot &snap)
    {
        std::vector<QtTankRow> newRows;
        newRows.reserve(snap.tanks_size());
        for (const auto &t : snap.tanks()) {
            newRows.push_back(QtTankRow{
                t.entity_id(),
                t.x(),
                t.y(),
                t.x(),
                t.y(),
                t.hull_angle(),
                t.turret_angle(),
                t.hull_angle(),
                t.turret_angle(),
                (float)t.hp(),
                (float)t.ammo()});
        }
        {
            std::scoped_lock lk(m_);
            beginResetModel();
            rows_.swap(newRows);
            endResetModel();
        }
    }

    void applyDelta(const t2d::DeltaSnapshot &d)
    {
        std::scoped_lock lk(m_);
        bool anyChange = false;
        for (auto rid : d.removed_tanks()) {
            for (size_t i = 0; i < rows_.size(); ++i)
                if (rows_[i].id == rid) {
                    beginRemoveRows({}, (int)i, (int)i);
                    rows_.erase(rows_.begin() + i);
                    endRemoveRows();
                    anyChange = true;
                    break;
                }
        }
        for (const auto &t : d.tanks()) {
            bool found = false;
            for (size_t i = 0; i < rows_.size(); ++i)
                if (rows_[i].id == t.entity_id()) {
                    rows_[i].prev_x = rows_[i].x;
                    rows_[i].prev_y = rows_[i].y;
                    rows_[i].x = t.x();
                    rows_[i].y = t.y();
                    rows_[i].prev_hull_angle = rows_[i].hull_angle;
                    rows_[i].prev_turret_angle = rows_[i].turret_angle;
                    rows_[i].hull_angle = t.hull_angle();
                    rows_[i].turret_angle = t.turret_angle();
                    rows_[i].hp = (float)t.hp();
                    rows_[i].ammo = (float)t.ammo();
                    auto top = index((int)i);
                    emit dataChanged(top, top);
                    found = true;
                    break;
                }
            if (!found) {
                beginInsertRows({}, (int)rows_.size(), (int)rows_.size());
                rows_.push_back(QtTankRow{
                    t.entity_id(),
                    t.x(),
                    t.y(),
                    t.x(),
                    t.y(),
                    t.hull_angle(),
                    t.turret_angle(),
                    t.hull_angle(),
                    t.turret_angle(),
                    (float)t.hp(),
                    (float)t.ammo()});
                endInsertRows();
            }
        }
        (void)anyChange;
    }

private:
    mutable std::mutex m_;
    std::vector<QtTankRow> rows_;
};
