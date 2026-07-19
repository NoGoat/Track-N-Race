import type { FrameScheduleHandle } from './frameScheduler';

interface InteractionEntry {
    readonly frame: (timestamp: number) => boolean;
    registered: boolean;
}

/**
 * Display-rate rAF lane for pointer interaction. Chart presentation FPS can
 * be capped or paused without making crosshairs and tooltips feel delayed.
 * Pointer events are still coalesced: each entry runs at most once per frame.
 */
class TimeChartInteractionFrameScheduler {
    private active = new Set<InteractionEntry>();
    private spare = new Set<InteractionEntry>();
    private raf = 0;

    register(frame: (timestamp: number) => boolean): FrameScheduleHandle {
        const entry: InteractionEntry = { frame, registered: true };
        return {
            wake: () => this.wake(entry),
            unregister: () => this.unregister(entry),
        };
    }

    private wake(entry: InteractionEntry) {
        if (!entry.registered) return;
        this.active.add(entry);
        if (this.raf === 0) {
            this.raf = requestAnimationFrame((timestamp) => this.run(timestamp));
        }
    }

    private unregister(entry: InteractionEntry) {
        if (!entry.registered) return;
        entry.registered = false;
        this.active.delete(entry);
        this.spare.delete(entry);
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
            this.active.delete(entry);
            if (!entry.registered) continue;
            let keepRunning = false;
            try {
                keepRunning = entry.frame(timestamp);
            } catch (error) {
                setTimeout(() => { throw error; });
            }
            if (keepRunning && entry.registered) this.active.add(entry);
        }
        current.clear();
        this.spare = current;

        if (this.active.size > 0) {
            this.raf = requestAnimationFrame((nextTimestamp) => this.run(nextTimestamp));
        }
    }
}

export const timeChartInteractionFrameScheduler = new TimeChartInteractionFrameScheduler();
