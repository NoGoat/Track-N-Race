// Bridges the telemetry store's re-published windowed row slices into the
// mutation model TimeChart requires. uPlot let us reassign the whole
// `AlignedData` every frame; TimeChart syncs series data to the GPU and after
// that will only accept mutations at the *ends* of the buffer (push/pop at the
// back, shift/unshift/splice at the front) — the DataPointsBuffer tracks those
// deltas (pushed_back / poped_front / …) so the renderer can sync incrementally.
//
// The store hands us the latest window each publication (identity changes every
// time, content overlaps the previous window as it slides). We diff against the
// last x we pushed: append genuinely-new tail points, trim points that fell off
// the front of the window, and — when the incoming window is not a forward-
// contiguous continuation (playback seek / flush / session restart) — rebuild
// the whole buffer from scratch. Only end mutations are ever issued, so the
// GPU-sync contract holds.

export interface DataPoint {
  x: number
  y: number
}

// The series `data` arrays handed to TimeChart are DataPointsBuffer instances
// (an Array subclass); we only ever touch the standard Array surface here, and
// its overridden push/shift/splice keep the GPU-sync bookkeeping correct.
type Buffer = DataPoint[]

// First index i where getX(rows[i]) > x, assuming rows is sorted ascending by
// getX. Returns rows.length if none. Strictly-greater so an equal trailing x is
// not re-pushed.
function firstIndexAfter<T>(rows: readonly T[], x: number, getX: (r: T) => number): number {
  let lo = 0
  let hi = rows.length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (getX(rows[mid]) > x) hi = mid
    else lo = mid + 1
  }
  return lo
}

export class TimeChartDataBridge<T> {
  private lastX = NaN

  constructor(
    private readonly buffers: Buffer[],
    private readonly getX: (row: T) => number,
    private readonly getYs: ((row: T) => number)[],
  ) {}

  /** The x-sorted point buffer (series 0), for cursor nearest-index search. */
  get xBuffer(): readonly DataPoint[] {
    return this.buffers[0]
  }

  /**
   * Reconcile the buffers with the latest window. Returns true if any buffer
   * changed (i.e. a redraw is warranted).
   */
  sync(rows: readonly T[]): boolean {
    const n = rows.length
    if (n === 0) {
      if (this.buffers[0].length > 0) {
        this.rebuild(rows)
        this.lastX = NaN
        return true
      }
      return false
    }

    const firstX = this.getX(rows[0])
    const lastRowX = this.getX(rows[n - 1])
    const haveData = this.buffers[0].length > 0 && !Number.isNaN(this.lastX)

    // Contiguous forward continuation: the new window still overlaps our tail
    // (firstX <= lastX) and does not go backwards (lastRowX >= lastX).
    const contiguous = haveData && lastRowX >= this.lastX && firstX <= this.lastX
    if (!contiguous) {
      this.rebuild(rows)
      this.lastX = lastRowX
      return true
    }

    let changed = false

    // Append the genuinely-new tail points.
    const k = firstIndexAfter(rows, this.lastX, this.getX)
    if (k < n) {
      for (let s = 0; s < this.buffers.length; s++) {
        const buf = this.buffers[s]
        const getY = this.getYs[s]
        for (let i = k; i < n; i++) buf.push({ x: this.getX(rows[i]), y: getY(rows[i]) })
      }
      this.lastX = lastRowX
      changed = true
    }

    // Trim points that fell off the front of the window (older than firstX).
    const buf0 = this.buffers[0]
    let trim = 0
    while (trim < buf0.length && buf0[trim].x < firstX) trim++
    if (trim > 0) {
      for (const buf of this.buffers) buf.splice(0, trim)
      changed = true
    }

    return changed
  }

  private rebuild(rows: readonly T[]): void {
    for (let s = 0; s < this.buffers.length; s++) {
      const buf = this.buffers[s]
      if (buf.length > 0) buf.splice(0, buf.length)
      const getY = this.getYs[s]
      for (let i = 0; i < rows.length; i++) buf.push({ x: this.getX(rows[i]), y: getY(rows[i]) })
    }
  }
}
