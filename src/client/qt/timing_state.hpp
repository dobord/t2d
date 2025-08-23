// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>

#include <QtCore/QObject>

class TimingState : public QObject
{
    Q_OBJECT
    Q_PROPERTY(float alpha READ alpha NOTIFY alphaChanged)

public:
    explicit TimingState(QObject *parent = nullptr) : QObject(parent) {}

    void setTickIntervalMs(int ms) { tickIntervalMs_ = ms; }

    void markServerTick() { lastTick_ = std::chrono::steady_clock::now(); }

    void update()
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

signals:
    void alphaChanged();

private:
    int tickIntervalMs_{50};
    std::chrono::steady_clock::time_point lastTick_ = std::chrono::steady_clock::now();
    float alpha_{};
};
