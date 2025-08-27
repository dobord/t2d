// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>

#include <QtCore/QObject>
#include <QtCore/QTimer>

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
    Q_PROPERTY(int targetFrameHz READ targetFrameHz WRITE setTargetFrameHz NOTIFY targetFrameHzChanged)

public:
    explicit TimingState(QObject *parent = nullptr) : QObject(parent) {}

    int targetFrameHz() const { return frameHz_; }

    void setTargetFrameHz(int hz)
    {
        if (hz < 10)
            hz = 10;
        if (hz > 1000)
            hz = 1000;
        if (frameHz_ == hz)
            return;
        frameHz_ = hz;
        recomputeFrameInterval();
        emit targetFrameHzChanged();
    }

    // Start internal driving timer (call once after constructing on UI thread)
    Q_INVOKABLE void start()
    {
        if (frameTimer_)
            return;
        frameTimer_ = new QTimer(this);
        frameTimer_->setTimerType(Qt::PreciseTimer);
        frameTimer_->setSingleShot(true);
        connect(
            frameTimer_,
            &QTimer::timeout,
            this,
            [this]()
            {
                this->tickFrame();
                frameTimer_->start(frameIntervalMs_);
            });
        recomputeFrameInterval();
        frameTimer_->start(frameIntervalMs_);
    }

    void setTickIntervalMs(int ms)
    {
        std::scoped_lock lk(m_);
        tickIntervalMs_ = ms;
        smoothedTickIntervalMs_ = (float)ms;
    }

    // Called from network thread when a new authoritative tick (snapshot/delta) arrives.
    void markServerTick()
    {
        auto now = std::chrono::steady_clock::now();
        std::scoped_lock lk(m_);
        prevTick_ = lastTick_;
        lastTick_ = now;
        if (havePrevTick_) {
            float dt_ms = std::chrono::duration<float, std::milli>(lastTick_ - prevTick_).count();
            // Basic sanity bounds (ignore absurd spikes / zeros)
            if (dt_ms > 1.f && dt_ms < 1000.f) {
                // Exponential moving average smoothing of observed intervals
                constexpr float kBlend = 0.10f;
                if (smoothedTickIntervalMs_ <= 0.f)
                    smoothedTickIntervalMs_ = dt_ms;
                else
                    smoothedTickIntervalMs_ = smoothedTickIntervalMs_ + kBlend * (dt_ms - smoothedTickIntervalMs_);
            }
        }
        havePrevTick_ = true;
    }

    // Single-thread (UI) frame tick; updates alpha & timers. Must NOT be called concurrently with markServerTick.
    Q_INVOKABLE void tickFrame()
    {
        updateAlpha();
        // Countdown & auto requeue logic (same as old tickFrame())
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
        if (!matchOver_ && serverTickSeen_ && fallbackTicks_ > 0 && remainingHardCapSeconds_ == 0) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastServerTickUpdate_).count() > 1) {
                matchOutcome_ = 0; // draw
                matchOver_ = true;
                autoReturnSeconds_ = 10;
                lastAutoReturnDecrement_ = now;
                emit matchOverChanged();
                emit autoReturnSecondsChanged();
            }
        }
        emit frameTick();
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

    // (Legacy compatibility) kept to avoid build errors if still referenced; calls tickFrame().
    void update() { tickFrame(); }

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
    void frameTick();
    void targetFrameHzChanged();

private:
    mutable std::mutex m_;
    int tickIntervalMs_{50};
    float smoothedTickIntervalMs_{50.f};
    std::chrono::steady_clock::time_point lastTick_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point prevTick_ = std::chrono::steady_clock::now();
    bool havePrevTick_{false};
    float alpha_{}; // displayed interpolation alpha (smoothed / soft-clamped)
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
    QTimer *frameTimer_{nullptr};
    int frameHz_{144};
    int frameIntervalMs_{7}; // 1000/144 ~= 6.94ms -> rounded to 7ms

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

    void updateAlpha()
    {
        int localTickIntervalMs;
        float localSmoothed;
        std::chrono::steady_clock::time_point localLast;
        std::chrono::steady_clock::time_point localPrev;
        bool localHavePrev;
        {
            std::scoped_lock lk(m_);
            localTickIntervalMs = tickIntervalMs_;
            localSmoothed = smoothedTickIntervalMs_;
            localLast = lastTick_;
            localPrev = prevTick_;
            localHavePrev = havePrevTick_;
        }
        if (localTickIntervalMs <= 0)
            return;
        // Choose origin: one-tick delay for jitter buffer if we have previous tick.
        auto origin = (localHavePrev ? localPrev : localLast);
        auto now = std::chrono::steady_clock::now();
        float interval = (localSmoothed > 1.f ? localSmoothed : (float)localTickIntervalMs);
        float raw = std::chrono::duration<float, std::milli>(now - origin).count() / interval;
        // Soft overshoot compression: do not hard stop at 1; compress >1 region.
        float a;
        if (raw <= 1.f)
            a = raw;
        else {
            float overshoot = raw - 1.f; // allow up to ~0.15 visually
            a = 1.f + overshoot * 0.2f;
            if (a > 1.15f)
                a = 1.15f;
        }
        if (a < 0.f)
            a = 0.f;
        if (std::abs(a - alpha_) > 1e-6f) {
            alpha_ = a;
            emit alphaChanged();
        }
    }

    void recomputeFrameInterval()
    {
        double exact = 1000.0 / (double)frameHz_;
        int ms = (int)std::llround(exact);
        if (ms < 1)
            ms = 1;
        frameIntervalMs_ = ms;
    }
};
