import { scaleLinear } from "d3-scale";
import { ResolvedCoreOptions, TimeChartSeriesOptions } from '../options';
import { EventDispatcher } from '../utils';
import { timeChartFrameScheduler } from './frameScheduler';
import type { FrameScheduleHandle } from './frameScheduler';

export interface DataPoint {
    x: number;
    y: number;
}

export interface MinMax { min: number; max: number; }

export class RenderModel {
    xScale = scaleLinear();
    yScale = scaleLinear();
    xRange: MinMax | null = null;
    yRange: MinMax | null = null;
    private fixedYMin = NaN;
    private fixedYMax = NaN;
    private redrawRequested = false;
    private readonly frameHandle: FrameScheduleHandle;

    constructor(private options: ResolvedCoreOptions, element: HTMLElement) {
        this.frameHandle = timeChartFrameScheduler.register(element, () => {
            if (!this.redrawRequested || this.abortController.signal.aborted) return false;
            this.redrawRequested = false;
            this.update();
            return false;
        });
        if (options.xRange !== 'auto' && options.xRange) {
            this.xScale.domain([options.xRange.min, options.xRange.max])
        }
        if (options.yRange !== 'auto' && options.yRange) {
            this.applyFixedYRange(options.yRange.min, options.yRange.max)
        }
    }

    private applyFixedYRange(min: number, max: number) {
        if (min === this.fixedYMin && max === this.fixedYMax) {
            return;
        }
        this.fixedYMin = min;
        this.fixedYMax = max;
        this.yScale.domain([min, max]);
    }

    resized = new EventDispatcher<(width: number, height: number) => void>();
    resize(width: number, height: number) {
        const op = this.options;
        this.xScale.range([op.paddingLeft, width - op.paddingRight]);
        this.yScale.range([height - op.paddingBottom, op.paddingTop]);

        this.resized.dispatch(width, height)
        this.requestRedraw()
    }

    updated = new EventDispatcher();
    disposing = new EventDispatcher();
    readonly abortController = new AbortController();

    dispose() {
        if (!this.abortController.signal.aborted) {
            this.abortController.abort();
            this.frameHandle.unregister();
            this.disposing.dispatch();
        }
    }

    update() {
        // A synchronous scheduler-owned full draw satisfies any coalesced
        // redraw that was already waiting for this chart.
        this.redrawRequested = false;
        this.updateModel();
        this.updated.dispatch();
        for (const s of this.options.series) {
            s.data.markSynced();
        }
    }

    updateModel() {
        const o = this.options;
        let minDomain = Infinity;
        let maxDomain = -Infinity;
        let hasData = false;
        for (const s of o.series) {
            const d = s.data;
            if (d.length === 0) continue;
            hasData = true;
            const firstX = d.xAt(0);
            const lastX = d.xAt(d.length - 1);
            if (firstX < minDomain) minDomain = firstX;
            if (lastX > maxDomain) maxDomain = lastX;
        }
        if (!hasData) {
            return;
        }

        this.xRange = { max: maxDomain, min: minDomain };
        if (o.realTime) {
            const currentDomain = this.xScale.domain();
            const range = currentDomain[1] - currentDomain[0];
            this.xScale.domain([maxDomain - range, maxDomain]);
        } else if (o.xRange === 'auto') {
            this.xScale.domain([minDomain, maxDomain]);
        } else if (o.xRange) {
            this.xScale.domain([o.xRange.min, o.xRange.max])
        }

        // Track N Race supplies explicit y-ranges for every chart (including
        // its throttled auto-range implementation). Avoid scanning point deltas
        // unless the engine's own auto mode was explicitly requested.
        if (o.yRange === 'auto') {
            let minY = this.yRange?.min ?? Infinity;
            let maxY = this.yRange?.max ?? -Infinity;
            for (const s of o.series) {
                const d = s.data;
                const backStart = d.resetPending ? 0 : d.length - d.pushedBack;
                for (let i = backStart; i < d.length; i++) {
                    const y = d.yAt(i);
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
            if (Number.isFinite(minY) && Number.isFinite(maxY)) {
                this.yRange = { min: minY, max: maxY };
                this.fixedYMin = this.fixedYMax = NaN;
                this.yScale.domain([minY, maxY]).nice();
            }
        } else if (o.yRange) {
            this.applyFixedYRange(o.yRange.min, o.yRange.max);
        }
    }

    requestRedraw() {
        if (this.redrawRequested || this.abortController.signal.aborted) return;
        this.redrawRequested = true;
        this.frameHandle.wake();
    }

    pxPoint(dataPoint: DataPoint) {
        return {
            x: this.xScale(dataPoint.x)!,
            y: this.yScale(dataPoint.y)!,
        }
    }
}
