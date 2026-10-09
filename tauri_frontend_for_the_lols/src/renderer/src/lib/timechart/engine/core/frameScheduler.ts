export interface FrameScheduleHandle {
    wake(): void;
    unregister(): void;
}

export type TimeChartFrameRate = 0 | 1 | 10 | 30 | 60 | 120 | 'display';

interface FrameEntry {
    readonly element: HTMLElement;
    readonly frame: (timestamp: number) => boolean;
    registered: boolean;
}

/**
 * One animation-frame owner for every TimeChart instance. Entries return true
 * only while they need another frame; invisible work is parked until its chart
 * intersects again, and the browser rAF is cancelled when the queue is empty.
 */
class TimeChartFrameScheduler {
    private active = new Set<FrameEntry>();
    private spare = new Set<FrameEntry>();
    private dormant = new Set<FrameEntry>();
    private entriesByElement = new Map<HTMLElement, Set<FrameEntry>>();
    private visible = new Map<HTMLElement, boolean>();
    private raf = 0;
    private frameTimer: ReturnType<typeof setTimeout> | null = null;
    private focused = document.hasFocus();
    private focusedFrameRate: TimeChartFrameRate = 'display';
    private unfocusedFrameRate: TimeChartFrameRate = 30;
    private lastFrameAt = 0;
    private readonly observer: IntersectionObserver | null;

    constructor() {
        this.observer = typeof IntersectionObserver === 'undefined' ? null : new IntersectionObserver((records) => {
            for (const record of records) {
                const element = record.target as HTMLElement;
                const intersects = record.isIntersecting && record.intersectionRatio > 0;
                this.visible.set(element, intersects);
                const entries = this.entriesByElement.get(element);
                if (!entries) continue;
                if (this.isVisible(element)) {
                    for (const entry of entries) {
                        if (this.dormant.delete(entry)) this.active.add(entry);
                    }
                } else {
                    for (const entry of entries) {
                        if (this.active.delete(entry)) this.dormant.add(entry);
                    }
                }
            }
            this.ensureFrame();
            this.stopIfIdle();
        });
        document.addEventListener('visibilitychange', () => this.onPageVisibility());
        window.addEventListener('focus', () => this.onWindowFocusChanged(true));
        window.addEventListener('blur', () => this.onWindowFocusChanged(false));
    }

    configureFrameRates(focused: TimeChartFrameRate, unfocused: TimeChartFrameRate) {
        this.focusedFrameRate = focused;
        this.unfocusedFrameRate = unfocused;
        // Apply a changed cap immediately instead of inheriting cadence from
        // the previous rate (particularly when switching back to display FPS).
        this.lastFrameAt = 0;
        this.cancelFrameTimer();
        this.ensureFrame();
    }

    register(element: HTMLElement, frame: (timestamp: number) => boolean): FrameScheduleHandle {
        const entry: FrameEntry = { element, frame, registered: true };
        let entries = this.entriesByElement.get(element);
        if (!entries) {
            entries = new Set();
            this.entriesByElement.set(element, entries);
            // This one registration-time read seeds visibility until the
            // observer delivers its first asynchronous record.
            this.visible.set(
                element,
                element.isConnected && element.clientWidth > 0 && element.clientHeight > 0,
            );
            this.observer?.observe(element);
        }
        entries.add(entry);

        return {
            wake: () => this.wake(entry),
            unregister: () => this.unregister(entry),
        };
    }

    private wake(entry: FrameEntry) {
        if (!entry.registered) return;
        if (this.isVisible(entry.element)) {
            this.dormant.delete(entry);
            this.active.add(entry);
            this.ensureFrame();
        } else {
            this.active.delete(entry);
            this.dormant.add(entry);
        }
    }

    private unregister(entry: FrameEntry) {
        if (!entry.registered) return;
        entry.registered = false;
        this.active.delete(entry);
        this.spare.delete(entry);
        this.dormant.delete(entry);
        const entries = this.entriesByElement.get(entry.element);
        entries?.delete(entry);
        if (entries?.size === 0) {
            this.entriesByElement.delete(entry.element);
            this.visible.delete(entry.element);
            this.observer?.unobserve(entry.element);
        }
        this.stopIfIdle();
    }

    private isVisible(element: HTMLElement) {
        return document.visibilityState === 'visible' && (this.visible.get(element) ?? true);
    }

    private ensureFrame() {
        if (this.raf !== 0 || this.frameTimer !== null || this.active.size === 0) return;

        const frameRate = this.focused ? this.focusedFrameRate : this.unfocusedFrameRate;
        if (frameRate === 0) return;
        if (frameRate !== 'display' && this.lastFrameAt !== 0) {
            const remaining = 1000 / frameRate - (performance.now() - this.lastFrameAt);
            if (remaining > 1) {
                // Sleep until the next permitted presentation window. The rAF
                // requested afterwards aligns the actual draw with the display.
                this.frameTimer = setTimeout(() => {
                    this.frameTimer = null;
                    this.ensureFrame();
                }, remaining - 1);
                return;
            }
        }
        this.raf = requestAnimationFrame((timestamp) => this.run(timestamp));
    }

    private stopIfIdle() {
        if (this.active.size === 0 && this.raf !== 0) {
            cancelAnimationFrame(this.raf);
            this.raf = 0;
        }
        if (this.active.size === 0) this.cancelFrameTimer();
    }

    private run(timestamp: number) {
        this.raf = 0;
        const frameRate = this.focused ? this.focusedFrameRate : this.unfocusedFrameRate;
        // Zero is the explicit Pause policy. Keep the active work registered
        // so changing focus or FPS can resume every chart immediately, but do
        // not execute or reschedule any chart callbacks while it applies.
        if (frameRate === 0) return;
        if (frameRate !== 'display') {
            const interval = 1000 / frameRate;
            // rAF timestamps can land a fraction below an exact cadence on
            // displays whose refresh rate is a multiple of the requested cap.
            // A small tolerance avoids turning 60 FPS into 45 FPS at 180 Hz.
            if (this.lastFrameAt !== 0 && timestamp - this.lastFrameAt < interval - 0.25) {
                this.ensureFrame();
                return;
            }
            if (this.lastFrameAt === 0 || timestamp - this.lastFrameAt > interval * 4) {
                this.lastFrameAt = timestamp;
            } else {
                // Retain fractional cadence instead of resetting to the latest
                // display frame. This alternates gaps correctly when, for
                // example, 60 FPS is requested on a 144 Hz monitor.
                const elapsed = timestamp - this.lastFrameAt;
                this.lastFrameAt += Math.max(1, Math.floor((elapsed + 0.25) / interval)) * interval;
            }
        } else {
            this.lastFrameAt = timestamp;
        }

        const current = this.active;
        this.active = this.spare;
        this.active.clear();

        for (const entry of current) {
            // Consume a wake queued by an earlier client in this same frame;
            // this entry is about to service that work now.
            this.active.delete(entry);
            if (!entry.registered) continue;
            if (!this.isVisible(entry.element)) {
                this.dormant.add(entry);
                continue;
            }
            let keepRunning = false;
            try {
                keepRunning = entry.frame(timestamp);
            } catch (error) {
                // Keep one failing chart from stopping every other chart.
                setTimeout(() => { throw error; });
            }
            if (keepRunning && entry.registered) this.active.add(entry);
        }
        current.clear();
        this.spare = current;
        this.ensureFrame();
    }

    private onPageVisibility() {
        if (document.visibilityState !== 'visible') {
            for (const entry of this.active) this.dormant.add(entry);
            this.active.clear();
            this.stopIfIdle();
            return;
        }
        this.focused = document.hasFocus();
        this.lastFrameAt = 0;
        for (const entry of this.dormant) {
            if (this.visible.get(entry.element) !== false) {
                this.dormant.delete(entry);
                this.active.add(entry);
            }
        }
        this.ensureFrame();
    }

    private onWindowFocusChanged(focused: boolean) {
        if (this.focused === focused) return;
        this.focused = focused;
        this.lastFrameAt = 0;
        this.cancelFrameTimer();
        this.ensureFrame();
    }

    private cancelFrameTimer() {
        if (this.frameTimer === null) return;
        clearTimeout(this.frameTimer);
        this.frameTimer = null;
    }
}

export const timeChartFrameScheduler = new TimeChartFrameScheduler();
