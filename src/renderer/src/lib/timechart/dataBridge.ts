import { ALIGNED_MAX_POINTS, AlignedDataBuffer, type AlignedSeriesData } from './engine/core/alignedData'

export interface DataPoint {
  x: number
  y: number
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

  sync(rows: readonly T[]): boolean {
    const n = rows.length
    if (n === 0) {
      if (this.data.length === 0) return false
      this.data.clear()
      this.lastX = NaN
      return true
    }

    const firstX = this.getX(rows[0])
    const lastRowX = this.getX(rows[n - 1])
    const contiguous = this.data.length > 0 && !Number.isNaN(this.lastX) &&
      lastRowX >= this.lastX && firstX <= this.lastX
    if (!contiguous) {
      this.rebuild(rows)
      this.lastX = lastRowX
      return true
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
    return appendStart < n || trim > 0
  }

  private appendRow(row: T) {
    for (let channel = 0; channel < this.getYs.length; channel++) {
      this.yScratch[channel] = this.getYs[channel](row)
    }
    this.data.append(this.getX(row), this.yScratch)
  }

  private rebuild(rows: readonly T[]) {
    this.data.clear()
    // If an input publication exceeds the hard renderer cap, retain its newest
    // samples. Normal 10-minute/60 Hz windows remain within one 65,536 page.
    const start = Math.max(0, rows.length - ALIGNED_MAX_POINTS)
    for (let i = start; i < rows.length; i++) this.appendRow(rows[i])
  }
}
