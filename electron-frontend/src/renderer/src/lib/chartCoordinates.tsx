import { createContext, useCallback, useContext, useEffect, useRef } from 'react'
import { useTelemetryStore } from '../stores/telemetryStore'
import type { AnalyzeLapData, LapProgressPoint } from '../types'

interface ChartCoordinates {
  mode: 'CL' | 'PL' | null
  distanceMode: boolean
  trackLengthM: number
  lapRevision: number
  progressRevision: string
  lapData: AnalyzeLapData | null
  getX: (row: { session_time: number }) => number
  getComparisonX: (row: { session_time: number }) => number
  formatX: (x: number) => string
}

const DEFAULT: ChartCoordinates = {
  mode: null,
  distanceMode: false,
  trackLengthM: 0,
  lapRevision: 0,
  progressRevision: '',
  lapData: null,
  getX: row => row.session_time,
  getComparisonX: row => row.session_time,
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

export function ChartCoordinatesProvider({ mode, children }: { mode: 'CL' | 'PL' | null; children: React.ReactNode }) {
  const currentProgress = useTelemetryStore(state => state.analyzeLapProgress)
  const currentLapStartTime = useTelemetryStore(state => state.analyzeLapStartTime)
  const trackLengthM = useTelemetryStore(state => state.analyzeTrackLengthM)
  const currentLapRevision = useTelemetryStore(state => state.analyzeLapRevision)
  const currentLapNum = useTelemetryStore(state => state.lap?.lap_num ?? null)
  const isPlayback = useTelemetryStore(state => state.speedRpmBlocks !== null)
  const playbackCache = useTelemetryStore(state => state.playbackLapDataCache)
  const livePreviousLap = useTelemetryStore(state => state.livePreviousLapData)
  const previousLapNum = currentLapNum !== null && currentLapNum > 1 ? currentLapNum - 1 : null
  const previousLapData = mode === 'PL'
    ? isPlayback
      ? previousLapNum !== null ? playbackCache[previousLapNum] ?? null : null
      : livePreviousLap
    : null
  useEffect(() => {
    if (mode === 'PL' && isPlayback && previousLapNum !== null && !playbackCache[previousLapNum]) {
      window.playerBridge.getLapData(previousLapNum)
    }
  }, [isPlayback, mode, playbackCache, previousLapNum])

  const enabled = mode !== null
  const rawProgress = currentProgress
  const lapStartTime = currentLapStartTime
  const lapRevision = currentLapRevision
  const pointsRef = useRef<readonly LapProgressPoint[]>([])
  const comparisonPointsRef = useRef<readonly LapProgressPoint[]>([])
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
  if (mode === 'PL' && previousLapData) {
    const points: LapProgressPoint[] = [{ session_time: previousLapData.startSessionTime, current_lap_ms: 0, lap_distance_m: 0 }]
    let lastTime = previousLapData.startSessionTime
    let lastDistance = 0
    for (const point of previousLapData.lapProgress) {
      if (!Number.isFinite(point.session_time) || !Number.isFinite(point.lap_distance_m) ||
          point.session_time < lastTime || point.lap_distance_m <= lastDistance || point.current_lap_ms < 0) continue
      points.push(point)
      lastTime = point.session_time
      lastDistance = point.lap_distance_m
    }
    comparisonPointsRef.current = points
  } else comparisonPointsRef.current = []
  const getX = useCallback((row: { session_time: number }) => interpolateDistance(pointsRef.current, row.session_time), [])
  const getComparisonX = useCallback((row: { session_time: number }) => interpolateDistance(comparisonPointsRef.current, row.session_time), [])
  const lastProgress = rawProgress[rawProgress.length - 1]
  const progressRevision = `${lapRevision}:${rawProgress.length}:${lastProgress?.session_time ?? ''}:${lastProgress?.lap_distance_m ?? ''}`
  return <Context.Provider value={{
    mode,
    distanceMode: enabled,
    trackLengthM,
    lapRevision,
    progressRevision,
    lapData: previousLapData,
    getX: enabled ? getX : DEFAULT.getX,
    getComparisonX: mode === 'PL' ? getComparisonX : DEFAULT.getComparisonX,
    formatX: enabled ? formatChartDistance : DEFAULT.formatX,
  }}>{children}</Context.Provider>
}

export function useChartCoordinates(): ChartCoordinates {
  return useContext(Context)
}
