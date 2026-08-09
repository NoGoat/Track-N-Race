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
  const points: LapProgressPoint[] = [{
    session_time: startSessionTime,
    current_lap_ms: 0,
    lap_distance_m: 0,
  }]
  let lastTime = startSessionTime
  let lastDistance = 0
  for (const point of lapProgress) {
    if (point.session_time > endSessionTime) break
    if (!Number.isFinite(point.session_time) || !Number.isFinite(point.lap_distance_m) ||
        !Number.isFinite(point.current_lap_ms) || point.session_time < lastTime ||
        point.lap_distance_m < lastDistance || point.current_lap_ms < 0) continue
    // The zero-distance/zero-time point is the mathematical lap origin. Keep it
    // even when the first recorded packet has the same timestamp: playback's
    // native seek boundary and the indexed JSON row can differ by sub-microsecond
    // float representation, but that must not change the resulting progress map.
    if (points.length === 1) {
      if (point.lap_distance_m === 0) continue
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
  if (distance < 0 || distance > progress.maxDistance) return NaN
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
