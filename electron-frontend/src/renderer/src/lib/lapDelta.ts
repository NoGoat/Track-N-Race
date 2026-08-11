import type { AnalyzeLapData, LapProgressPoint } from '../types'

export interface LapProgressMap {
  points: LapProgressPoint[]
  maxSessionTime: number
  maxDistance: number
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
