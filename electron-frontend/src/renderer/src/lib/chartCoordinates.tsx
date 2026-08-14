import { createContext, useCallback, useContext, useEffect, useRef } from 'react'
import { useTelemetryStore } from '../stores/telemetryStore'
import type { AnalyzeLapData, LapProgressPoint } from '../types'
import { buildLapProgressMap, buildLapProgressMapFromPoints, interpolateLapElapsed, type LapProgressMap } from './lapDelta'
import type { ChartMode, DistanceChartMode } from '../app/appConfig'
import { playbackDebug } from './playbackDebug'
import { DATA_ROW } from './historyDependencies'

interface ChartCoordinates {
  mode: DistanceChartMode | null
  distanceMode: boolean
  allLapsMode: boolean
  stintLapsMode: boolean
  historyStartTime: number
  historyRevision: string
  comparisonMode: boolean
  trackLengthM: number
  lapRevision: number
  progressRevision: string
  lapData: AnalyzeLapData | null
  getX: (row: { session_time: number }) => number
  getComparisonX: (row: { session_time: number }) => number
  getDeltaAtDistance: (distance: number) => number
  formatX: (x: number) => string
  xTickValues?: (min: number, max: number) => number[]
  axisRevision: string
}

const DEFAULT: ChartCoordinates = {
  mode: null,
  distanceMode: false,
  allLapsMode: false,
  stintLapsMode: false,
  historyStartTime: -Infinity,
  historyRevision: '',
  comparisonMode: false,
  trackLengthM: 0,
  lapRevision: 0,
  progressRevision: '',
  lapData: null,
  getX: row => row.session_time,
  getComparisonX: row => row.session_time,
  getDeltaAtDistance: () => NaN,
  formatX: x => `${Math.floor(x / 60)}:${String(Math.floor(x % 60)).padStart(2, '0')}`,
  axisRevision: '',
}

const Context = createContext(DEFAULT)
function interpolateDistance(points: readonly LapProgressPoint[], sessionTime: number): number {
  if (points.length === 0 || sessionTime > points[points.length - 1].session_time) return NaN
  // Current-lap publications can intentionally include the preceding sparse
  // status row, and binary session times are float32 while cached JSON lap
  // boundaries are rounded decimals. Both belong at the lap origin. Returning
  // NaN here made sync stop at the first row (or put NaN into a chart buffer).
  if (sessionTime <= points[0].session_time) return points[0].lap_distance_m
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

export function ChartCoordinatesProvider({ mode, referenceLapNum, rowTypeMask, children }: { mode: ChartMode | null; referenceLapNum: number | null; rowTypeMask: number; children: React.ReactNode }) {
  const currentProgress = useTelemetryStore(state => state.analyzeLapProgress)
  const currentLapStartTime = useTelemetryStore(state => state.analyzeLapStartTime)
  const trackLengthM = useTelemetryStore(state => state.analyzeTrackLengthM)
  const currentLapRevision = useTelemetryStore(state => state.analyzeLapRevision)
  const currentLapNum = useTelemetryStore(state => state.lap?.lap_num ?? null)
  const isPlayback = useTelemetryStore(state => state.speedRpmBlocks !== null)
  const playbackCache = useTelemetryStore(state => state.playbackLapDataCache)
  const livePreviousLap = useTelemetryStore(state => state.livePreviousLapData)
  const liveFastestLap = useTelemetryStore(state => state.liveFastestLapData)
  const fastestLapNum = useTelemetryStore(state => state.fastestLapNum)
  const liveLapBoundaries = useTelemetryStore(state => state.lapBoundaries)
  const currentStintStartTime = useTelemetryStore(state => state.currentStintStartTime)
  const lapBlocks = useTelemetryStore(state => state.speedRpmBlocks) as Array<{ lapNum: number; startSessionTime: number; endSessionTime: number }> | null
  const playbackCurrentLap = isPlayback && currentLapNum !== null
    ? playbackCache[currentLapNum] ?? null
    : null
  const previousLapNum = currentLapNum !== null && currentLapNum > 1 ? currentLapNum - 1 : null
  const comparisonLapNum = mode === 'PL' ? previousLapNum : mode === 'FL' ? fastestLapNum : mode === 'RL' ? referenceLapNum : null
  const comparisonLapData = mode === 'PL' || mode === 'FL' || mode === 'RL'
    ? isPlayback
      ? comparisonLapNum !== null ? playbackCache[comparisonLapNum] ?? null : null
      : mode === 'PL' ? livePreviousLap : mode === 'FL' ? liveFastestLap : null
    : null
  useEffect(() => {
    if (!isPlayback || (mode !== 'PL' && mode !== 'FL' && mode !== 'RL') || comparisonLapNum === null) return
    const requiredMask = (rowTypeMask | DATA_ROW.lap) >>> 0
    if (((playbackCache[comparisonLapNum]?.rowTypeMask ?? 0) & requiredMask) !== requiredMask) {
      window.playerBridge.getLapData(comparisonLapNum, requiredMask)
    }
  }, [comparisonLapNum, isPlayback, mode, playbackCache, rowTypeMask])

  const enabled = mode !== null && mode !== 'AL' && mode !== 'SL'
  const allLapsMode = mode === 'AL' || mode === 'SL'
  const stintLapsMode = mode === 'SL'
  const comparisonMode = mode === 'PL' || mode === 'FL' || mode === 'RL'
  // Match Analysis: after a playback seek, use the indexed lap's canonical
  // origin/progress instead of mixing seek-backfill progress with cached
  // comparison progress. The latter can differ by one packet (~17 ms).
  const rawProgress = playbackCurrentLap?.lapProgress ?? currentProgress
  const lapStartTime = playbackCurrentLap?.startSessionTime ?? currentLapStartTime
  const lapEndTime = playbackCurrentLap?.endSessionTime ?? Infinity
  const lapRevision = currentLapRevision
  const pointsRef = useRef<readonly LapProgressPoint[]>([])
  const comparisonPointsRef = useRef<readonly LapProgressPoint[]>([])
  const currentProgressMapRef = useRef<LapProgressMap | null>(null)
  const comparisonProgressMapRef = useRef<LapProgressMap | null>(null)
  if (enabled) {
    const progressMap = buildLapProgressMapFromPoints(rawProgress, lapStartTime, lapEndTime)
    currentProgressMapRef.current = progressMap
    pointsRef.current = progressMap?.points ?? [{ session_time: lapStartTime, current_lap_ms: 0, lap_distance_m: 0 }]
  } else {
    pointsRef.current = rawProgress
    currentProgressMapRef.current = null
  }
  if (comparisonMode && comparisonLapData) {
    const progressMap = buildLapProgressMap(comparisonLapData)
    comparisonProgressMapRef.current = progressMap
    comparisonPointsRef.current = progressMap?.points ?? []
  } else {
    comparisonPointsRef.current = []
    comparisonProgressMapRef.current = null
  }
  const getX = useCallback((row: { session_time: number }) => interpolateDistance(pointsRef.current, row.session_time), [])
  const getComparisonX = useCallback((row: { session_time: number }) => interpolateDistance(comparisonPointsRef.current, row.session_time), [])
  const getDeltaAtDistance = useCallback((distance: number) => {
    const current = currentProgressMapRef.current
    const comparison = comparisonProgressMapRef.current
    if (!current || !comparison) return NaN
    const currentElapsed = interpolateLapElapsed(current, distance)
    const comparisonElapsed = interpolateLapElapsed(comparison, distance)
    const delta = currentElapsed - comparisonElapsed
    return Number.isFinite(delta) ? delta : NaN
  }, [])
  const lapBoundaries = isPlayback
    ? (lapBlocks ?? [])
      .map(block => ({ lapNum: block.lapNum, sessionTime: block.startSessionTime }))
      .sort((a, b) => a.sessionTime - b.sessionTime)
    : liveLapBoundaries
  const stintStartTime = stintLapsMode ? currentStintStartTime : -Infinity
  const boundaryLabelsRef = useRef(new Map<number, string>())
  boundaryLabelsRef.current = new Map(lapBoundaries.map(boundary => [boundary.sessionTime, String(boundary.lapNum)]))
  const boundaryValuesRef = useRef<number[]>([])
  boundaryValuesRef.current = lapBoundaries
    .filter(boundary => !stintLapsMode || boundary.sessionTime >= stintStartTime)
    .map(boundary => boundary.sessionTime)
  const getAllLapTicks = useCallback((min: number, max: number) => boundaryValuesRef.current
    .filter(time => time >= min && time <= max), [])
  const formatAllLapX = useCallback((x: number) => boundaryLabelsRef.current.get(x) ?? '', [])
  const lastProgress = rawProgress[rawProgress.length - 1]
  const progressRevision = `${lapRevision}:${rawProgress.length}:${lastProgress?.session_time ?? ''}:${lastProgress?.lap_distance_m ?? ''}`
  useEffect(() => {
    if (!isPlayback || mode === null) return
    playbackDebug('chart-coordinates', {
      mode,
      currentLapNum,
      comparisonLapNum,
      fastestLapNum,
      lapRevision,
      progressRevision,
      currentLapCacheHit: playbackCurrentLap !== null,
      comparisonLapCacheHit: comparisonLapData !== null,
      storeProgressRows: currentProgress.length,
      selectedProgressRows: rawProgress.length,
      lapStartTime,
      lapEndTime,
      firstProgressTime: rawProgress[0]?.session_time ?? null,
      lastProgressTime: lastProgress?.session_time ?? null,
      firstDistance: rawProgress[0]?.lap_distance_m ?? null,
      lastDistance: lastProgress?.lap_distance_m ?? null,
    })
  }, [comparisonLapData, comparisonLapNum, currentLapNum, currentLapRevision, fastestLapNum,
    isPlayback, lapEndTime, lapStartTime, lastProgress, mode, playbackCurrentLap,
    progressRevision, rawProgress, currentProgress.length])
  return <Context.Provider value={{
    mode: enabled ? mode as DistanceChartMode : null,
    distanceMode: enabled,
    allLapsMode,
    stintLapsMode,
    historyStartTime: stintStartTime,
    historyRevision: stintLapsMode ? `SL:${stintStartTime}` : mode === 'AL' ? 'AL' : '',
    comparisonMode,
    trackLengthM,
    lapRevision,
    progressRevision,
    lapData: comparisonLapData,
    getX: enabled ? getX : DEFAULT.getX,
    getComparisonX: comparisonMode ? getComparisonX : DEFAULT.getComparisonX,
    getDeltaAtDistance: comparisonMode ? getDeltaAtDistance : DEFAULT.getDeltaAtDistance,
    formatX: enabled ? formatChartDistance : allLapsMode ? formatAllLapX : DEFAULT.formatX,
    xTickValues: allLapsMode ? getAllLapTicks : undefined,
    axisRevision: allLapsMode
      ? `${mode}:${stintStartTime}:` + lapBoundaries.map(boundary => `${boundary.lapNum}:${boundary.sessionTime}`).join('|')
      : progressRevision,
  }}>{children}</Context.Provider>
}

export function useChartCoordinates(): ChartCoordinates {
  return useContext(Context)
}
