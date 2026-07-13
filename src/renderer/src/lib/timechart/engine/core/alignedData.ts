export const ALIGNED_PAGE_SIZE = 65_536;
export const ALIGNED_MAX_POINTS = 750_000;

const PAGE_COUNT = Math.ceil(ALIGNED_MAX_POINTS / ALIGNED_PAGE_SIZE);
const PHYSICAL_CAPACITY = PAGE_COUNT * ALIGNED_PAGE_SIZE;

export interface DirtySpan {
    start: number;
    count: number;
}

/** Scalar series view over an aligned timeline. No {x, y} objects are stored. */
export interface SeriesData {
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
        private readonly owner: AlignedDataBuffer,
        private readonly channel: number,
    ) {}

    get length() { return this.owner.length; }
    get pushedBack() { return this.owner.pushedBack; }
    get poppedFront() { return this.owner.poppedFront; }
    get resetPending() { return this.owner.resetPending; }
    xAt(index: number) { return this.owner.xAt(index); }
    yAt(index: number) { return this.owner.yAt(this.channel, index); }
    lowerBoundX(value: number, start = 0, end = this.length) {
        return this.owner.lowerBoundX(value, start, end);
    }
    markSynced() { this.owner.markSynced(); }
}

/**
 * Paged, aligned circular telemetry storage. X is written once per sample and
 * shared by every Y channel. Evicting the front advances a logical head; it
 * never shifts or reindexes the retained samples.
 */
export class AlignedDataBuffer {
    readonly series: readonly AlignedSeriesData[];
    private xPages: Array<Float64Array | undefined> = new Array(PAGE_COUNT);
    private yPages: Array<Array<Float32Array | undefined>>;
    private pageCounts = new Uint32Array(PAGE_COUNT);
    private head = 0;
    private size = 0;
    private appended = 0;
    private evicted = 0;
    private needsReset = true;

    constructor(readonly channelCount: number) {
        if (!Number.isInteger(channelCount) || channelCount <= 0) {
            throw new RangeError('AlignedDataBuffer requires at least one Y channel.');
        }
        this.yPages = Array.from({ length: channelCount }, () => new Array(PAGE_COUNT));
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
        return (this.head + logicalIndex) % PHYSICAL_CAPACITY;
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

        const physical = (this.head + this.size) % PHYSICAL_CAPACITY;
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
            physical = (physical + chunk) % PHYSICAL_CAPACITY;
            remaining -= chunk;
        }
        this.head = (this.head + count) % PHYSICAL_CAPACITY;
        this.size -= count;
    }

    clear() {
        this.xPages = new Array(PAGE_COUNT);
        this.yPages = Array.from({ length: this.channelCount }, () => new Array(PAGE_COUNT));
        this.pageCounts = new Uint32Array(PAGE_COUNT);
        this.head = 0;
        this.size = 0;
        this.appended = 0;
        this.evicted = 0;
        this.needsReset = true;
    }

    /** Physical spans appended since the previous GPU synchronization. */
    dirtySpans(): DirtySpan[] {
        if (this.appended === 0) return [];
        const start = (this.head + this.size - this.appended + PHYSICAL_CAPACITY) % PHYSICAL_CAPACITY;
        const firstCount = Math.min(this.appended, PHYSICAL_CAPACITY - start);
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
    return !!data && typeof data.xAt === 'function' && typeof data.yAt === 'function' &&
        typeof data.lowerBoundX === 'function' && typeof data.markSynced === 'function';
}
