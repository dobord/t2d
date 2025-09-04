// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "game.pb.h"

#include <unordered_map>
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

    // Direct radian accessor (no interpolation needed for crates currently)
    Q_INVOKABLE float angleRad(int row) const
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
        if (row < 0 || (size_t)row >= rows_.size())
            return 0.f;
        return rows_[row].angle * kDegToRad;
    }

    QVariant data(const QModelIndex &idx, int role) const override
    {
        if (!idx.isValid())
            return {};
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
        std::vector<int> removeIdx;
        removeIdx.reserve(d.removed_crates_size());
        for (auto rid : d.removed_crates()) {
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
        std::vector<int> changed;
        changed.reserve(d.crates_size());
        for (auto &c : d.crates()) {
            auto it = index_.find(c.crate_id());
            if (it != index_.end()) {
                int i = it->second;
                auto &row = rows_[i];
                row.x = c.x();
                row.y = c.y();
                row.angle = c.angle();
                changed.push_back(i);
            } else {
                beginInsertRows({}, (int)rows_.size(), (int)rows_.size());
                rows_.push_back({c.crate_id(), c.x(), c.y(), c.angle()});
                endInsertRows();
                index_.emplace(c.crate_id(), (int)rows_.size() - 1);
            }
        }
        if (!changed.empty()) {
            std::sort(changed.begin(), changed.end());
            int rs = changed.front();
            int prev = rs;
            for (size_t k = 1; k < changed.size(); ++k) {
                int cur = changed[k];
                if (cur != prev + 1) {
                    emit dataChanged(index(rs), index(prev));
                    rs = cur;
                }
                prev = cur;
            }
            emit dataChanged(index(rs), index(prev));
        }
    }

private:
    std::vector<QtCrateRow> rows_;
    std::unordered_map<uint32_t, int> index_;
};
