import { ResolvedCoreOptions, TimeChartSeriesOptions } from '../options';
import { domainSearch, EventDispatcher } from '../utils';
import { CanvasLayer } from './canvasLayer';
import { ContentBoxDetector } from "./contentBoxDetector";
import { DataPoint, RenderModel } from './renderModel';

export class NearestPointModel {
    dataPoints = new Map<TimeChartSeriesOptions, DataPoint>();
    lastPointerPos: null | {x: number, y: number} = null;

    updated = new EventDispatcher();

    constructor(
        private canvas: CanvasLayer,
        private model: RenderModel,
        private options: ResolvedCoreOptions,
        detector: ContentBoxDetector
    ) {
        detector.moved.on((x, y) => {
            this.lastPointerPos = {
                x: x + options.paddingLeft,
                y: y + options.paddingTop,
            };
            this.adjustPoints();
        });
        detector.left.on(() => {
            this.lastPointerPos = null;
            this.adjustPoints();
        });

        model.updated.on(() => this.adjustPoints());
    }

    adjustPoints() {
        if (this.lastPointerPos === null) {
            this.dataPoints.clear();
        } else {
            const domain = this.model.xScale.invert(this.lastPointerPos.x);
            const width = this.canvas.canvas.width / this.options.pixelRatio;
            const height = this.canvas.canvas.height / this.options.pixelRatio;
            for (const s of this.options.series) {
                if (s.data.length == 0 || !s.visible) {
                    this.dataPoints.delete(s);
                    continue;
                }
                const pos = domainSearch(s.data, 0, s.data.length, domain, d => d.x);
                const before = pos > 0 ? s.data[pos - 1] : undefined;
                const after = pos < s.data.length ? s.data[pos] : undefined;
                const nearest = before === undefined ? after!
                    : after === undefined ? before
                        : domain - before.x <= after.x - domain ? before : after;
                const pxX = this.model.xScale(nearest.x)!;
                const pxY = this.model.yScale(nearest.y)!;

                if (pxX <= width && pxX >= 0 && pxY <= height && pxY >= 0) {
                    this.dataPoints.set(s, nearest);
                } else {
                    this.dataPoints.delete(s);
                }
            }
        }
        this.updated.dispatch();
    }
}
