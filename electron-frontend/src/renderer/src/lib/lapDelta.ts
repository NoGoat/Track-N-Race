import type { AnalyzeLapData, LapProgressPoint } from '../types'

export interface LapProgressMap {
  points: LapProgressPoint[]
  maxSessionTime: number
  maxDistance: number
}

export interface SectorSplit {
  /** Completed sector number; the next sector starts at this position. */
  afterSector: 1 | 2
  distance: number
  elapsedSeconds: number
}

export function buildLapProgressMapFromPoints(
  lapProgress: readonly LapProgressPoint[],
  startSessionTime: number,
  endSessionTime = Infinity,
): LapProgressMap | null {
  if (lapProgress.length === 0) return null
  // Race Lap 1 can begin away from the timing-line origin: the starting grid
  // is before the line, and every packet received before the session clock
  // starts may share the same session_time/current_lap_ms of zero. Preserve the
  // earliest recorded distance for that collapsed boundary instead of
  // fabricating (time 0, distance 0), which creates a wide vertical/fill wedge
  // when the first positive-time sample is already hundreds of metres around
  // the circuit.
  let originDistance = Infinity
  for (const point of lapProgress) {
    if (point.session_time > endSessionTime) break
    if (point.session_time < startSessionTime || point.current_lap_ms !== 0 ||
        !Number.isFinite(point.lap_distance_m) || point.lap_distance_m < 0) continue
    originDistance = Math.min(originDistance, point.lap_distance_m)
  }
  if (!Number.isFinite(originDistance)) originDistance = 0
  const points: LapProgressPoint[] = [{
    session_time: startSessionTime,
    current_lap_ms: 0,
    lap_distance_m: originDistance,
    sector: 0,
  }]
  let lastTime = startSessionTime
  let lastDistance = originDistance
  for (const point of lapProgress) {
    if (point.session_time > endSessionTime) break
    if (!Number.isFinite(point.session_time) || !Number.isFinite(point.lap_distance_m) ||
        !Number.isFinite(point.current_lap_ms) || point.session_time < lastTime ||
        point.lap_distance_m < lastDistance || point.current_lap_ms < 0) continue
    // The synthetic boundary above represents every row at the exact lap-start
    // timestamp. Adding those rows again would create duplicate X coordinates.
    if (point.session_time === startSessionTime) continue
    if (points.length === 1) {
      if (point.lap_distance_m === originDistance) continue
      points.push(point)
    } else if (point.lap_distance_m === lastDistance) {
      // Distance -> elapsed time is defined by the first arrival at a distance.
      // Recorded rows can repeat the exact float distance while time advances.
      continue
    } else if (point.session_time === lastTime) {
      points[points.length - 1] = point
    } else {
      points.push(point)
    }
    lastTime = point.session_time
    lastDistance = point.lap_distance_m
  }
  if (points.length < 2) return null
  return { points, maxSessionTime: lastTime, maxDistance: lastDistance }
}

export function buildLapProgressMap(lap: AnalyzeLapData | null): LapProgressMap | null {
  return lap
    ? buildLapProgressMapFromPoints(lap.lapProgress, lap.startSessionTime, lap.endSessionTime)
    : null
}

export function interpolateLapElapsed(progress: LapProgressMap, distance: number): number {
  const points = progress.points
  if (distance < points[0].lap_distance_m || distance > progress.maxDistance) return NaN
  let lo = 1, hi = points.length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (points[mid].lap_distance_m < distance) lo = mid + 1
    else hi = mid
  }
  if (lo >= points.length) return points[points.length - 1].current_lap_ms / 1000
  const before = points[lo - 1], after = points[lo]
  const span = after.lap_distance_m - before.lap_distance_m
  const ratio = span > 0 ? (distance - before.lap_distance_m) / span : 1
  return (before.current_lap_ms + (after.current_lap_ms - before.current_lap_ms) * ratio) / 1000
}

function interpolateLapDistanceAtElapsed(progress: LapProgressMap, elapsedMs: number): number {
  const points = progress.points
  if (elapsedMs < points[0].current_lap_ms || elapsedMs > points[points.length - 1].current_lap_ms) return NaN
  let lo = 1, hi = points.length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (points[mid].current_lap_ms < elapsedMs) lo = mid + 1
    else hi = mid
  }
  if (lo >= points.length) return points[points.length - 1].lap_distance_m
  const before = points[lo - 1], after = points[lo]
  const span = after.current_lap_ms - before.current_lap_ms
  const ratio = span > 0 ? (elapsedMs - before.current_lap_ms) / span : 1
  return before.lap_distance_m + (after.lap_distance_m - before.lap_distance_m) * ratio
}

export function findSectorSplitsFromProgress(
  lapProgress: readonly LapProgressPoint[],
  progress: LapProgressMap | null,
): SectorSplit[] {
  if (!progress) return []
  const splits: SectorSplit[] = []
  let enteredFirstSector = false
  for (const point of lapProgress) {
    const sector = point.sector
    if (!Number.isFinite(sector)) continue
    // Race Lap 1 starts behind the timing line. Until the player crosses it,
    // the game reports the grid samples as sector 2 with a negative distance.
    // Do not mistake that preceding-lap state for this lap's S1/S2 boundaries.
    if (!enteredFirstSector) {
      if (sector === 0 && point.lap_distance_m >= 0) enteredFirstSector = true
      continue
    }
    const afterSector = splits.length === 0 && sector! >= 1 ? 1
      : splits.length === 1 && sector! >= 2 ? 2
      : null
    if (afterSector === null) continue
    const exactMs = afterSector === 1
      ? point.s1_ms
      : Number.isFinite(point.s1_ms) && Number.isFinite(point.s2_ms) && point.s1_ms! > 0 && point.s2_ms! > 0
        ? point.s1_ms! + point.s2_ms!
        : undefined
    const elapsedMs = Number.isFinite(exactMs) && exactMs! > 0 ? exactMs! : point.current_lap_ms
    const distance = interpolateLapDistanceAtElapsed(progress, elapsedMs)
    if (!Number.isFinite(distance)) continue
    splits.push({ afterSector, distance, elapsedSeconds: elapsedMs / 1000 })
    if (splits.length === 2) break
  }
  return splits
}

export function findSectorSplits(lap: AnalyzeLapData | null): SectorSplit[] {
  return lap ? findSectorSplitsFromProgress(lap.lapProgress, buildLapProgressMap(lap)) : []
}
