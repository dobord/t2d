// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <mutex>

#include <QtCore/QObject>

class InputState : public QObject
{
    Q_OBJECT
    Q_PROPERTY(float move READ move WRITE setMove NOTIFY changed)
    Q_PROPERTY(float turn READ turn WRITE setTurn NOTIFY changed)
    Q_PROPERTY(float turretTurn READ turretTurn WRITE setTurretTurn NOTIFY changed)
    Q_PROPERTY(bool fire READ fire WRITE setFire NOTIFY changed)

public:
    explicit InputState(QObject *parent = nullptr) : QObject(parent) {}

    float move() const { return move_; }

    float turn() const { return turn_; }

    float turretTurn() const { return turretTurn_; }

    bool fire() const { return fire_; }

    void setMove(float v)
    {
        if (move_ != v) {
            move_ = v;
            emit changed();
        }
    }

    void setTurn(float v)
    {
        if (turn_ != v) {
            turn_ = v;
            emit changed();
        }
    }

    void setTurretTurn(float v)
    {
        if (turretTurn_ != v) {
            turretTurn_ = v;
            emit changed();
        }
    }

    void setFire(bool v)
    {
        if (fire_ != v) {
            fire_ = v;
            emit changed();
        }
    }

signals:
    void changed();

private:
    float move_{0.f};
    float turn_{0.f};
    float turretTurn_{0.f};
    bool fire_{false};
};
