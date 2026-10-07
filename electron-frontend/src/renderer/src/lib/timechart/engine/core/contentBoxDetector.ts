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
    private pendingClientX = 0;
    private pendingClientY = 0;
    /** Viewport position of the pointer last published through `moved`. */
    clientX = 0;
    clientY = 0;
    // Viewport origin of the content box, read once per hover rather than per
    // event. `MouseEvent.offsetX` and `getBoundingClientRect()` both force a
    // synchronous layout whenever any chart has dirtied the DOM, which during
    // a live scroll is every frame.
    private origin: { left: number, top: number } | null = null;
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
            this.clientX = this.pendingClientX;
            this.clientY = this.pendingClientY;
            this.moved.dispatch(this.pointerX, this.pointerY);
            return false;
        });
        const invalidateOrigin = () => { this.origin = null; };

        // Mouse devices can report substantially faster than the display can
        // paint. Coalesce the chart's crosshair, nearest-point and tooltip work
        // behind one display-rate animation-frame event instead of giving each
        // feature its own unthrottled DOM mousemove listener. This interaction
        // lane deliberately stays independent from the chart presentation cap.
        const signal = model.abortController.signal;
        this.node.addEventListener('mousemove', (ev) => {
            if (!this.origin) {
                const rect = this.node.getBoundingClientRect();
                this.origin = { left: rect.left, top: rect.top };
            }
            this.pendingClientX = ev.clientX;
            this.pendingClientY = ev.clientY;
            this.pointerX = ev.clientX - this.origin.left;
            this.pointerY = ev.clientY - this.origin.top;
            this.movePending = true;
            this.frameHandle.wake();
        }, { signal });
        this.node.addEventListener('mouseenter', () => {
            invalidateOrigin();
            this.entered.dispatch();
        }, { signal });
        this.node.addEventListener('mouseleave', () => {
            this.movePending = false;
            invalidateOrigin();
            this.left.dispatch();
        }, { signal });
        // Re-read the origin after anything that can move the chart under a
        // stationary hover.
        model.resized.on(invalidateOrigin);
        window.addEventListener('scroll', invalidateOrigin, { signal, capture: true, passive: true });
        window.addEventListener('resize', invalidateOrigin, { signal, passive: true });

        model.disposing.on(() => {
            this.frameHandle.unregister();
            el.shadowRoot!.removeChild(this.node);
        })
    }
}
