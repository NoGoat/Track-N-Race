export const ALIGNED_PAGE_SIZE = 65_536;
export const ALIGNED_MAX_POINTS = 750_000;

export const ALIGNED_PAGE_COUNT = Math.ceil(ALIGNED_MAX_POINTS / ALIGNED_PAGE_SIZE);
export const ALIGNED_PHYSICAL_CAPACITY = ALIGNED_PAGE_COUNT * ALIGNED_PAGE_SIZE;

export interface DirtySpan {
    start: number;
    count: number;
}

/** Scalar series view over an aligned timeline. No {x, y} objects are stored. */
export interface SeriesData {
    readonly buffer: AlignedDataBuffer;
    readonly channel: number;
    readonly length: number;
    readonly pushedBack: number;
    readonly poppedFront: number;
    readonly resetPending: boolean;
    xAt(index: number): number;
    yAt(index: number): number;
    lowerBoundX(value: number, start?: number, end?: number): number;
    markSynced(): void;
}

export class AlignedSeriesData implements SeriesData {
    constructor(
        readonly buffer: AlignedDataBuffer,
        readonly channel: number,
    ) {}

    get length() { return this.buffer.length; }
    get pushedBack() { return this.buffer.pushedBack; }
    get poppedFront() { return this.buffer.poppedFront; }
    get resetPending() { return this.buffer.resetPending; }
    xAt(index: number) { return this.buffer.xAt(index); }
    yAt(index: number) { return this.buffer.yAt(this.channel, index); }
    lowerBoundX(value: number, start = 0, end = this.length) {
        return this.buffer.lowerBoundX(value, start, end);
    }
    markSynced() { this.buffer.markSynced(); }
}

/**
 * Paged, aligned circular telemetry storage. X is written once per sample and
 * shared by every Y channel. Evicting the front advances a logical head; it
 * never shifts or reindexes the retained samples.
 */
export class AlignedDataBuffer {
    readonly series: readonly AlignedSeriesData[];
    private xPages: Array<Float64Array | undefined> = new Array(ALIGNED_PAGE_COUNT);
    private yPages: Array<Array<Float32Array | undefined>>;
    private pageCounts = new Uint32Array(ALIGNED_PAGE_COUNT);
    private head = 0;
    private size = 0;
    private appended = 0;
    private evicted = 0;
    private needsReset = true;

    constructor(readonly channelCount: number) {
        if (!Number.isInteger(channelCount) || channelCount <= 0) {
            throw new RangeError('AlignedDataBuffer requires at least one Y channel.');
        }
        this.yPages = Array.from({ length: channelCount }, () => new Array(ALIGNED_PAGE_COUNT));
        this.series = Array.from({ length: channelCount }, (_, channel) => new AlignedSeriesData(this, channel));
    }

    get length() { return this.size; }
    get pushedBack() { return this.appended; }
    get poppedFront() { return this.evicted; }
    get resetPending() { return this.needsReset; }
    get firstX() { return this.size === 0 ? NaN : this.xAt(0); }
    get lastX() { return this.size === 0 ? NaN : this.xAt(this.size - 1); }

    private physicalIndex(logicalIndex: number) {
        if (logicalIndex < 0 || logicalIndex >= this.size) {
            throw new RangeError(`Aligned data index ${logicalIndex} outside [0, ${this.size}).`);
        }
        return (this.head + logicalIndex) % ALIGNED_PHYSICAL_CAPACITY;
    }

    private ensurePage(page: number) {
        let x = this.xPages[page];
        if (!x) {
            x = new Float64Array(ALIGNED_PAGE_SIZE);
            this.xPages[page] = x;
            for (let channel = 0; channel < this.channelCount; channel++) {
                this.yPages[channel][page] = new Float32Array(ALIGNED_PAGE_SIZE);
            }
        }
        return x;
    }

    xAt(index: number) {
        const physical = this.physicalIndex(index);
        return this.xPages[Math.floor(physical / ALIGNED_PAGE_SIZE)]![physical % ALIGNED_PAGE_SIZE];
    }

    yAt(channel: number, index: number) {
        if (channel < 0 || channel >= this.channelCount) throw new RangeError(`Invalid Y channel ${channel}.`);
        const physical = this.physicalIndex(index);
        return this.yPages[channel][Math.floor(physical / ALIGNED_PAGE_SIZE)]![physical % ALIGNED_PAGE_SIZE];
    }

    /** Physical ring position for a logical sample, used by the GPU pager. */
    physicalIndexAt(index: number) {
        return this.physicalIndex(index);
    }

    /** Logical index for an occupied physical slot, or -1 for the ring gap. */
    logicalIndexForPhysical(physicalIndex: number) {
        if (physicalIndex < 0 || physicalIndex >= ALIGNED_PHYSICAL_CAPACITY) return -1;
        const logical = (physicalIndex - this.head + ALIGNED_PHYSICAL_CAPACITY) % ALIGNED_PHYSICAL_CAPACITY;
        return logical < this.size ? logical : -1;
    }

    /** Read-only page access for allocation-free GPU uploads. */
    xPage(page: number) { return this.xPages[page]; }
    yPage(channel: number, page: number) { return this.yPages[channel]?.[page]; }
    hasPage(page: number) { return this.pageCounts[page] !== 0; }

    lowerBoundX(value: number, start = 0, end = this.size) {
        start = Math.max(0, start);
        end = Math.min(this.size, end);
        while (start < end) {
            const mid = (start + end) >> 1;
            if (this.xAt(mid) < value) start = mid + 1;
            else end = mid;
        }
        return start;
    }

    append(x: number, ys: ArrayLike<number>) {
        if (ys.length !== this.channelCount) {
            throw new RangeError(`Expected ${this.channelCount} Y values, received ${ys.length}.`);
        }
        if (this.size === ALIGNED_MAX_POINTS) this.evictFront(1);

        const physical = (this.head + this.size) % ALIGNED_PHYSICAL_CAPACITY;
        const page = Math.floor(physical / ALIGNED_PAGE_SIZE);
        const offset = physical % ALIGNED_PAGE_SIZE;
        this.ensurePage(page)[offset] = x;
        for (let channel = 0; channel < this.channelCount; channel++) {
            this.yPages[channel][page]![offset] = ys[channel];
        }
        this.pageCounts[page]++;
        this.size++;
        this.appended++;
    }

    /** Replace the newest sample in place and include it in the next GPU upload. */
    replaceLast(ys: ArrayLike<number>) {
        if (ys.length !== this.channelCount) {
            throw new RangeError(`Expected ${this.channelCount} Y values, received ${ys.length}.`);
        }
        if (this.size === 0) throw new RangeError('Cannot replace a sample in an empty aligned data buffer.');

        const physical = (this.head + this.size - 1) % ALIGNED_PHYSICAL_CAPACITY;
        const page = Math.floor(physical / ALIGNED_PAGE_SIZE);
        const offset = physical % ALIGNED_PAGE_SIZE;
        for (let channel = 0; channel < this.channelCount; channel++) {
            this.yPages[channel][page]![offset] = ys[channel];
        }
        // dirtySpans represents one contiguous dirty tail. Marking the final
        // point dirty works whether it was already synchronized or appended in
        // the current frame.
        this.appended = Math.max(this.appended, 1);
    }

    /** Rewrite one complete Y channel without changing the timeline or length. */
    replaceChannel(channel: number, values: ArrayLike<number>) {
        if (channel < 0 || channel >= this.channelCount) throw new RangeError(`Invalid Y channel ${channel}.`);
        if (values.length !== this.size) throw new RangeError(`Expected ${this.size} values, received ${values.length}.`);
        for (let index = 0; index < this.size; index++) {
            const physical = this.physicalIndex(index);
            const page = Math.floor(physical / ALIGNED_PAGE_SIZE);
            const offset = physical % ALIGNED_PAGE_SIZE;
            this.yPages[channel][page]![offset] = values[index];
        }
        // Retain the occupied timeline while asking the GPU pager to upload the
        // rewritten values as one complete snapshot on the next draw.
        this.appended = this.size;
        this.evicted = 0;
        this.needsReset = true;
    }

    evictFront(count: number) {
        count = Math.min(Math.max(Math.trunc(count), 0), this.size);
        if (count === 0) return;

        const stable = Math.max(0, this.size - this.appended);
        const stableRemoved = Math.min(count, stable);
        this.evicted += stableRemoved;
        this.appended -= count - stableRemoved;

        let remaining = count;
        let physical = this.head;
        while (remaining > 0) {
            const page = Math.floor(physical / ALIGNED_PAGE_SIZE);
            const offset = physical % ALIGNED_PAGE_SIZE;
            const chunk = Math.min(remaining, ALIGNED_PAGE_SIZE - offset);
            this.pageCounts[page] -= chunk;
            if (this.pageCounts[page] === 0) {
                this.xPages[page] = undefined;
                for (let channel = 0; channel < this.channelCount; channel++) {
                    this.yPages[channel][page] = undefined;
                }
            }
            physical = (physical + chunk) % ALIGNED_PHYSICAL_CAPACITY;
            remaining -= chunk;
        }
        this.head = (this.head + count) % ALIGNED_PHYSICAL_CAPACITY;
        this.size -= count;
    }

    clear() {
        this.xPages = new Array(ALIGNED_PAGE_COUNT);
        this.yPages = Array.from({ length: this.channelCount }, () => new Array(ALIGNED_PAGE_COUNT));
        this.pageCounts = new Uint32Array(ALIGNED_PAGE_COUNT);
        this.head = 0;
        this.size = 0;
        this.appended = 0;
        this.evicted = 0;
        this.needsReset = true;
    }

    /** Physical spans appended since the previous GPU synchronization. */
    dirtySpans(): DirtySpan[] {
        if (this.appended === 0) return [];
        const start = (this.head + this.size - this.appended + ALIGNED_PHYSICAL_CAPACITY) % ALIGNED_PHYSICAL_CAPACITY;
        const firstCount = Math.min(this.appended, ALIGNED_PHYSICAL_CAPACITY - start);
        return firstCount === this.appended
            ? [{ start, count: firstCount }]
            : [{ start, count: firstCount }, { start: 0, count: this.appended - firstCount }];
    }

    markSynced() {
        this.appended = 0;
        this.evicted = 0;
        this.needsReset = false;
    }
}

export function isSeriesData(value: unknown): value is SeriesData {
    const data = value as Partial<SeriesData> | null;
    return !!data && data.buffer instanceof AlignedDataBuffer && Number.isInteger(data.channel) &&
        typeof data.xAt === 'function' && typeof data.yAt === 'function' &&
        typeof data.lowerBoundX === 'function' && typeof data.markSynced === 'function';
}
