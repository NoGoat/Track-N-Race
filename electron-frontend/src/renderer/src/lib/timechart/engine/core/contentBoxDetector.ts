import { ResolvedCoreOptions } from '../options';
import { EventDispatcher } from '../utils';
import { RenderModel } from './renderModel';
import type { FrameScheduleHandle } from './frameScheduler';
import { timeChartInteractionFrameScheduler } from './interactionFrameScheduler';

export class ContentBoxDetector {
    node: HTMLElement;
    readonly moved = new EventDispatcher<(x: number, y: number) => void>();
    readonly entered = new EventDispatcher();
    readonly left = new EventDispatcher();
    private movePending = false;
    private pointerX = 0;
    private pointerY = 0;
    private readonly frameHandle: FrameScheduleHandle;

    setPadding(left: number, right: number, top: number, bottom: number) {
        this.node.style.left = `${left}px`;
        this.node.style.right = `${right}px`;
        this.node.style.top = `${top}px`;
        this.node.style.bottom = `${bottom}px`;
    }

    constructor(el: HTMLElement, model: RenderModel, options: ResolvedCoreOptions) {
        this.node = document.createElement('div');
        this.node.style.position = 'absolute';
        this.setPadding(options.paddingLeft, options.paddingRight, options.paddingTop, options.paddingBottom);
        el.shadowRoot!.appendChild(this.node);
        this.frameHandle = timeChartInteractionFrameScheduler.register(() => {
            if (!this.movePending || model.abortController.signal.aborted) return false;
            this.movePending = false;
            this.moved.dispatch(this.pointerX, this.pointerY);
            return false;
        });

        // Mouse devices can report substantially faster than the display can
        // paint. Coalesce the chart's crosshair, nearest-point and tooltip work
        // behind one display-rate animation-frame event instead of giving each
        // feature its own unthrottled DOM mousemove listener. This interaction
        // lane deliberately stays independent from the chart presentation cap.
        const signal = model.abortController.signal;
        this.node.addEventListener('mousemove', (ev) => {
            this.pointerX = ev.offsetX;
            this.pointerY = ev.offsetY;
            this.movePending = true;
            this.frameHandle.wake();
        }, { signal });
        this.node.addEventListener('mouseenter', () => this.entered.dispatch(), { signal });
        this.node.addEventListener('mouseleave', () => {
            this.movePending = false;
            this.left.dispatch();
        }, { signal });

        model.disposing.on(() => {
            this.frameHandle.unregister();
            el.shadowRoot!.removeChild(this.node);
        })
    }
}
