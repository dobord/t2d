// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <chrono>

#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtQuick/QQuickWindow>

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
    // Instrumentation for diagnosing freezes
    Q_PROPERTY(double lastFrameMs READ lastFrameMs NOTIFY frameStatsChanged)
    Q_PROPERTY(double maxFrameMs READ maxFrameMs NOTIFY frameStatsChanged)
    Q_PROPERTY(qulonglong longFrameCount READ longFrameCount NOTIFY frameStatsChanged)

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
        idealFramePeriodMs_ = 1000.0 / static_cast<double>(frameHz_);
        recomputeFrameInterval();
        emit targetFrameHzChanged();
    }

    // Start internal driving timer (call once after constructing on UI thread)
    Q_INVOKABLE void enableVsyncPacing(QObject *windowObject)
    {
        if (usingVsync_)
            return;
        QQuickWindow *w = qobject_cast<QQuickWindow *>(windowObject);
        if (!w)
            return;
        window_ = w;
        usingVsync_ = true;
        // Connect to frameSwapped (after present) so we pace on actual swap; request first update.
        connect(window_, &QQuickWindow::frameSwapped, this, [this]() { this->onVsyncFrame(); });
        window_->requestUpdate();
    }

    Q_INVOKABLE void start()
    {
        if (usingVsync_) {
            // Vsync pacing will drive tickFrame via onVsyncFrame()
            idealFramePeriodMs_ = 1000.0 / static_cast<double>(frameHz_);
            return;
        }
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
                // Fractional frame pacing: distribute 6/7ms to approximate 6.94...
                double nextExact = idealFramePeriodMs_;
                double candidate = nextExact + frameErrorAccum_;
                int nextMs = static_cast<int>(candidate);
                if (nextMs < 1)
                    nextMs = 1;
                frameErrorAccum_ = candidate - static_cast<double>(nextMs);
                frameTimer_->start(nextMs);
            });
        idealFramePeriodMs_ = 1000.0 / static_cast<double>(frameHz_);
        frameTimer_->start(frameIntervalMs_);
    }

    void setTickIntervalMs(int ms)
    {
        std::scoped_lock lk(m_);
        tickIntervalMs_ = ms;
        smoothedTickIntervalMs_ = (float)ms;
        lastIntervalMs_ = (float)ms; // initialize window length
    }

    // Called from network thread when a new authoritative tick (snapshot/delta) arrives.
    void markServerTick()
    {
        auto now = std::chrono::steady_clock::now();
        std::scoped_lock lk(m_);
        prevTick_ = lastTick_;
        lastTick_ = now;
        // ring buffer push
        if (tickTimesSize_ < (int)tickTimes_.size()) {
            tickTimes_[tickTimesSize_++] = now;
        } else {
            for (int i = 1; i < tickTimesSize_; ++i)
                tickTimes_[i - 1] = tickTimes_[i];
            tickTimes_[tickTimesSize_ - 1] = now;
        }
        if (havePrevTick_) {
            float dt_ms = std::chrono::duration<float, std::milli>(lastTick_ - prevTick_).count();
            if (dt_ms > 1.f && dt_ms < 1000.f) {
                constexpr float kBlend = 0.10f;
                if (smoothedTickIntervalMs_ <= 0.f)
                    smoothedTickIntervalMs_ = dt_ms;
                else
                    smoothedTickIntervalMs_ += kBlend * (dt_ms - smoothedTickIntervalMs_);
                lastIntervalMs_ = dt_ms;
                // Stable interval update windowed
                if (++ticksSinceStableUpdate_ >= stableUpdateWindow_) {
                    // Only adopt if change exceeds threshold (reduces speed jumps)
                    float diff = std::abs(stableIntervalMs_ - smoothedTickIntervalMs_);
                    if (diff > stableIntervalMs_ * 0.05f) {
                        stableIntervalMs_ = smoothedTickIntervalMs_;
                    }
                    ticksSinceStableUpdate_ = 0;
                }
            } else {
                lastIntervalMs_ = (float)tickIntervalMs_;
            }
        } else {
            lastIntervalMs_ = (float)tickIntervalMs_;
            stableIntervalMs_ = lastIntervalMs_;
        }
        havePrevTick_ = true;
    }

    // Single-thread (UI) frame tick; updates alpha & timers.
    Q_INVOKABLE void tickFrame()
    {
        // Frame duration instrumentation
        auto nowStart = std::chrono::steady_clock::now();
        if (lastFrameStart_.time_since_epoch().count() != 0) {
            lastFrameDurationMs_ = std::chrono::duration<double, std::milli>(nowStart - lastFrameStart_).count();
            if (lastFrameDurationMs_ > maxFrameDurationMs_)
                maxFrameDurationMs_ = lastFrameDurationMs_;
            if (lastFrameDurationMs_ > longFrameThresholdMs_)
                ++longFrameCount_;
            static int frameStatEmitCounter = 0;
            if (++frameStatEmitCounter >= 30) {
                frameStatEmitCounter = 0;
                emit frameStatsChanged();
            }
        }
        lastFrameStart_ = nowStart;
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

    // Frame stats accessors
    double lastFrameMs() const { return lastFrameDurationMs_; }

    double maxFrameMs() const { return maxFrameDurationMs_; }

    qulonglong longFrameCount() const { return longFrameCount_; }

    Q_INVOKABLE void resetFrameStats()
    {
        maxFrameDurationMs_ = 0.0;
        longFrameCount_ = 0;
        emit frameStatsChanged();
    }

    void setHardCap(uint64_t serverTickAtStart, uint64_t tickRate, uint64_t fallbackTicks)
    {
        matchStartServerTick_ = serverTickAtStart;
        tickRate_ = tickRate;
        fallbackTicks_ = fallbackTicks;
        matchOver_ = false;
        // Reset auto-return / requeue state for a fresh match so that auto requeue works every time.
        if (autoRequeueTriggered_ || requeueRequested_ || autoReturnSeconds_ != 0) {
            autoRequeueTriggered_ = false;
            requeueRequested_ = false;
            if (autoReturnSeconds_ != 0) {
                autoReturnSeconds_ = 0;
                emit autoReturnSecondsChanged();
            }
        }
        serverTickSeen_ = false;
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

    // Explicit QML hook used by the match end overlay button (alias for requestRequeueNow()).
    Q_INVOKABLE void returnToLobbyNow() { requestRequeueNow(); }

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
    void frameStatsChanged();

private:
    mutable std::mutex m_;
    int tickIntervalMs_{50};
    float smoothedTickIntervalMs_{50.f}; // EMA (informational)
    float lastIntervalMs_{50.f}; // Locked window length used for current interpolation cycle
    std::chrono::steady_clock::time_point lastTick_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point prevTick_ = std::chrono::steady_clock::now();
    bool havePrevTick_{false};
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
    QTimer *frameTimer_{nullptr};
    int frameHz_{144};
    int frameIntervalMs_{7}; // 1000/144 ~= 6.94ms -> 7ms
    // snapshot buffering & pacing members
    std::array<std::chrono::steady_clock::time_point, 8> tickTimes_{}; // arrival times
    int tickTimesSize_{0};
    int playbackDelayTicks_{1}; // fixed delay (buffer one full tick)
    float stableIntervalMs_{50.f}; // frozen playback interval
    int ticksSinceStableUpdate_{0};
    int stableUpdateWindow_{4};
    double idealFramePeriodMs_{1000.0 / 144.0};
    double frameErrorAccum_{0.0};

    QQuickWindow *window_{nullptr};
    bool usingVsync_{false};
    std::chrono::steady_clock::time_point lastVsyncTime_{};
    double vsyncAccumulatorMs_{0.0};
    // Grace stretch parameters: when next server tick is late, gradually slow interpolation instead of flat stall.
    float maxStretchFactor_{1.3f}; // Allow window to extend up to 1.3x original length
    float stretchStartFraction_{0.9f}; // Begin stretching after 90% progress if no new tick
    // Instrumentation
    std::chrono::steady_clock::time_point lastFrameStart_{};
    double lastFrameDurationMs_{0.0};
    double maxFrameDurationMs_{0.0};
    uint64_t longFrameCount_{0};
    double longFrameThresholdMs_{16.0}; // > ~2 frames at 144Hz (~13.9ms) treated as long

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
        std::chrono::steady_clock::time_point localPrev;
        bool localHavePrev;
        float windowMs;
        int localTickCount;
        std::array<std::chrono::steady_clock::time_point, 8> localTimes;
        int localPlaybackDelay;
        float localStable;
        {
            std::scoped_lock lk(m_);
            localPrev = prevTick_;
            localHavePrev = havePrevTick_;
            windowMs = (lastIntervalMs_ > 1.f ? lastIntervalMs_ : (float)tickIntervalMs_);
            localTickCount = tickTimesSize_;
            localTimes = tickTimes_;
            localPlaybackDelay = playbackDelayTicks_;
            localStable = stableIntervalMs_;
        }
        if (localTickCount >= localPlaybackDelay + 2) {
            int endIndex = localTickCount - 1 - localPlaybackDelay;
            int startIndex = endIndex - 1;
            if (startIndex >= 0) {
                auto startTime = localTimes[startIndex];
                auto endTime = localTimes[endIndex];
                auto now = std::chrono::steady_clock::now();
                auto delayedNow = now - std::chrono::milliseconds((long long)(localPlaybackDelay * localStable));
                float spanMs = (localStable > 1.f ? localStable : windowMs); // expected base interval
                float elapsedMs = std::chrono::duration<float, std::milli>(delayedNow - startTime).count();
                float a = 0.f;
                if (delayedNow <= startTime) {
                    a = 0.f;
                } else {
                    float baseProgress = elapsedMs / spanMs; // may exceed 1 if tick late
                    if (baseProgress <= stretchStartFraction_ || delayedNow <= endTime) {
                        // Normal progression until stretch threshold OR until scheduled end within time.
                        a = std::min(std::max(baseProgress, 0.f), 1.f);
                    } else {
                        // Grace stretch: remap remaining progress to extended window.
                        float extendedSpanMs = spanMs * maxStretchFactor_;
                        float startStretchMs = stretchStartFraction_ * spanMs;
                        float denom = (extendedSpanMs - startStretchMs);
                        float numer = elapsedMs - startStretchMs;
                        if (denom < 1e-3f)
                            a = 1.f; // degenerate
                        else if (elapsedMs >= extendedSpanMs) {
                            a = 1.f; // fully stretched limit reached
                        } else {
                            float t = numer / denom; // 0..1 across stretch region
                            // Linear blend from stretchStartFraction_ .. 1
                            a = stretchStartFraction_ + t * (1.f - stretchStartFraction_);
                        }
                    }
                }
                if (std::abs(a - alpha_) > 1e-6f) {
                    alpha_ = a;
                    emit alphaChanged();
                }
                return;
            }
        }
        if (!localHavePrev) {
            if (alpha_ != 0.f) {
                alpha_ = 0.f;
                emit alphaChanged();
            }
            return;
        }
        auto now = std::chrono::steady_clock::now();
        float elapsedMs = std::chrono::duration<float, std::milli>(now - localPrev).count();
        float a = 0.f;
        if (windowMs > 0.5f) {
            float baseProgress = elapsedMs / windowMs;
            if (baseProgress <= stretchStartFraction_) {
                a = std::max(0.f, baseProgress);
            } else {
                // Apply grace stretch when beyond threshold (simulates buffered mode behavior in early phase)
                float extendedSpanMs = windowMs * maxStretchFactor_;
                float startStretchMs = stretchStartFraction_ * windowMs;
                if (elapsedMs >= extendedSpanMs)
                    a = 1.f;
                else {
                    float t = (elapsedMs - startStretchMs) / (extendedSpanMs - startStretchMs);
                    if (t < 0.f)
                        t = 0.f;
                    if (t > 1.f)
                        t = 1.f;
                    a = stretchStartFraction_ + t * (1.f - stretchStartFraction_);
                }
            }
        }
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

    void onVsyncFrame()
    {
        auto now = std::chrono::steady_clock::now();
        if (!lastVsyncTime_.time_since_epoch().count()) {
            lastVsyncTime_ = now;
            tickFrame();
            if (window_)
                window_->requestUpdate();
            return;
        }
        double dtMs = std::chrono::duration<double, std::milli>(now - lastVsyncTime_).count();
        lastVsyncTime_ = now;
        vsyncAccumulatorMs_ += dtMs;
        int steps = 0;
        while (vsyncAccumulatorMs_ + 0.0001 >= idealFramePeriodMs_ && steps < 4) {
            vsyncAccumulatorMs_ -= idealFramePeriodMs_;
            tickFrame();
            ++steps;
        }
        if (steps == 0)
            tickFrame();
        if (window_)
            window_->requestUpdate();
    }
};
