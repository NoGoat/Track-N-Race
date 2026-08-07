import { createContext, useCallback, useContext, useRef } from 'react'
import { useTelemetryStore } from '../stores/telemetryStore'
import type { LapProgressPoint } from '../types'

interface ChartCoordinates {
  distanceMode: boolean
  trackLengthM: number
  lapRevision: number
  progressRevision: string
  getX: (row: { session_time: number }) => number
  formatX: (x: number) => string
}

const DEFAULT: ChartCoordinates = {
  distanceMode: false,
  trackLengthM: 0,
  lapRevision: 0,
  progressRevision: '',
  getX: row => row.session_time,
  formatX: x => `${Math.floor(x / 60)}:${String(Math.floor(x % 60)).padStart(2, '0')}`,
}

const Context = createContext(DEFAULT)

function interpolateDistance(points: readonly LapProgressPoint[], sessionTime: number): number {
  if (points.length === 0 || sessionTime < points[0].session_time || sessionTime > points[points.length - 1].session_time) return NaN
  let lo = 1, hi = points.length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (points[mid].session_time < sessionTime) lo = mid + 1
    else hi = mid
  }
  if (lo >= points.length) return points[points.length - 1].lap_distance_m
  const before = points[lo - 1], after = points[lo]
  const span = after.session_time - before.session_time
  const ratio = span > 0 ? (sessionTime - before.session_time) / span : 1
  return before.lap_distance_m + (after.lap_distance_m - before.lap_distance_m) * ratio
}

export function formatChartDistance(metres: number): string {
  return `${Math.round(metres)} m`
}

export function ChartCoordinatesProvider({ enabled, children }: { enabled: boolean; children: React.ReactNode }) {
  const rawProgress = useTelemetryStore(state => state.analyzeLapProgress)
  const lapStartTime = useTelemetryStore(state => state.analyzeLapStartTime)
  const trackLengthM = useTelemetryStore(state => state.analyzeTrackLengthM)
  const lapRevision = useTelemetryStore(state => state.analyzeLapRevision)
  const pointsRef = useRef<readonly LapProgressPoint[]>([])
  if (enabled) {
    const points: LapProgressPoint[] = [{ session_time: lapStartTime, current_lap_ms: 0, lap_distance_m: 0 }]
    let lastTime = lapStartTime
    let lastDistance = 0
    for (const point of rawProgress) {
      if (!Number.isFinite(point.session_time) || !Number.isFinite(point.lap_distance_m) ||
          point.session_time < lastTime || point.lap_distance_m < lastDistance || point.current_lap_ms < 0) continue
      if (point.session_time === lastTime && point.lap_distance_m === lastDistance) continue
      if (point.lap_distance_m === lastDistance) continue
      points.push(point)
      lastTime = point.session_time
      lastDistance = point.lap_distance_m
    }
    pointsRef.current = points
  } else pointsRef.current = rawProgress
  const getX = useCallback((row: { session_time: number }) => interpolateDistance(pointsRef.current, row.session_time), [])
  const lastProgress = rawProgress[rawProgress.length - 1]
  const progressRevision = `${lapRevision}:${rawProgress.length}:${lastProgress?.session_time ?? ''}:${lastProgress?.lap_distance_m ?? ''}`
  return <Context.Provider value={{
    distanceMode: enabled,
    trackLengthM,
    lapRevision,
    progressRevision,
    getX: enabled ? getX : DEFAULT.getX,
    formatX: enabled ? formatChartDistance : DEFAULT.formatX,
  }}>{children}</Context.Provider>
}

export function useChartCoordinates(): ChartCoordinates {
  return useContext(Context)
}
