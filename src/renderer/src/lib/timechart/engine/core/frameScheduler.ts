export interface FrameScheduleHandle {
    wake(): void;
    unregister(): void;
}

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
        if (this.raf === 0 && this.active.size > 0) {
            this.raf = requestAnimationFrame((timestamp) => this.run(timestamp));
        }
    }

    private stopIfIdle() {
        if (this.active.size === 0 && this.raf !== 0) {
            cancelAnimationFrame(this.raf);
            this.raf = 0;
        }
    }

    private run(timestamp: number) {
        this.raf = 0;
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
        for (const entry of this.dormant) {
            if (this.visible.get(entry.element) !== false) {
                this.dormant.delete(entry);
                this.active.add(entry);
            }
        }
        this.ensureFrame();
    }
}

export const timeChartFrameScheduler = new TimeChartFrameScheduler();
