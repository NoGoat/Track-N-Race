import type { AnalyzeLapData, LapProgressPoint } from '../types'
import type { ColumnView } from './columnStore'

// A lap's distance/elapsed-time curve as columns: point i is
// (time[i], elapsedMs[i], distance[i]) for i < length. Arrays may be longer
// than `length`; a map from LapProgressBuilder shares them with later maps of
// the same lap, so only read below `length`.
export interface LapProgressMap {
  length: number
  time: Float64Array
  elapsedMs: Float64Array
  distance: Float64Array
  maxSessionTime: number
  maxDistance: number
}

export interface SectorSplit {
  /** Completed sector number; the next sector starts at this position. */
  afterSector: 1 | 2
  distance: number
  elapsedSeconds: number
}

// Race Lap 1 can begin away from the timing-line origin: the starting grid is
// before the line, and every packet received before the session clock starts
// may share the same session_time/current_lap_ms of zero. Preserve the earliest
// recorded distance for that collapsed boundary instead of fabricating
// (time 0, distance 0), which creates a wide vertical/fill wedge when the first
// positive-time sample is already hundreds of metres around the circuit.
function isOriginSample(sessionTime: number, elapsedMs: number, distance: number, start: number): boolean {
  return sessionTime >= start && elapsedMs === 0 && Number.isFinite(distance) && distance >= 0
}

/**
 * Builds a lap's progress map from its lap rows, consuming only the rows
 * appended since the previous update. Any other change to the rows, or to the
 * lap's bounds, rebuilds it, so the result always equals a build from scratch.
 */
export class LapProgressBuilder {
  private source: ColumnView<LapProgressPoint> | null = null
  private start = NaN
  private end = NaN
  private consumed = 0
  private stopped = false
  // Fingerprint of the first and last consumed rows, to detect rewritten rows.
  private firstTime = NaN
  private lastRow = [NaN, NaN, NaN]
  private originRaw = Infinity
  private origin = 0
  private time = new Float64Array(64)
  private elapsedMs = new Float64Array(64)
  private distance = new Float64Array(64)
  private count = 0
  private lastTime = NaN
  private lastDistance = NaN
  private map: LapProgressMap | null = null
  private dirty = false
  private splits = new SectorSplitScan()

  update(rows: ColumnView<LapProgressPoint>, startSessionTime: number, endSessionTime = Infinity): LapProgressMap | null {
    if (!this.extends(rows, startSessionTime, endSessionTime)) this.rebuild(rows, startSessionTime, endSessionTime)
    else if (rows.length > this.consumed && !this.stopped) {
      // Origin samples only occur in the lap-start prefix; one that moves the
      // origin changes the first point, which only a rebuild can apply.
      if (this.newRowsMoveOrigin(rows)) this.rebuild(rows, startSessionTime, endSessionTime)
      else this.ingest(rows, this.consumed)
    }
    this.source = rows
    return this.publish()
  }

  /** The sector splits of the rows last given to update(). */
  sectorSplits(): SectorSplit[] {
    return this.source ? this.splits.scan(this.source, this.publish()) : []
  }

  private extends(rows: ColumnView<LapProgressPoint>, start: number, end: number): boolean {
    if (!this.source || !Object.is(start, this.start) || !Object.is(end, this.end)) return false
    if (rows === this.source) return true
    // Rows past the lap end are not fingerprinted, yet the split scan reads them.
    if (this.stopped || rows.length < this.consumed) return false
    if (this.consumed === 0) return true
    const last = this.consumed - 1
    return Object.is(rows.time(0), this.firstTime) &&
      Object.is(rows.time(last), this.lastRow[0]) &&
      Object.is(rows.num('current_lap_ms', last), this.lastRow[1]) &&
      Object.is(rows.num('lap_distance_m', last), this.lastRow[2])
  }

  private newRowsMoveOrigin(rows: ColumnView<LapProgressPoint>): boolean {
    let originRaw = this.originRaw
    for (let i = this.consumed; i < rows.length; i++) {
      const sessionTime = rows.time(i)
      if (sessionTime > this.end) break
      const distance = rows.num('lap_distance_m', i)
      if (isOriginSample(sessionTime, rows.num('current_lap_ms', i), distance, this.start)) {
        originRaw = Math.min(originRaw, distance)
      }
    }
    return (Number.isFinite(originRaw) ? originRaw : 0) !== this.origin
  }

  private rebuild(rows: ColumnView<LapProgressPoint>, start: number, end: number): void {
    this.start = start
    this.end = end
    this.consumed = 0
    this.stopped = false
    this.firstTime = NaN
    this.lastRow = [NaN, NaN, NaN]
    this.splits.reset()
    this.map = null
    this.dirty = true
    let originRaw = Infinity
    for (let i = 0; i < rows.length; i++) {
      const sessionTime = rows.time(i)
      if (sessionTime > end) break
      const distance = rows.num('lap_distance_m', i)
      if (isOriginSample(sessionTime, rows.num('current_lap_ms', i), distance, start)) {
        originRaw = Math.min(originRaw, distance)
      }
    }
    this.originRaw = originRaw
    this.origin = Number.isFinite(originRaw) ? originRaw : 0
    // New arrays: maps published earlier keep the ones they were built with.
    const capacity = Math.max(64, this.time.length)
    this.time = new Float64Array(capacity)
    this.elapsedMs = new Float64Array(capacity)
    this.distance = new Float64Array(capacity)
    this.time[0] = start
    this.elapsedMs[0] = 0
    this.distance[0] = this.origin
    this.count = 1
    this.lastTime = start
    this.lastDistance = this.origin
    this.ingest(rows, 0)
  }

  private ingest(rows: ColumnView<LapProgressPoint>, from: number): void {
    const start = this.start
    let i = from
    for (; i < rows.length; i++) {
      const sessionTime = rows.time(i)
      if (sessionTime > this.end) { this.stopped = true; break }
      const elapsedMs = rows.num('current_lap_ms', i)
      const distance = rows.num('lap_distance_m', i)
      if (isOriginSample(sessionTime, elapsedMs, distance, start)) this.originRaw = Math.min(this.originRaw, distance)
      if (!Number.isFinite(sessionTime) || !Number.isFinite(distance) ||
          !Number.isFinite(elapsedMs) || sessionTime < this.lastTime ||
          distance < this.lastDistance || elapsedMs < 0) continue
      // The synthetic boundary represents every row at the exact lap-start
      // timestamp. Adding those rows again would create duplicate X coordinates.
      if (sessionTime === start) continue
      if (this.count === 1) {
        if (distance === this.origin) continue
        this.push(sessionTime, elapsedMs, distance)
      } else if (distance === this.lastDistance) {
        // Distance -> elapsed time is defined by the first arrival at a distance.
        // Recorded rows can repeat the exact float distance while time advances.
        continue
      } else if (sessionTime === this.lastTime) {
        const last = this.count - 1
        this.time[last] = sessionTime
        this.elapsedMs[last] = elapsedMs
        this.distance[last] = distance
        this.dirty = true
      } else {
        this.push(sessionTime, elapsedMs, distance)
      }
      this.lastTime = sessionTime
      this.lastDistance = distance
    }
    this.consumed = i
    if (this.consumed > 0) {
      const last = this.consumed - 1
      this.firstTime = rows.time(0)
      this.lastRow = [rows.time(last), rows.num('current_lap_ms', last), rows.num('lap_distance_m', last)]
    }
  }

  private push(sessionTime: number, elapsedMs: number, distance: number): void {
    if (this.count === this.time.length) {
      const grow = (old: Float64Array) => { const next = new Float64Array(old.length * 2); next.set(old); return next }
      this.time = grow(this.time)
      this.elapsedMs = grow(this.elapsedMs)
      this.distance = grow(this.distance)
    }
    this.time[this.count] = sessionTime
    this.elapsedMs[this.count] = elapsedMs
    this.distance[this.count] = distance
    this.count++
    this.dirty = true
  }

  private publish(): LapProgressMap | null {
    if (!this.dirty) return this.map
    this.dirty = false
    this.map = this.count < 2 ? null : {
      length: this.count,
      time: this.time,
      elapsedMs: this.elapsedMs,
      distance: this.distance,
      maxSessionTime: this.lastTime,
      maxDistance: this.lastDistance,
    }
    return this.map
  }
}

export function buildLapProgressMapFromPoints(
  lapProgress: ColumnView<LapProgressPoint>,
  startSessionTime: number,
  endSessionTime = Infinity,
): LapProgressMap | null {
  return new LapProgressBuilder().update(lapProgress, startSessionTime, endSessionTime)
}

// Lap snapshots are immutable, so their maps and splits are computed once.
const lapProgressMaps = new WeakMap<AnalyzeLapData, LapProgressMap | null>()
const lapSectorSplits = new WeakMap<AnalyzeLapData, SectorSplit[]>()

export function buildLapProgressMap(lap: AnalyzeLapData | null): LapProgressMap | null {
  if (!lap) return null
  let map = lapProgressMaps.get(lap)
  if (map === undefined) {
    map = buildLapProgressMapFromPoints(lap.lapProgress, lap.startSessionTime, lap.endSessionTime)
    lapProgressMaps.set(lap, map)
  }
  return map
}

// Index of the first point whose `values` entry is >= target, searching from 1.
function lowerBoundFrom1(values: Float64Array, length: number, target: number): number {
  let lo = 1, hi = length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (values[mid] < target) lo = mid + 1
    else hi = mid
  }
  return lo
}

/**
 * Distance at a session time. Times after the map are NaN; times before its
 * first point are NaN unless `clampBeforeStart`, which gives the origin.
 */
export function interpolateDistanceAtTime(progress: LapProgressMap, sessionTime: number, clampBeforeStart = false): number {
  const { time, distance, length } = progress
  if (sessionTime > progress.maxSessionTime) return NaN
  if (sessionTime < time[0]) return clampBeforeStart ? distance[0] : NaN
  const lo = lowerBoundFrom1(time, length, sessionTime)
  if (lo >= length) return distance[length - 1]
  const span = time[lo] - time[lo - 1]
  const ratio = span > 0 ? (sessionTime - time[lo - 1]) / span : 1
  return distance[lo - 1] + (distance[lo] - distance[lo - 1]) * ratio
}

export function interpolateLapElapsed(progress: LapProgressMap, distance: number): number {
  const { elapsedMs, length } = progress
  const distances = progress.distance
  if (distance < distances[0] || distance > progress.maxDistance) return NaN
  const lo = lowerBoundFrom1(distances, length, distance)
  if (lo >= length) return elapsedMs[length - 1] / 1000
  const span = distances[lo] - distances[lo - 1]
  const ratio = span > 0 ? (distance - distances[lo - 1]) / span : 1
  return (elapsedMs[lo - 1] + (elapsedMs[lo] - elapsedMs[lo - 1]) * ratio) / 1000
}

function interpolateLapDistanceAtElapsed(progress: LapProgressMap, elapsed: number): number {
  const { elapsedMs, distance, length } = progress
  if (elapsed < elapsedMs[0] || elapsed > elapsedMs[length - 1]) return NaN
  const lo = lowerBoundFrom1(elapsedMs, length, elapsed)
  if (lo >= length) return distance[length - 1]
  const span = elapsedMs[lo] - elapsedMs[lo - 1]
  const ratio = span > 0 ? (elapsed - elapsedMs[lo - 1]) / span : 1
  return distance[lo - 1] + (distance[lo] - distance[lo - 1]) * ratio
}

interface FoundSplit { afterSector: 1 | 2; elapsedMs: number }
interface SplitScanState { index: number; entered: boolean; found: FoundSplit[] }

// The S1/S2 boundaries of a lap's rows, located on its progress map. A row
// whose boundary is not on the map yet may be on a longer map later, so an
// incremental scan resumes from the first such row instead of passing it.
class SectorSplitScan {
  private resume: SplitScanState = { index: 0, entered: false, found: [] }

  reset(): void {
    this.resume = { index: 0, entered: false, found: [] }
  }

  scan(rows: ColumnView<LapProgressPoint>, progress: LapProgressMap | null): SectorSplit[] {
    if (!progress) return []
    // Boundaries already found were located on the map as it was; each must
    // still be on it, or the scan from scratch would have passed its row.
    for (const split of this.resume.found) {
      if (!Number.isFinite(interpolateLapDistanceAtElapsed(progress, split.elapsedMs))) { this.reset(); break }
    }
    const state: SplitScanState = { ...this.resume, found: [...this.resume.found] }
    let firstMiss: SplitScanState | null = null
    let i = state.index
    for (; i < rows.length && state.found.length < 2; i++) {
      const sector = rows.num('sector', i)
      if (!Number.isFinite(sector)) continue
      // Race Lap 1 starts behind the timing line. Until the player crosses it,
      // the game reports the grid samples as sector 2 with a negative distance.
      // Do not mistake that preceding-lap state for this lap's S1/S2 boundaries.
      if (!state.entered) {
        if (sector === 0 && rows.num('lap_distance_m', i) >= 0) state.entered = true
        continue
      }
      const afterSector = state.found.length === 0 && sector >= 1 ? 1
        : state.found.length === 1 && sector >= 2 ? 2
        : null
      if (afterSector === null) continue
      const s1 = rows.num('s1_ms', i)
      const s2 = rows.num('s2_ms', i)
      const exactMs = afterSector === 1
        ? s1
        : Number.isFinite(s1) && Number.isFinite(s2) && s1 > 0 && s2 > 0 ? s1 + s2 : NaN
      const elapsedMs = Number.isFinite(exactMs) && exactMs > 0 ? exactMs : rows.num('current_lap_ms', i)
      if (!Number.isFinite(interpolateLapDistanceAtElapsed(progress, elapsedMs))) {
        firstMiss ??= { index: i, entered: state.entered, found: [...state.found] }
        continue
      }
      state.found.push({ afterSector, elapsedMs })
    }
    this.resume = firstMiss ?? { ...state, index: i }
    return state.found.map(split => ({
      afterSector: split.afterSector,
      distance: interpolateLapDistanceAtElapsed(progress, split.elapsedMs),
      elapsedSeconds: split.elapsedMs / 1000,
    }))
  }
}

export function findSectorSplitsFromProgress(
  lapProgress: ColumnView<LapProgressPoint>,
  progress: LapProgressMap | null,
): SectorSplit[] {
  return new SectorSplitScan().scan(lapProgress, progress)
}

export function findSectorSplits(lap: AnalyzeLapData | null): SectorSplit[] {
  if (!lap) return []
  let splits = lapSectorSplits.get(lap)
  if (!splits) {
    splits = findSectorSplitsFromProgress(lap.lapProgress, buildLapProgressMap(lap))
    lapSectorSplits.set(lap, splits)
  }
  return splits
}
