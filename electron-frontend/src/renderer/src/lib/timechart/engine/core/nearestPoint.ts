import { ResolvedCoreOptions, TimeChartSeriesOptions } from '../options';
import { EventDispatcher } from '../utils';
import { CanvasLayer } from './canvasLayer';
import { ContentBoxDetector } from "./contentBoxDetector";
import { DataPoint, RenderModel } from './renderModel';

/** Convert a series point using its optional shared-canvas panel viewport. */
export function seriesPointToPixels(
    model: RenderModel,
    series: TimeChartSeriesOptions,
    x: number,
    y: number,
) {
    const pxX = model.xScale(x)!;
    const viewport = series.viewport;
    if (!viewport) return { x: pxX, y: model.yScale(y)! };

    const [plotBottom, plotTop] = model.yScale.range().map(Number);
    const [yMin, yMax] = model.yScale.domain().map(Number);
    const panelTop = plotTop + viewport.top * (plotBottom - plotTop);
    const panelBottom = plotTop + viewport.bottom * (plotBottom - plotTop) - (viewport.gapAfter ?? 0);
    const normalized = (y - yMin) / (yMax - yMin);
    return {
        x: pxX,
        y: panelBottom - normalized * (panelBottom - panelTop),
    };
}

export class NearestPointModel {
    dataPoints = new Map<TimeChartSeriesOptions, DataPoint>();
    private pointCache = new Map<TimeChartSeriesOptions, DataPoint>();
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
            this.dataPoints.clear();
            // Leaving the plot is a hover-state change even when no nearest
            // point is currently available. Custom tooltips listen to this
            // event to hide themselves, so always publish the transition.
            this.updated.dispatch();
        });

        model.updated.on(() => this.adjustPoints());
    }

    adjustPoints() {
        if (this.lastPointerPos === null) {
            // Model updates happen every display frame while a chart scrolls.
            // With no active pointer there is no nearest-point work to publish;
            // avoid waking the SVG markers and tooltip listeners every frame.
            if (this.dataPoints.size === 0) return;
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
                const pos = s.data.lowerBoundX(domain);
                let nearestIndex: number;
                if (pos === 0) nearestIndex = 0;
                else if (pos === s.data.length) nearestIndex = pos - 1;
                else {
                    const beforeX = s.data.xAt(pos - 1);
                    const afterX = s.data.xAt(pos);
                    nearestIndex = domain - beforeX <= afterX - domain ? pos - 1 : pos;
                }
                const x = s.data.xAt(nearestIndex);
                const y = s.data.yAt(nearestIndex);
                const pixel = seriesPointToPixels(this.model, s, x, y);
                const pxX = pixel.x;
                const pxY = pixel.y;

                if (pxX <= width && pxX >= 0 && pxY <= height && pxY >= 0) {
                    let nearest = this.pointCache.get(s);
                    if (!nearest) {
                        nearest = { x, y };
                        this.pointCache.set(s, nearest);
                    } else {
                        nearest.x = x;
                        nearest.y = y;
                    }
                    this.dataPoints.set(s, nearest);
                } else {
                    this.dataPoints.delete(s);
                }
            }
        }
        this.updated.dispatch();
    }
}
