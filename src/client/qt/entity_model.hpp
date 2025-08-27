// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "game.pb.h"

#include <cmath>
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
    // Precomputed unit vectors for hull & turret (current and previous) to enable slerp without per-frame trig.
    float hull_dir_x{1.f};
    float hull_dir_y{0.f};
    float prev_hull_dir_x{1.f};
    float prev_hull_dir_y{0.f};
    float turret_dir_x{1.f};
    float turret_dir_y{0.f};
    float prev_turret_dir_x{1.f};
    float prev_turret_dir_y{0.f};
};

class EntityModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(float mapWidth READ mapWidth NOTIFY mapDimensionsChanged)
    Q_PROPERTY(float mapHeight READ mapHeight NOTIFY mapDimensionsChanged)

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

    explicit EntityModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    float mapWidth() const { return map_width_; }

    float mapHeight() const { return map_height_; }

    Q_INVOKABLE int count() const { return static_cast<int>(rows_.size()); }

    Q_INVOKABLE uint32_t entityId(int row) const
    {
        std::scoped_lock lk(m_);
        if (row < 0 || (size_t)row >= rows_.size())
            return 0;
        return rows_[row].id;
    }

    Q_INVOKABLE int rowForEntity(uint32_t id) const
    {
        std::scoped_lock lk(m_);
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].id == id)
                return (int)i;
        }
        return -1;
    }

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
        return slerpAngleDeg(r.prev_hull_dir_x, r.prev_hull_dir_y, r.hull_dir_x, r.hull_dir_y, alpha);
    }

    Q_INVOKABLE float interpTurretAngle(int row, float alpha) const
    {
        std::scoped_lock lk(m_);
        if (row < 0 || (size_t)row >= rows_.size())
            return 0.f;
        const auto &r = rows_[row];
        return slerpAngleDeg(r.prev_turret_dir_x, r.prev_turret_dir_y, r.turret_dir_x, r.turret_dir_y, alpha);
    }

    // Radian versions to reduce per-frame JS math in QML (deg->rad conversion moved to C++):
    Q_INVOKABLE float interpHullAngleRad(int row, float alpha) const
    {
        std::scoped_lock lk(m_);
        if (row < 0 || (size_t)row >= rows_.size())
            return 0.f;
        const auto &r = rows_[row];
        return slerpAngleRad(r.prev_hull_dir_x, r.prev_hull_dir_y, r.hull_dir_x, r.hull_dir_y, alpha);
    }

    Q_INVOKABLE float interpTurretAngleRad(int row, float alpha) const
    {
        std::scoped_lock lk(m_);
        if (row < 0 || (size_t)row >= rows_.size())
            return 0.f;
        const auto &r = rows_[row];
        return slerpAngleRad(r.prev_turret_dir_x, r.prev_turret_dir_y, r.turret_dir_x, r.turret_dir_y, alpha);
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
        constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
        for (const auto &t : snap.tanks()) {
            float ha = t.hull_angle();
            float ta = t.turret_angle();
            float hrad = ha * kDegToRad;
            float trad = ta * kDegToRad;
            QtTankRow row;
            row.id = t.entity_id();
            row.x = t.x();
            row.y = t.y();
            row.prev_x = t.x();
            row.prev_y = t.y();
            row.hull_angle = ha;
            row.turret_angle = ta;
            row.prev_hull_angle = ha;
            row.prev_turret_angle = ta;
            row.hp = (float)t.hp();
            row.ammo = (float)t.ammo();
            row.hull_dir_x = std::cos(hrad);
            row.hull_dir_y = std::sin(hrad);
            row.prev_hull_dir_x = row.hull_dir_x;
            row.prev_hull_dir_y = row.hull_dir_y;
            row.turret_dir_x = std::cos(trad);
            row.turret_dir_y = std::sin(trad);
            row.prev_turret_dir_x = row.turret_dir_x;
            row.prev_turret_dir_y = row.turret_dir_y;
            newRows.push_back(row);
        }
        bool dimsChanged = false;
        // Proto3 scalars default to 0; treat >0 as provided.
        float w = snap.map_width();
        float h = snap.map_height();
        if (w > 0.f && h > 0.f && (w != map_width_ || h != map_height_)) {
            map_width_ = w;
            map_height_ = h;
            dimsChanged = true;
        }
        {
            std::scoped_lock lk(m_);
            beginResetModel();
            rows_.swap(newRows);
            // Rebuild persistent index cache.
            index_.clear();
            index_.reserve(rows_.size());
            for (int i = 0; i < (int)rows_.size(); ++i)
                index_.emplace(rows_[i].id, i);
            endResetModel();
        }
        if (dimsChanged)
            emit mapDimensionsChanged();
    }

    void applyDelta(const t2d::DeltaSnapshot &d)
    {
        std::scoped_lock lk(m_);
        // Removals: collect indices via persistent index_.
        std::vector<int> removeIdx;
        removeIdx.reserve(d.removed_tanks_size());
        for (auto rid : d.removed_tanks()) {
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
            // Rebuild index_ after structural removals.
            index_.clear();
            index_.reserve(rows_.size());
            for (int i = 0; i < (int)rows_.size(); ++i)
                index_.emplace(rows_[i].id, i);
        }
        // Updates / additions.
        for (const auto &t : d.tanks()) {
            auto it = index_.find(t.entity_id());
            if (it != index_.end()) {
                int i = it->second;
                auto &row = rows_[i];
                row.prev_x = row.x;
                row.prev_y = row.y;
                row.x = t.x();
                row.y = t.y();
                // Copy previous vector & angle state
                row.prev_hull_angle = row.hull_angle;
                row.prev_turret_angle = row.turret_angle;
                row.prev_hull_dir_x = row.hull_dir_x;
                row.prev_hull_dir_y = row.hull_dir_y;
                row.prev_turret_dir_x = row.turret_dir_x;
                row.prev_turret_dir_y = row.turret_dir_y;
                // Update new angles
                row.hull_angle = t.hull_angle();
                row.turret_angle = t.turret_angle();
                constexpr float kDegToRad2 = 3.14159265358979323846f / 180.f;
                float hrad = row.hull_angle * kDegToRad2;
                float trad = row.turret_angle * kDegToRad2;
                row.hull_dir_x = std::cos(hrad);
                row.hull_dir_y = std::sin(hrad);
                row.turret_dir_x = std::cos(trad);
                row.turret_dir_y = std::sin(trad);
                row.hp = (float)t.hp();
                row.ammo = (float)t.ammo();
                auto ix = index(i);
                emit dataChanged(ix, ix);
            } else {
                beginInsertRows({}, (int)rows_.size(), (int)rows_.size());
                constexpr float kDegToRad3 = 3.14159265358979323846f / 180.f;
                float ha = t.hull_angle();
                float ta = t.turret_angle();
                float hrad = ha * kDegToRad3;
                float trad = ta * kDegToRad3;
                QtTankRow row;
                row.id = t.entity_id();
                row.x = t.x();
                row.y = t.y();
                row.prev_x = t.x();
                row.prev_y = t.y();
                row.hull_angle = ha;
                row.turret_angle = ta;
                row.prev_hull_angle = ha;
                row.prev_turret_angle = ta;
                row.hp = (float)t.hp();
                row.ammo = (float)t.ammo();
                row.hull_dir_x = std::cos(hrad);
                row.hull_dir_y = std::sin(hrad);
                row.prev_hull_dir_x = row.hull_dir_x;
                row.prev_hull_dir_y = row.hull_dir_y;
                row.turret_dir_x = std::cos(trad);
                row.turret_dir_y = std::sin(trad);
                row.prev_turret_dir_x = row.turret_dir_x;
                row.prev_turret_dir_y = row.turret_dir_y;
                rows_.push_back(row);
                endInsertRows();
                index_.emplace(t.entity_id(), (int)rows_.size() - 1);
            }
        }
    }

signals:
    void mapDimensionsChanged();

private:
    mutable std::mutex m_;
    std::vector<QtTankRow> rows_;
    float map_width_{0.f};
    float map_height_{0.f};
    // Persistent id->row index cache to avoid rebuilding per delta.
    std::unordered_map<uint32_t, int> index_;

    static float slerpAngleRad(float x0, float y0, float x1, float y1, float alpha)
    {
        // Clamp alpha
        if (alpha <= 0.f)
            return std::atan2(y0, x0);
        if (alpha >= 1.f)
            return std::atan2(y1, x1);
        float dot = x0 * x1 + y0 * y1;
        if (dot > 1.f)
            dot = 1.f;
        if (dot < -1.f)
            dot = -1.f;
        // If vectors are almost identical, linear blend & normalize
        if (dot > 0.9995f || dot < -0.9995f) {
            float xr = x0 + (x1 - x0) * alpha;
            float yr = y0 + (y1 - y0) * alpha;
            float len = std::sqrt(xr * xr + yr * yr);
            if (len > 1e-6f) {
                xr /= len;
                yr /= len;
            }
            return std::atan2(yr, xr);
        }
        float theta = std::acos(dot);
        float sinTheta = std::sin(theta);
        if (sinTheta < 1e-6f) {
            return std::atan2(y0, x0); // fallback
        }
        float w0 = std::sin((1.f - alpha) * theta) / sinTheta;
        float w1 = std::sin(alpha * theta) / sinTheta;
        float xr = w0 * x0 + w1 * x1;
        float yr = w0 * y0 + w1 * y1;
        return std::atan2(yr, xr);
    }

    static float slerpAngleDeg(float x0, float y0, float x1, float y1, float alpha)
    {
        constexpr float kRadToDeg = 180.f / 3.14159265358979323846f;
        return slerpAngleRad(x0, y0, x1, y1, alpha) * kRadToDeg;
    }
};
