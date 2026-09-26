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
        resetChartCadence();
        schedule(chart_, Policy::Chart);
    }

    // Called by every chart canvas (QRhiWidget::frameSubmitted). In "Match
    // display" mode this is the chart queue's requestAnimationFrame: the next
    // batch of chart work is released only once the previous frame has been
    // submitted, so the swapchain's vsync wait — not a millisecond timer — sets
    // the frame rate, exactly like Electron's rAF-driven TimeChart scheduler.
    void chartFrameSubmitted() {
        if (!chartAwaitingFrame_) return;
        chartAwaitingFrame_ = false;
        chartFrameFallback_.stop();
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

    void resetChartCadence() {
        chart_.timer.stop();
        chart_.hasPresented = false;
        chartAwaitingFrame_ = false;
        chartFrameFallback_.stop();
    }

    explicit PresentationScheduler(QObject* parent) : QObject(parent) {
        initializeQueue(ui_, Policy::Ui);
        initializeQueue(animation_, Policy::Animation);
        initializeQueue(chart_, Policy::Chart);
        // Safety net for display-paced charts: if a released batch did not
        // repaint any canvas (nothing changed, or every chart is hidden), no
        // frameSubmitted arrives, so release the queue after two display frames.
        chartFrameFallback_.setSingleShot(true);
        chartFrameFallback_.setTimerType(Qt::PreciseTimer);
        connect(&chartFrameFallback_, &QTimer::timeout, this, [this] {
            chartAwaitingFrame_ = false;
            schedule(chart_, Policy::Chart);
        });
        if (qApp) {
            connect(qApp, &QGuiApplication::applicationStateChanged, this,
                    [this](Qt::ApplicationState) {
                ui_.timer.stop();
                animation_.timer.stop();
                ui_.hasPresented = false;
                animation_.hasPresented = false;
                resetChartCadence();
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
            if (policy == Policy::Chart) {
                if (!advanceChartCadence()) return;   // woke early for a capped rate
            } else if (policy != Policy::Ui) {
                if (!targetPtr->clock.isValid()) targetPtr->clock.start();
                targetPtr->lastPresentedNs = targetPtr->clock.nsecsElapsed();
                targetPtr->hasPresented = true;
            }
            QHash<QObject*, std::function<void()>> ready;
            ready.swap(targetPtr->pending);
            for (auto it = ready.begin(); it != ready.end(); ++it)
                if (tracked_.contains(it.key())) it.value()();
            if (policy == Policy::Chart && configuredFrameRate(policy) == MatchDisplay
                    && !ready.isEmpty()) {
                // Wait for the frame this batch produces before releasing the next.
                chartAwaitingFrame_ = true;
                chartFrameFallback_.start(qMax(4, int(std::ceil(2.0 * 1000.0 / displayRefreshRate()))));
                return;
            }
            schedule(*targetPtr, policy);
        });
    }

    // Electron's capped-rate cadence (frameScheduler.ts): accept the frame once
    // at least interval - 0.25 ms has elapsed and advance the reference by whole
    // intervals, so fractional cadences alternate correctly (e.g. 120 FPS on a
    // 180 Hz display) instead of rounding every gap up. Returns false when the
    // timer fired too early; the queue is then rescheduled.
    bool advanceChartCadence() {
        Queue& target = chart_;
        if (!target.clock.isValid()) target.clock.start();
        const qint64 now = target.clock.nsecsElapsed();
        const int configured = configuredFrameRate(Policy::Chart);
        if (configured == MatchDisplay || !target.hasPresented) {
            target.lastPresentedNs = now;
            target.hasPresented = true;
            return true;
        }
        const double interval = frameIntervalNs(Policy::Chart);
        const double elapsed = double(now - target.lastPresentedNs);
        if (elapsed < interval - 250000.0 - 1000000.0) {
            schedule(target, Policy::Chart);
            return false;
        }
        if (elapsed > interval * 4) {
            target.lastPresentedNs = now;
        } else {
            const double steps = qMax(1.0, std::floor((elapsed + 250000.0) / interval));
            target.lastPresentedNs += qint64(steps * interval);
        }
        return true;
    }

    void schedule(Queue& target, Policy policy) {
        if (target.pending.isEmpty() || target.timer.isActive()) return;
        if (policy == Policy::Chart && chartAwaitingFrame_) return;   // released by frameSubmitted
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
        if (policy == Policy::Chart) {
            // Display rate: no interval wait at all — the frameSubmitted gate
            // above and the swapchain's vsync pace the charts. Capped rates sleep
            // until one millisecond before the next slot (Electron's setTimeout
            // before rAF); the present then aligns the frame to vsync.
            if (configuredFrameRate(policy) != MatchDisplay && target.hasPresented) {
                const double remainingNs = frameNs -
                    double(target.clock.nsecsElapsed() - target.lastPresentedNs);
                if (remainingNs > 1000000.0)
                    delayMs = qMax(1, int(std::floor(remainingNs / 1000000.0)) - 1);
            }
        } else if (target.hasPresented) {
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
    bool chartAwaitingFrame_ = false;   // display mode: a released batch's frame is pending
    QTimer chartFrameFallback_;
};
