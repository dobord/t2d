// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "game.pb.h"

#include <QtCore/QObject>

class LobbyState : public QObject
{
    Q_OBJECT
    Q_PROPERTY(uint32_t state READ state NOTIFY stateChanged)
    Q_PROPERTY(uint32_t position READ position NOTIFY positionChanged)
    Q_PROPERTY(uint32_t playersInQueue READ playersInQueue NOTIFY queueChanged)
    Q_PROPERTY(uint32_t neededForMatch READ neededForMatch NOTIFY queueChanged)
    Q_PROPERTY(uint32_t lobbyCountdown READ lobbyCountdown NOTIFY queueChanged)
    Q_PROPERTY(uint32_t projectedBotFill READ projectedBotFill NOTIFY queueChanged)

public:
    explicit LobbyState(QObject *parent = nullptr) : QObject(parent) {}

    uint32_t state() const { return state_; }

    uint32_t position() const { return position_; }

    uint32_t playersInQueue() const { return playersInQueue_; }

    uint32_t neededForMatch() const { return neededForMatch_; }

    uint32_t lobbyCountdown() const { return lobbyCountdown_; }

    uint32_t projectedBotFill() const { return projectedBotFill_; }

    void updateFromQueue(const t2d::QueueStatusUpdate &qs)
    {
        bool sc = false, qc = false, pc = false;
        if (state_ != qs.lobby_state()) {
            state_ = qs.lobby_state();
            sc = true;
        }
        if (position_ != qs.position()) {
            position_ = qs.position();
            pc = true;
        }
        if (playersInQueue_ != qs.players_in_queue() || neededForMatch_ != qs.needed_for_match()
            || lobbyCountdown_ != qs.lobby_countdown() || projectedBotFill_ != qs.projected_bot_fill()) {
            playersInQueue_ = qs.players_in_queue();
            neededForMatch_ = qs.needed_for_match();
            lobbyCountdown_ = qs.lobby_countdown();
            projectedBotFill_ = qs.projected_bot_fill();
            qc = true;
        }
        if (sc)
            emit stateChanged();
        if (pc)
            emit positionChanged();
        if (qc)
            emit queueChanged();
    }

signals:
    void stateChanged();
    void positionChanged();
    void queueChanged();

private:
    uint32_t state_{0};
    uint32_t position_{0};
    uint32_t playersInQueue_{0};
    uint32_t neededForMatch_{0};
    uint32_t lobbyCountdown_{0};
    uint32_t projectedBotFill_{0};
};
