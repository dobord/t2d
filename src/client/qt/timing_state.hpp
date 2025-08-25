// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>

#include <QtCore/QObject>

class TimingState : public QObject
{
    Q_OBJECT
    Q_PROPERTY(float alpha READ alpha NOTIFY alphaChanged)
    Q_PROPERTY(int remainingHardCapSeconds READ remainingHardCapSeconds NOTIFY remainingHardCapSecondsChanged)
    Q_PROPERTY(bool matchOver READ matchOver NOTIFY matchOverChanged)
    Q_PROPERTY(int matchOutcome READ matchOutcome NOTIFY matchOverChanged) // 1=win, -1=lose, 0=draw
    Q_PROPERTY(int autoReturnSeconds READ autoReturnSeconds NOTIFY autoReturnSecondsChanged)
    Q_PROPERTY(bool matchActive READ matchActive NOTIFY matchActiveChanged)
    Q_PROPERTY(uint myEntityId READ myEntityId NOTIFY myEntityIdChanged)

public:
    explicit TimingState(QObject *parent = nullptr) : QObject(parent) {}

    void setTickIntervalMs(int ms) { tickIntervalMs_ = ms; }

    void markServerTick() { lastTick_ = std::chrono::steady_clock::now(); }

    Q_INVOKABLE void update()
    {
        if (tickIntervalMs_ <= 0)
            return;
        auto now = std::chrono::steady_clock::now();
        float a = std::chrono::duration<float, std::milli>(now - lastTick_).count() / (float)tickIntervalMs_;
        if (a > 1.f)
            a = 1.f;
        if (a < 0.f)
            a = 0.f;
        if (a != alpha_) {
            alpha_ = a;
            emit alphaChanged();
        }
    }

    float alpha() const { return alpha_; }

    void setHardCap(uint64_t serverTickAtStart, uint64_t tickRate, uint64_t fallbackTicks)
    {
        matchStartServerTick_ = serverTickAtStart;
        tickRate_ = tickRate;
        fallbackTicks_ = fallbackTicks;
        matchOver_ = false;
        serverTickSeen_ = false;
        // Pre-populate remaining time so HUD shows immediately.
        if (tickRate_ > 0 && fallbackTicks_ > 0) {
            int secs = (int)(fallbackTicks_ / tickRate_);
            if (secs != remainingHardCapSeconds_) {
                remainingHardCapSeconds_ = secs;
                emit remainingHardCapSecondsChanged();
            }
        } else if (remainingHardCapSeconds_ != 0) {
            remainingHardCapSeconds_ = 0;
            emit remainingHardCapSecondsChanged();
        }
        emit matchOverChanged();
    }

    void setServerTick(uint64_t tick)
    {
        currentServerTick_ = tick;
        serverTickSeen_ = true;
        updateRemaining();
    }

    void onMatchEnd(uint32_t winnerEntity, uint32_t myEntity)
    {
        if (matchOver_)
            return;
        if (winnerEntity == 0)
            matchOutcome_ = 0;
        else if (winnerEntity == myEntity)
            matchOutcome_ = 1;
        else
            matchOutcome_ = -1;
        matchOver_ = true;
        remainingHardCapSeconds_ = 0;
        emit remainingHardCapSecondsChanged();
        emit matchOverChanged();
        autoReturnSeconds_ = 10; // start countdown
        lastAutoReturnDecrement_ = std::chrono::steady_clock::now();
        emit autoReturnSecondsChanged();
    }

    void tickFrame()
    {
        update();
        if (matchOver_ && autoReturnSeconds_ > 0) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastAutoReturnDecrement_).count() >= 1) {
                lastAutoReturnDecrement_ = now;
                --autoReturnSeconds_;
                emit autoReturnSecondsChanged();
            }
        }
        if (matchOver_ && autoReturnSeconds_ == 0 && !autoRequeueTriggered_) {
            requeueRequested_ = true;
            autoRequeueTriggered_ = true;
        }
        // Fallback: if hard cap elapsed (remainingHardCapSeconds_ == 0) and we did not receive MatchEnd after >1s,
        // treat as draw.
        if (!matchOver_ && serverTickSeen_ && fallbackTicks_ > 0 && remainingHardCapSeconds_ == 0) {
            auto now = std::chrono::steady_clock::now();
            // Use lastServerTickUpdate_ (track when serverTickSeen_) to avoid early trigger before any snapshot
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastServerTickUpdate_).count() > 1) {
                matchOutcome_ = 0; // draw
                matchOver_ = true;
                autoReturnSeconds_ = 10;
                lastAutoReturnDecrement_ = now;
                emit matchOverChanged();
                emit autoReturnSecondsChanged();
            }
        }
    }

    int remainingHardCapSeconds() const { return remainingHardCapSeconds_; }

    bool matchOver() const { return matchOver_; }

    bool matchActive() const { return matchActive_; }

    int matchOutcome() const { return matchOutcome_; }

    int autoReturnSeconds() const { return autoReturnSeconds_; }

    uint32_t myEntityId() const { return myEntityId_; }

    void setMyEntityId(uint32_t id)
    {
        if (myEntityId_ != id) {
            myEntityId_ = id;
            emit myEntityIdChanged();
        }
    }

    Q_INVOKABLE void requestRequeueNow()
    {
        if (matchOver_) {
            requeueRequested_ = true;
            autoReturnSeconds_ = 0;
            emit autoReturnSecondsChanged();
        }
    }

    bool consumeRequeueRequest()
    {
        if (requeueRequested_) {
            requeueRequested_ = false;
            return true;
        }
        return false;
    }

    void setMatchActive(bool a)
    {
        if (matchActive_ != a) {
            matchActive_ = a;
            emit matchActiveChanged();
        }
    }

signals:
    void remainingHardCapSecondsChanged();
    void matchOverChanged();
    void autoReturnSecondsChanged();
    void matchActiveChanged();

signals:
    void alphaChanged();
    void myEntityIdChanged();

private:
    int tickIntervalMs_{50};
    std::chrono::steady_clock::time_point lastTick_ = std::chrono::steady_clock::now();
    float alpha_{};
    uint64_t matchStartServerTick_{};
    uint64_t tickRate_{20};
    uint64_t fallbackTicks_{0};
    uint64_t currentServerTick_{};
    int remainingHardCapSeconds_{0};
    bool matchOver_{false};
    int matchOutcome_{0};
    int autoReturnSeconds_{0};
    std::chrono::steady_clock::time_point lastAutoReturnDecrement_ = std::chrono::steady_clock::now();
    bool requeueRequested_{false};
    bool autoRequeueTriggered_{false};
    bool serverTickSeen_{false};
    std::chrono::steady_clock::time_point lastServerTickUpdate_ = std::chrono::steady_clock::now();
    bool matchActive_{false};
    uint32_t myEntityId_{0};

    void updateRemaining()
    {
        if (matchOver_ || tickRate_ == 0 || fallbackTicks_ == 0)
            return;
        if (currentServerTick_ < matchStartServerTick_)
            return;
        uint64_t elapsed = currentServerTick_ - matchStartServerTick_;
        lastServerTickUpdate_ = std::chrono::steady_clock::now();
        if (elapsed >= fallbackTicks_) {
            remainingHardCapSeconds_ = 0;
            emit remainingHardCapSecondsChanged();
            return;
        }
        uint64_t remainingTicks = fallbackTicks_ - elapsed;
        int secs = (int)(remainingTicks / tickRate_);
        if (secs != remainingHardCapSeconds_) {
            remainingHardCapSeconds_ = secs;
            emit remainingHardCapSecondsChanged();
        }
    }
};
