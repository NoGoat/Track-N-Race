#pragma once

#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHash>
#include <QObject>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <QWindow>

#include <cmath>
#include <functional>
#include <utility>

// Bounded presentation queues for the Qt frontend. Producers may dirty a
// presenter as often as telemetry arrives, but each owner retains only its most
// recent callback. Ordinary QWidget work is event-driven and merely coalesced
// until the next event-loop turn. Charts use their configured frame cadence,
// while visual animations follow the display refresh rate like browser rAF.
// Back-pressure therefore collapses duplicate work without imposing an
// artificial dashboard FPS cap.
class PresentationScheduler final : public QObject {
public:
    // Stored FPS values mirror Electron's TimeChartFrameRate values. -1 is the
    // Qt persistence representation of Electron's "display" string.
    static constexpr int MatchDisplay = -1;
    enum class Policy { Ui, Animation, Chart };

    static PresentationScheduler& instance() {
        static PresentationScheduler* scheduler = new PresentationScheduler(qApp);
        return *scheduler;
    }

    void configureChartFrameRates(int focused, int unfocused) {
        chartFocusedFps_ = normalizeRate(focused, MatchDisplay);
        chartUnfocusedFps_ = normalizeRate(unfocused, 30);
        chart_.timer.stop();
        chart_.hasPresented = false;
        schedule(chart_, Policy::Chart);
    }

    void request(QObject* owner, std::function<void()> callback,
                 Policy policy = Policy::Ui) {
        if (!owner) return;
        Queue& target = queue(policy);
        target.pending.insert(owner, std::move(callback));
        if (!tracked_.contains(owner)) {
            tracked_.insert(owner);
            connect(owner, &QObject::destroyed, this, [this](QObject* destroyed) {
                ui_.pending.remove(destroyed);
                animation_.pending.remove(destroyed);
                chart_.pending.remove(destroyed);
                tracked_.remove(destroyed);
            });
        }
        schedule(target, policy);
    }

    void cancel(QObject* owner) {
        ui_.pending.remove(owner);
        animation_.pending.remove(owner);
        chart_.pending.remove(owner);
    }

private:
    struct Queue {
        QTimer timer;
        QElapsedTimer clock;
        QHash<QObject*, std::function<void()>> pending;
        qint64 lastPresentedNs = 0;
        bool hasPresented = false;
    };

    explicit PresentationScheduler(QObject* parent) : QObject(parent) {
        initializeQueue(ui_, Policy::Ui);
        initializeQueue(animation_, Policy::Animation);
        initializeQueue(chart_, Policy::Chart);
        if (qApp) {
            connect(qApp, &QGuiApplication::applicationStateChanged, this,
                    [this](Qt::ApplicationState) {
                ui_.timer.stop();
                animation_.timer.stop();
                chart_.timer.stop();
                ui_.hasPresented = false;
                animation_.hasPresented = false;
                chart_.hasPresented = false;
                schedule(ui_, Policy::Ui);
                schedule(animation_, Policy::Animation);
                schedule(chart_, Policy::Chart);
            });
        }
    }

    static int normalizeRate(int rate, int fallback) {
        switch (rate) {
            case MatchDisplay: case 0: case 1: case 10: case 30: case 60: case 120:
                return rate;
            default:
                return fallback;
        }
    }

    Queue& queue(Policy policy) {
        if (policy == Policy::Chart) return chart_;
        if (policy == Policy::Animation) return animation_;
        return ui_;
    }

    int configuredFrameRate(Policy policy) const {
        if (policy == Policy::Animation) return MatchDisplay;
        const bool focused = qApp && qApp->applicationState() == Qt::ApplicationActive;
        return focused ? chartFocusedFps_ : chartUnfocusedFps_;
    }

    static double displayRefreshRate() {
        QScreen* screen = nullptr;
        if (QWindow* window = QGuiApplication::focusWindow()) screen = window->screen();
        if (!screen) screen = QGuiApplication::primaryScreen();
        const double rate = screen ? screen->refreshRate() : 60.0;
        return std::isfinite(rate) && rate > 1.0 ? rate : 60.0;
    }

    double frameIntervalNs(Policy policy) const {
        const int configured = configuredFrameRate(policy);
        if (configured == 0) return 0.0;
        const double rate = configured == MatchDisplay ? displayRefreshRate()
                                                        : double(configured);
        return 1000000000.0 / rate;
    }

    void initializeQueue(Queue& target, Policy policy) {
        target.timer.setSingleShot(true);
        target.timer.setTimerType(Qt::PreciseTimer);
        Queue* targetPtr = &target;
        connect(&target.timer, &QTimer::timeout, this, [this, targetPtr, policy] {
            if (policy != Policy::Ui) {
                if (!targetPtr->clock.isValid()) targetPtr->clock.start();
                targetPtr->lastPresentedNs = targetPtr->clock.nsecsElapsed();
                targetPtr->hasPresented = true;
            }
            QHash<QObject*, std::function<void()>> ready;
            ready.swap(targetPtr->pending);
            for (auto it = ready.begin(); it != ready.end(); ++it)
                if (tracked_.contains(it.key())) it.value()();
            schedule(*targetPtr, policy);
        });
    }

    void schedule(Queue& target, Policy policy) {
        if (target.pending.isEmpty() || target.timer.isActive()) return;
        if (policy == Policy::Ui) {
            // Match Electron's ordinary store/component path: publish when data
            // arrives, while collapsing duplicate requests made during the same
            // producer burst. A real one-millisecond deadline keeps the callback
            // from being starved by continuous focused-window event traffic; it
            // is not used as a recurring cadence or FPS limit.
            target.timer.start(1);
            return;
        }

        const double frameNs = frameIntervalNs(policy);
        if (frameNs <= 0.0) return;
        if (!target.clock.isValid()) target.clock.start();
        // A zero-duration QTimer is a special idle timer: Qt only delivers it
        // after the current window-system event backlog.  Mouse, hover and
        // compositor traffic can therefore starve a newly woken chart queue
        // while the window is focused; deactivating the window drains that
        // traffic and makes the chart appear to update only out of focus.
        // Use the shortest real precise-timer deadline instead.  This remains
        // effectively immediate, but it participates in the timer queue and
        // cannot be indefinitely postponed by focused-window events.
        int delayMs = 1;
        if (target.hasPresented) {
            const double elapsedNs = double(target.clock.nsecsElapsed() - target.lastPresentedNs);
            const double remainingNs = frameNs - elapsedNs;
            if (remainingNs > 0.0)
                delayMs = qMax(1, int(std::ceil(remainingNs / 1000000.0)));
        }
        // As in Electron, a newly woken idle queue draws immediately (within one
        // millisecond); subsequent work observes the configured cadence.
        target.timer.start(delayMs);
    }

    Queue ui_;
    Queue animation_;
    Queue chart_;
    QSet<QObject*> tracked_;
    int chartFocusedFps_ = MatchDisplay;
    int chartUnfocusedFps_ = 30;
};
