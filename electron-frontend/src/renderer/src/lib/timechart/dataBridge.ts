import { ALIGNED_MAX_POINTS, AlignedDataBuffer, type AlignedSeriesData } from './engine/core/alignedData'

export interface DataPoint {
  x: number
  y: number
}

export interface DataSyncResult {
  changed: boolean
  // First source-row index copied into the aligned buffer during this sync.
  // Null means the update only trimmed existing points (or changed nothing).
  syncedFrom: number | null
}

// Reconciles the telemetry store's republished window with one aligned ring.
// X is evaluated and stored once per row; all Y channels share that timeline.
export class TimeChartDataBridge<T> {
  readonly data: AlignedDataBuffer
  readonly series: readonly AlignedSeriesData[]
  private lastX = NaN
  private readonly yScratch: Float64Array

  constructor(
    private readonly getX: (row: T) => number,
    private readonly getYs: readonly ((row: T) => number)[],
  ) {
    this.data = new AlignedDataBuffer(getYs.length)
    this.series = this.data.series
    this.yScratch = new Float64Array(getYs.length)
  }

  get length() { return this.data.length }
  xAt(index: number) { return this.data.xAt(index) }
  yAt(channel: number, index: number) { return this.data.yAt(channel, index) }
  lowerBoundX(value: number, start = 0, end = this.length) {
    return this.data.lowerBoundX(value, start, end)
  }

  sync(rows: readonly T[]): DataSyncResult {
    const n = rows.length
    if (n === 0) {
      if (this.data.length === 0) return { changed: false, syncedFrom: null }
      this.data.clear()
      this.lastX = NaN
      return { changed: true, syncedFrom: null }
    }

    const firstX = this.getX(rows[0])
    const lastRowX = this.getX(rows[n - 1])
    // A larger visible window republishes older rows at the front. The
    // incremental path can append and trim, but it cannot prepend, so rebuild
    // once when the earliest representable source point moves backwards.
    // Compare against the capped source start to avoid repeatedly rebuilding
    // publications larger than the renderer's hard point limit.
    const retainedStart = Math.max(0, n - ALIGNED_MAX_POINTS)
    const retainedFirstX = this.getX(rows[retainedStart])
    const needsBackfill = this.data.length > 0 && retainedFirstX < this.data.firstX
    const contiguous = this.data.length > 0 && !Number.isNaN(this.lastX) &&
      lastRowX >= this.lastX && firstX <= this.lastX && !needsBackfill
    if (!contiguous) {
      const syncedFrom = this.rebuild(rows)
      this.lastX = lastRowX
      return { changed: true, syncedFrom }
    }

    let lo = 0
    let hi = n
    while (lo < hi) {
      const mid = (lo + hi) >> 1
      if (this.getX(rows[mid]) > this.lastX) hi = mid
      else lo = mid + 1
    }
    const appendStart = lo
    for (let i = appendStart; i < n; i++) this.appendRow(rows[i])
    if (appendStart < n) this.lastX = lastRowX

    // Ring eviction advances the logical head and releases vacated pages. It
    // does not reindex every retained point like Array.splice(0, n).
    const trim = this.data.lowerBoundX(firstX)
    if (trim > 0) this.data.evictFront(trim)
    return {
      changed: appendStart < n || trim > 0,
      syncedFrom: appendStart < n ? appendStart : null,
    }
  }

  private appendRow(row: T) {
    for (let channel = 0; channel < this.getYs.length; channel++) {
      this.yScratch[channel] = this.getYs[channel](row)
    }
    this.data.append(this.getX(row), this.yScratch)
  }

  private rebuild(rows: readonly T[]): number {
    this.data.clear()
    // If an input publication exceeds the hard renderer cap, retain its newest
    // samples. Normal 10-minute/60 Hz windows remain within one 65,536 page.
    const start = Math.max(0, rows.length - ALIGNED_MAX_POINTS)
    for (let i = start; i < rows.length; i++) this.appendRow(rows[i])
    return start
  }
}
