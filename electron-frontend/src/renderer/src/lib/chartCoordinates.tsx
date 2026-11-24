import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react'
import { requestStoredLiveLaps, useTelemetryStore } from '../stores/telemetryStore'
import type { AnalyzeLapData, PlaybackLapBlock } from '../types'
import { buildLapProgressMap, findSectorSplits, interpolateDistanceAtTime, interpolateLapElapsed, LapProgressBuilder, type LapProgressMap, type SectorSplit } from './lapDelta'
import type { ChartMode, DistanceChartMode } from '../app/appConfig'
import { playbackDebug } from './playbackDebug'
import { DATA_ROW } from './historyDependencies'
import { isSecondaryReferenceLap, useSecondaryBackground, useSecondaryComparisonLap, useSecondaryLapData } from './secondaryComparison'
import { concatAfter, type ColumnView } from './columnStore'

interface ChartCoordinates {
  mode: DistanceChartMode | null
  distanceMode: boolean
  allLapsMode: boolean
  stintLapsMode: boolean
  historyStartTime: number
  historyRevision: string
  comparisonMode: boolean
  /** Names the comparison trace when no window mode does (file B's background). */
  comparisonLabel: string | null
  trackLengthM: number
  lapRevision: number
  progressRevision: string
  lapData: AnalyzeLapData | null
  /** Chart X for a sample at this session time (distance in distance modes). */
  getX: (sessionTime: number) => number
  getComparisonX: (sessionTime: number) => number
  getDeltaAtDistance: (distance: number) => number
  formatX: (x: number) => string
  xTickValues?: (min: number, max: number) => number[]
  cullXTickLabels: boolean
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
  comparisonLabel: null,
  trackLengthM: 0,
  lapRevision: 0,
  progressRevision: '',
  lapData: null,
  getX: sessionTime => sessionTime,
  getComparisonX: sessionTime => sessionTime,
  getDeltaAtDistance: () => NaN,
  formatX: x => `${Math.floor(x / 60)}:${String(Math.floor(x % 60)).padStart(2, '0')}`,
  cullXTickLabels: true,
  axisRevision: '',
}

const Context = createContext(DEFAULT)
/** Charts with their own complete dataset keep their own axes. */
export function LocalChartCoordinatesProvider({ children, comparisonLabel }: { children: React.ReactNode; comparisonLabel?: string | null }) {
  // With a label, the chart's comparison rows (file B behind file A) sit on the
  // same axis as its own rows: getComparisonX stays the identity.
  const value = useMemo(() => comparisonLabel ? { ...DEFAULT, comparisonMode: true, comparisonLabel } : DEFAULT, [comparisonLabel])
  return <Context.Provider value={value}>{children}</Context.Provider>
}
// Current-lap publications can intentionally include the preceding sparse
// status row, and binary session times are float32 while cached JSON lap
// boundaries are rounded decimals. Both belong at the lap origin, so times
// before the map clamp to it. Returning NaN there made sync stop at the first
// row (or put NaN into a chart buffer). Without a map, a lap with a known start
// is only that start point, at distance 0.
function interpolateDistance(progress: LapProgressMap | null, lapStartTime: number | null, sessionTime: number): number {
  if (progress) return interpolateDistanceAtTime(progress, sessionTime, true)
  return lapStartTime === null || sessionTime > lapStartTime ? NaN : 0
}

export function formatChartDistance(metres: number): string {
  return `${Math.round(metres)} m`
}

export type LapBoundary = { lapNum: number; sessionTime: number }

/** Lap start times of the streamed driver, as the chart axes label them. */
export function useLapBoundaries(allLapsMode: boolean): readonly LapBoundary[] {
  const isPlayback = useTelemetryStore(state => state.speedRpmBlocks !== null)
  const lapBlocks = useTelemetryStore(state => state.speedRpmBlocks) as PlaybackLapBlock[] | null
  const liveLapBoundaries = useTelemetryStore(state => state.lapBoundaries)
  const allLapsLapBoundaries = useTelemetryStore(state => state.allLapsLapBoundaries)
  return useMemo(() => isPlayback
    ? (lapBlocks ?? [])
      .map(block => ({ lapNum: block.lapNum, sessionTime: block.startSessionTime }))
      .sort((a, b) => a.sessionTime - b.sessionTime)
    : allLapsMode && allLapsLapBoundaries.length > 0
      ? allLapsLapBoundaries
      : liveLapBoundaries,
  [allLapsLapBoundaries, allLapsMode, isPlayback, lapBlocks, liveLapBoundaries])
}

/** Session time at which `lapNum` began, using the latest attempt of a reused lap number. */
export function lapStartSessionTime(boundaries: readonly LapBoundary[], lapNum: number): number | undefined {
  for (let i = boundaries.length - 1; i >= 0; i--)
    if (boundaries[i].lapNum === lapNum) return boundaries[i].sessionTime
  return undefined
}

/**
 * `stintStartLap` replaces the store's tyre-change heuristic as the Stint Laps
 * origin when the caller knows the stint from Session History.
 */
export function ChartCoordinatesProvider({ mode, referenceLapNum, rowTypeMask, sectorBoundaries, children, stintStartLap, lapBoundaries: lapBoundariesOverride, comparison: comparisonOverride }: {
  mode: ChartMode | null; referenceLapNum: number | null; rowTypeMask: number; sectorBoundaries: boolean; children: React.ReactNode; stintStartLap?: number
  /** Lap starts for rows that are not the store's (file B's tyre history); default: the store's. */
  lapBoundaries?: ReturnType<typeof useLapBoundaries>
  /** A comparison the window mode does not supply (file B behind A's tyre history), mapped onto this axis. */
  comparison?: { lapData: AnalyzeLapData; getComparisonX: (sessionTime: number) => number; label: string } | null
}) {
  const currentProgress = useTelemetryStore(state => state.analyzeLapProgress)
  const currentLapStartTime = useTelemetryStore(state => state.analyzeLapStartTime)
  const trackLengthM = useTelemetryStore(state => state.analyzeTrackLengthM)
  const currentLapRevision = useTelemetryStore(state => state.analyzeLapRevision)
  const historyReplacementRevision = useTelemetryStore(state => state.historyReplacementRevision)
  const currentLapNum = useTelemetryStore(state => state.lap?.lap_num ?? null)
  const isPlayback = useTelemetryStore(state => state.speedRpmBlocks !== null)
  const playbackCache = useTelemetryStore(state => state.playbackLapDataCache)
  const livePreviousLap = useTelemetryStore(state => state.livePreviousLapData)
  const liveFastestLap = useTelemetryStore(state => state.liveFastestLapData)
  const liveSectorSplits = useTelemetryStore(state => state.liveSectorSplits)
  const fastestLapNum = useTelemetryStore(state => state.fastestLapNum)
  const currentStintStartTime = useTelemetryStore(state => state.currentStintStartTime)
  const lapBlocks = useTelemetryStore(state => state.speedRpmBlocks) as PlaybackLapBlock[] | null
  const playbackCurrentLap = isPlayback && currentLapNum !== null
    ? playbackCache[currentLapNum] ?? null
    : null
  const previousLapNum = currentLapNum !== null && currentLapNum > 1 ? currentLapNum - 1 : null
  // A reference lap from file B (picked in the lap selector) is fetched from B;
  // it has no lap number in A, so A's own lap fetch below stays idle.
  const secondaryReferenceLapData = useSecondaryComparisonLap(mode === 'RL' ? referenceLapNum : null, rowTypeMask)
  const referenceFromSecondary = mode === 'RL' && isSecondaryReferenceLap(referenceLapNum)
  // Side-by-side off with file B open: Current Lap and the time windows have
  // no comparison of their own, so B's lap with A's lap number plays behind
  // A's. Current Lap lines it up by lap distance; a time window by time into
  // the lap, from each lap's start.
  const secondaryBackground = useSecondaryBackground()
  const backgroundMode = secondaryBackground.enabled && (mode === null || mode === 'CL')
    ? mode === 'CL' ? 'distance' as const : 'time' as const
    : null
  const backgroundLapNum = backgroundMode && currentLapNum !== null ? currentLapNum : null
  const backgroundLap = useSecondaryLapData(backgroundLapNum, rowTypeMask)
  // A time window can reach back across the lap line: B's previous lap too.
  const backgroundPreviousLap = useSecondaryLapData(
    backgroundMode === 'time' && backgroundLapNum !== null && backgroundLapNum > 1 ? backgroundLapNum - 1 : null, rowTypeMask)
  const comparisonLapNum = mode === 'PL' ? previousLapNum : mode === 'FL' ? fastestLapNum : mode === 'RL' && !referenceFromSecondary ? referenceLapNum : null
  const comparisonLapData = referenceFromSecondary ? secondaryReferenceLapData : backgroundMode === 'distance' ? backgroundLap : mode === 'PL' || mode === 'FL' || mode === 'RL'
    ? isPlayback
      ? comparisonLapNum !== null ? playbackCache[comparisonLapNum] ?? null : null
      : mode === 'PL' ? livePreviousLap : mode === 'FL' ? liveFastestLap : null
    : null
  // A request made while the seek barrier is being established may complete
  // against the old delivery generation. Recheck after every authoritative
  // lap revision so a same-lap seek cannot leave a missing comparison cached
  // as an apparently stable state.
  useEffect(() => {
    if (!isPlayback || (mode !== 'PL' && mode !== 'FL' && mode !== 'RL') || comparisonLapNum === null) return
    const requiredMask = (rowTypeMask | DATA_ROW.lap) >>> 0
    if (((playbackCache[comparisonLapNum]?.rowTypeMask ?? 0) & requiredMask) !== requiredMask) {
      window.playerBridge.getLapData(comparisonLapNum, requiredMask)
    }
  }, [comparisonLapNum, currentLapRevision, isPlayback, mode, playbackCache, rowTypeMask])
  // Live Previous and Fastest are read from the engine's store, complete even
  // for laps this window missed while hidden; the store dedupes the requests.
  useEffect(() => {
    if (isPlayback || (mode !== 'PL' && mode !== 'FL') || comparisonLapNum === null) return
    requestStoredLiveLaps()
  }, [comparisonLapData, comparisonLapNum, isPlayback, mode])

  const enabled = mode !== null && mode !== 'AL' && mode !== 'SL'
  const allLapsMode = mode === 'AL' || mode === 'SL'
  const stintLapsMode = mode === 'SL'
  const comparisonMode = mode === 'PL' || mode === 'FL' || mode === 'RL' || backgroundMode !== null
  // Match Analysis: after a playback seek, use the indexed lap's canonical
  // origin/progress instead of mixing seek-backfill progress with cached
  // comparison progress. The latter can differ by one packet (~17 ms).
  const rawProgress = playbackCurrentLap?.lapProgress ?? currentProgress
  const lapStartTime = playbackCurrentLap?.startSessionTime ?? currentLapStartTime
  const lapEndTime = playbackCurrentLap?.endSessionTime ?? Infinity
  const lapRevision = currentLapRevision
  const currentLapStartRef = useRef<number | null>(null)
  const currentProgressMapRef = useRef<LapProgressMap | null>(null)
  const comparisonProgressMapRef = useRef<LapProgressMap | null>(null)
  // The current lap grows with every lap row; its builder only reads new rows.
  const [currentProgressBuilder] = useState(() => new LapProgressBuilder())
  // getX is the time identity outside distance modes; nothing reads these then.
  const currentProgressMap = enabled ? currentProgressBuilder.update(rawProgress, lapStartTime, lapEndTime) : null
  const comparisonProgressMap = comparisonMode && comparisonLapData ? buildLapProgressMap(comparisonLapData) : null
  // The accessors below keep one identity for the provider's lifetime, because
  // charts rebuild their buffers whenever getX/getComparisonX change, yet
  // children call them during this same render. So their data is published
  // here, during render, instead of an effect that would run after children.
  /* eslint-disable react-hooks/refs */
  currentProgressMapRef.current = currentProgressMap
  currentLapStartRef.current = enabled ? lapStartTime : null
  comparisonProgressMapRef.current = comparisonProgressMap
  /* eslint-enable react-hooks/refs */
  const getX = useCallback((sessionTime: number) =>
    interpolateDistance(currentProgressMapRef.current, currentLapStartRef.current, sessionTime), [])
  const getComparisonX = useCallback((sessionTime: number) =>
    interpolateDistance(comparisonProgressMapRef.current, null, sessionTime), [])
  const getDeltaAtDistance = useCallback((distance: number) => {
    const current = currentProgressMapRef.current
    const comparison = comparisonProgressMapRef.current
    if (!current || !comparison) return NaN
    const currentElapsed = interpolateLapElapsed(current, distance)
    const comparisonElapsed = interpolateLapElapsed(comparison, distance)
    const delta = currentElapsed - comparisonElapsed
    return Number.isFinite(delta) ? delta : NaN
  }, [])
  const storeLapBoundaries = useLapBoundaries(allLapsMode)
  const lapBoundaries = lapBoundariesOverride ?? storeLapBoundaries
  // Time-window background: B's rows placed at A's lap start plus their time into B's lap.
  const backgroundAStart = backgroundMode === 'time' && backgroundLapNum !== null
    ? lapStartSessionTime(lapBoundaries, backgroundLapNum) ?? lapStartTime : null
  const backgroundAPrevStart = backgroundMode === 'time' && backgroundLapNum !== null
    ? lapStartSessionTime(lapBoundaries, backgroundLapNum - 1) ?? null : null
  const backgroundTimeData = useMemo(() => backgroundMode === 'time' && backgroundLap && backgroundAStart !== null
    ? joinBackgroundLaps(backgroundPreviousLap, backgroundLap,
      backgroundAPrevStart !== null ? backgroundAStart - backgroundAPrevStart : null)
    : null, [backgroundAPrevStart, backgroundAStart, backgroundLap, backgroundMode, backgroundPreviousLap])
  const backgroundTimeRef = useRef<{ bStart: number; aStart: number; bPrevStart: number | null; aPrevStart: number | null } | null>(null)
  /* eslint-disable react-hooks/refs */
  backgroundTimeRef.current = backgroundTimeData && backgroundLap && backgroundAStart !== null ? {
    bStart: backgroundLap.startSessionTime,
    aStart: backgroundAStart,
    bPrevStart: backgroundPreviousLap?.startSessionTime ?? null,
    aPrevStart: backgroundAPrevStart,
  } : null
  /* eslint-enable react-hooks/refs */
  const getBackgroundTimeX = useCallback((sessionTime: number) => {
    const map = backgroundTimeRef.current
    if (!map) return NaN
    if (sessionTime >= map.bStart) return map.aStart + (sessionTime - map.bStart)
    if (map.bPrevStart !== null && map.aPrevStart !== null && sessionTime >= map.bPrevStart)
      return map.aPrevStart + (sessionTime - map.bPrevStart)
    return NaN
  }, [])
  const knownStintStartTime = stintStartLap ? lapStartSessionTime(lapBoundaries, stintStartLap) : undefined
  const stintStartTime = stintLapsMode ? knownStintStartTime ?? currentStintStartTime : -Infinity
  const boundaryLabelsRef = useRef(new Map<number, string>())
  const boundaryValuesRef = useRef<number[]>([])
  // Published during render for the same reason as the progress maps above.
  /* eslint-disable react-hooks/refs */
  boundaryLabelsRef.current = new Map(lapBoundaries.map(boundary => [boundary.sessionTime, String(boundary.lapNum)]))
  boundaryValuesRef.current = lapBoundaries
    .filter(boundary => !stintLapsMode || boundary.sessionTime >= stintStartTime)
    .map(boundary => boundary.sessionTime)
  /* eslint-enable react-hooks/refs */
  const getAllLapTicks = useCallback((min: number, max: number) => boundaryValuesRef.current
    .filter(time => time >= min && time <= max), [])
  const formatAllLapX = useCallback((x: number) => boundaryLabelsRef.current.get(x) ?? '', [])
  const sectorBoundaryMode = enabled && sectorBoundaries
  const sectorSplitsByNumber = new Map<number, SectorSplit>()
  if (sectorBoundaryMode && isPlayback) {
    const preferredLapNumbers = [currentLapNum, comparisonLapNum, fastestLapNum]
      .filter((lapNum): lapNum is number => lapNum !== null)
    const metadataBlock = preferredLapNumbers
      .map(lapNum => lapBlocks?.find(block => block.lapNum === lapNum))
      .find((block): block is PlaybackLapBlock => block !== undefined &&
        Number(block.sector1EndDistanceM) > 0 && Number(block.sector2EndDistanceM) > 0)
      ?? lapBlocks?.find(block =>
        Number(block.sector1EndDistanceM) > 0 && Number(block.sector2EndDistanceM) > 0)
    if (metadataBlock) {
      sectorSplitsByNumber.set(1, {
        afterSector: 1,
        distance: Number(metadataBlock.sector1EndDistanceM),
        elapsedSeconds: 0,
      })
      sectorSplitsByNumber.set(2, {
        afterSector: 2,
        distance: Number(metadataBlock.sector2EndDistanceM),
        elapsedSeconds: 0,
      })
    }
  }
  if (sectorBoundaryMode && comparisonMode) {
    for (const split of findSectorSplits(comparisonLapData)) sectorSplitsByNumber.set(split.afterSector, split)
  }
  if (sectorBoundaryMode) {
    for (const split of currentProgressBuilder.sectorSplits()) {
      sectorSplitsByNumber.set(split.afterSector, split)
    }
  }
  // Live: once a completed lap has fixed a boundary, keep it for the whole
  // session. The per-lap discovery above only fills sectors not yet learned,
  // so a new lap no longer starts with the boundaries missing or shifted.
  if (sectorBoundaryMode && !isPlayback) {
    for (const split of liveSectorSplits) sectorSplitsByNumber.set(split.afterSector, split)
  }
  const sectorSplits = ([1, 2] as const).flatMap(sector => {
    const split = sectorSplitsByNumber.get(sector)
    return split ? [split] : []
  })
  const sectorSplitsRef = useRef<SectorSplit[]>([])
  const sectorTickLabelsRef = useRef(new Map<number, string>())
  // Published during render for the same reason as the progress maps above.
  // eslint-disable-next-line react-hooks/refs
  sectorSplitsRef.current = sectorSplits
  const getSectorTicks = useCallback((min: number, max: number) => {
    const labels = new Map<number, string>()
    const values = [min]
    for (const split of sectorSplitsRef.current) {
      if (split.distance <= min || split.distance >= max) continue
      values.push(split.distance)
      labels.set(split.distance, `S${split.afterSector}`)
    }
    if (max > min) {
      values.push(max)
      labels.set(max, 'S3')
    }
    sectorTickLabelsRef.current = labels
    return values.filter((value, index) => index === 0 || Math.abs(value - values[index - 1]) > 1e-6)
  }, [])
  const formatSectorX = useCallback((x: number) => sectorTickLabelsRef.current.get(x) ?? '', [])
  const lastIndex = rawProgress.length - 1
  const lastProgress = lastIndex >= 0
    ? { session_time: rawProgress.time(lastIndex), lap_distance_m: rawProgress.num('lap_distance_m', lastIndex) }
    : undefined
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
      firstProgressTime: rawProgress.length ? rawProgress.time(0) : null,
      lastProgressTime: lastProgress?.session_time ?? null,
      firstDistance: rawProgress.length ? rawProgress.num('lap_distance_m', 0) : null,
      lastDistance: lastProgress?.lap_distance_m ?? null,
    })
  }, [comparisonLapData, comparisonLapNum, currentLapNum, lapRevision, fastestLapNum,
    isPlayback, lapEndTime, lapStartTime, mode, playbackCurrentLap,
    progressRevision, rawProgress, currentProgress.length, lastProgress?.session_time, lastProgress?.lap_distance_m])
  return <Context.Provider value={{
    mode: enabled ? mode as DistanceChartMode : null,
    distanceMode: enabled,
    allLapsMode,
    stintLapsMode,
    historyStartTime: stintStartTime,
    // Time-axis charts rebuild when this changes: on a new history range, and
    // when restored or backfilled history replaces rows they already hold.
    historyRevision: `${stintLapsMode ? `SL:${stintStartTime}` : mode === 'AL' ? 'AL' : ''}#${historyReplacementRevision}`,
    comparisonMode: comparisonMode || !!comparisonOverride,
    comparisonLabel: comparisonOverride ? comparisonOverride.label : backgroundMode ? secondaryBackground.label : null,
    trackLengthM,
    lapRevision,
    progressRevision,
    lapData: comparisonOverride ? comparisonOverride.lapData : backgroundMode === 'time' ? backgroundTimeData : comparisonLapData,
    getX: enabled ? getX : DEFAULT.getX,
    getComparisonX: comparisonOverride ? comparisonOverride.getComparisonX : backgroundMode === 'time' ? getBackgroundTimeX : comparisonMode ? getComparisonX : DEFAULT.getComparisonX,
    getDeltaAtDistance: comparisonMode && enabled ? getDeltaAtDistance : DEFAULT.getDeltaAtDistance,
    formatX: sectorBoundaryMode ? formatSectorX : enabled ? formatChartDistance : allLapsMode ? formatAllLapX : DEFAULT.formatX,
    xTickValues: sectorBoundaryMode ? getSectorTicks : allLapsMode ? getAllLapTicks : undefined,
    cullXTickLabels: !sectorBoundaryMode,
    axisRevision: allLapsMode
      ? `${mode}:${stintStartTime}:` + lapBoundaries.map(boundary => `${boundary.lapNum}:${boundary.sessionTime}`).join('|')
      : sectorBoundaryMode
        ? `sectors:${progressRevision}:${sectorSplits.map(split => `${split.afterSector}:${split.distance}`).join('|')}`
        : `distance:${progressRevision}`,
  }}>{children}</Context.Provider>
}

/**
 * The axes for file B's half of a split chart, in whichever window that half
 * picked (its own selector):
 *  - Current, Previous, Fastest, Selected: B's lap on its own lap-distance
 *    axis; the last three add that B lap as the comparison trace.
 *  - Stint Laps, All Laps: B's session time, ticked at B's lap starts.
 *  - a time window: plain session time.
 * B's rows themselves come through the side context (useSideRows).
 */
export function SecondaryChartCoordinatesProvider({ mode, lapData, comparisonLapData, lapBoundaries, stintStartTime, historyKey, sectorBoundaries, lapBlocks, fastestLapNum, children }: {
  mode: ChartMode | null
  /** B's current lap: the distance axis for the lap modes. */
  lapData: AnalyzeLapData | null
  /** B's previous, fastest or selected lap in those modes. */
  comparisonLapData: AnalyzeLapData | null
  /** B's lap starts, for the Stint Laps and All Laps ticks. */
  lapBoundaries: readonly LapBoundary[]
  /** Where B's stint starts, in Stint Laps. */
  stintStartTime: number
  /** Changes when B's history in those modes is replaced (another file or car). */
  historyKey: string
  sectorBoundaries: boolean
  /** B's lap blocks: their recorded sector-end distances, as A's playback blocks give A's. */
  lapBlocks: readonly { lapNum: number; sector1EndDistanceM?: number; sector2EndDistanceM?: number }[]
  /** B's fastest lap, the last preferred source of those distances (as for A). */
  fastestLapNum: number | null
  children: React.ReactNode
}) {
  const storeTrackLengthM = useTelemetryStore(state => state.analyzeTrackLengthM)
  const distance = mode === 'CL' || mode === 'PL' || mode === 'FL' || mode === 'RL'
  const comparing = distance && mode !== 'CL' && comparisonLapData !== null
  const allLapsMode = mode === 'AL' || mode === 'SL'
  const stintLapsMode = mode === 'SL'
  const progressMap = distance ? buildLapProgressMap(lapData) : null
  const comparisonMap = comparing ? buildLapProgressMap(comparisonLapData) : null
  const progressMapRef = useRef<LapProgressMap | null>(null)
  const comparisonMapRef = useRef<LapProgressMap | null>(null)
  // Published during render, as ChartCoordinatesProvider does, so children
  // calling getX in this render see these laps.
  /* eslint-disable react-hooks/refs */
  progressMapRef.current = progressMap
  comparisonMapRef.current = comparisonMap
  /* eslint-enable react-hooks/refs */
  const getX = useCallback((sessionTime: number) => interpolateDistance(progressMapRef.current, null, sessionTime), [])
  const getComparisonX = useCallback((sessionTime: number) => interpolateDistance(comparisonMapRef.current, null, sessionTime), [])
  const getDeltaAtDistance = useCallback((distanceM: number) => {
    const current = progressMapRef.current
    const comparison = comparisonMapRef.current
    if (!current || !comparison) return NaN
    const delta = interpolateLapElapsed(current, distanceM) - interpolateLapElapsed(comparison, distanceM)
    return Number.isFinite(delta) ? delta : NaN
  }, [])
  const rows = lapData?.lapProgress
  const lastDistance = rows && rows.length ? rows.num('lap_distance_m', rows.length - 1) : 0
  const trackLengthM = storeTrackLengthM > 0 ? storeTrackLengthM : lastDistance
  // Sector ticks resolved as A's are: the recorded sector-end distances of B's
  // current, comparison or fastest lap block (else any block that has them),
  // overridden by B's comparison lap's splits, then by B's current lap's own.
  const splits = new Map<number, SectorSplit>()
  if (distance && sectorBoundaries) {
    const hasSectors = (block: typeof lapBlocks[number] | undefined): block is typeof lapBlocks[number] =>
      block !== undefined && Number(block.sector1EndDistanceM) > 0 && Number(block.sector2EndDistanceM) > 0
    const metadataBlock = [lapData?.lapNum ?? null, comparing ? comparisonLapData?.lapNum ?? null : null, fastestLapNum]
      .filter((lapNum): lapNum is number => lapNum !== null)
      .map(lapNum => lapBlocks.find(block => block.lapNum === lapNum))
      .find(hasSectors) ?? lapBlocks.find(hasSectors)
    if (metadataBlock) {
      splits.set(1, { afterSector: 1, distance: Number(metadataBlock.sector1EndDistanceM), elapsedSeconds: 0 })
      splits.set(2, { afterSector: 2, distance: Number(metadataBlock.sector2EndDistanceM), elapsedSeconds: 0 })
    }
    if (comparing) for (const split of findSectorSplits(comparisonLapData)) splits.set(split.afterSector, split)
    for (const split of findSectorSplits(lapData)) splits.set(split.afterSector, split)
  }
  const sectorSplits = ([1, 2] as const).flatMap(sector => splits.get(sector) ?? [])
  const sectorSplitsRef = useRef<SectorSplit[]>([])
  const sectorLabelsRef = useRef(new Map<number, string>())
  // eslint-disable-next-line react-hooks/refs
  sectorSplitsRef.current = sectorSplits
  const getSectorTicks = useCallback((min: number, max: number) => {
    const labels = new Map<number, string>()
    const values = [min]
    for (const split of sectorSplitsRef.current) {
      if (split.distance <= min || split.distance >= max) continue
      values.push(split.distance)
      labels.set(split.distance, `S${split.afterSector}`)
    }
    if (max > min) {
      values.push(max)
      labels.set(max, 'S3')
    }
    sectorLabelsRef.current = labels
    return values.filter((value, index) => index === 0 || Math.abs(value - values[index - 1]) > 1e-6)
  }, [])
  const formatSectorX = useCallback((x: number) => sectorLabelsRef.current.get(x) ?? '', [])
  // B's lap ticks in Stint Laps and All Laps.
  const boundaryLabelsRef = useRef(new Map<number, string>())
  const boundaryValuesRef = useRef<number[]>([])
  /* eslint-disable react-hooks/refs */
  boundaryLabelsRef.current = new Map(lapBoundaries.map(boundary => [boundary.sessionTime, String(boundary.lapNum)]))
  boundaryValuesRef.current = lapBoundaries
    .filter(boundary => !stintLapsMode || boundary.sessionTime >= stintStartTime)
    .map(boundary => boundary.sessionTime)
  /* eslint-enable react-hooks/refs */
  const getAllLapTicks = useCallback((min: number, max: number) => boundaryValuesRef.current
    .filter(time => time >= min && time <= max), [])
  const formatAllLapX = useCallback((x: number) => boundaryLabelsRef.current.get(x) ?? '', [])
  // As A: the toggle alone decides; with no boundary known yet the axis still reads S3 at the end.
  const sectorMode = distance && sectorBoundaries
  const revision = `secondary:${lapData?.lapNum ?? ''}:${lapData?.startSessionTime ?? ''}:${rows?.length ?? 0}`
  const value = useMemo<ChartCoordinates>(() => distance ? {
    ...DEFAULT,
    mode: mode as DistanceChartMode,
    distanceMode: true,
    comparisonMode: comparing,
    trackLengthM,
    lapRevision: lapData?.lapNum ?? 0,
    progressRevision: `${revision}:${comparisonLapData?.lapNum ?? ''}`,
    lapData: comparing ? comparisonLapData : null,
    getX,
    getComparisonX: comparing ? getComparisonX : DEFAULT.getComparisonX,
    getDeltaAtDistance: comparing ? getDeltaAtDistance : DEFAULT.getDeltaAtDistance,
    formatX: sectorMode ? formatSectorX : formatChartDistance,
    xTickValues: sectorMode ? getSectorTicks : undefined,
    cullXTickLabels: !sectorMode,
    axisRevision: `${revision}:${sectorMode ? 'sectors:' : ''}${sectorSplits.map(split => `${split.afterSector}:${split.distance}`).join('|')}`,
  } : allLapsMode ? {
    ...DEFAULT,
    allLapsMode: true,
    stintLapsMode,
    historyStartTime: stintLapsMode ? stintStartTime : -Infinity,
    historyRevision: `B:${mode}:${historyKey}:${stintLapsMode ? stintStartTime : ''}`,
    formatX: formatAllLapX,
    xTickValues: getAllLapTicks,
    axisRevision: `B:${mode}:${stintStartTime}:` + lapBoundaries.map(boundary => `${boundary.lapNum}:${boundary.sessionTime}`).join('|'),
  } : { ...DEFAULT, historyRevision: `B:time:${historyKey}` },
  // The accessors keep their identity; these are what the axes depend on.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  [distance, allLapsMode, stintLapsMode, mode, comparing, trackLengthM, revision, comparisonLapData, sectorMode,
    sectorSplits.map(split => `${split.afterSector}:${split.distance}`).join('|'), stintStartTime, historyKey, lapBoundaries])
  return <Context.Provider value={value}>{children}</Context.Provider>
}

export function useChartCoordinates(): ChartCoordinates {
  return useContext(Context)
}

/**
 * B's lap for a time-window background, preceded by the part of B's previous
 * lap that fits A's previous lap (`previousDuration` seconds), so the trace
 * reaches back across the lap line without overlapping B's current lap.
 */
function joinBackgroundLaps(previous: AnalyzeLapData | null, current: AnalyzeLapData, previousDuration: number | null): AnalyzeLapData {
  if (!previous || previousDuration === null || previousDuration <= 0) return current
  const cut = previous.startSessionTime + previousDuration
  const join = <T extends { session_time: number }>(before: ColumnView<T>, after: ColumnView<T>) =>
    concatAfter(before.slice(0, before.lowerBound(cut, true)), after)
  return {
    ...current,
    telemetry: join(previous.telemetry, current.telemetry),
    motion: join(previous.motion, current.motion),
    motionEx: join(previous.motionEx, current.motionEx),
    statusHistory: join(previous.statusHistory, current.statusHistory),
    damageHistory: join(previous.damageHistory, current.damageHistory),
    lapProgress: join(previous.lapProgress, current.lapProgress),
    rowTypeMask: ((previous.rowTypeMask ?? 0xFFFFFFFF) & (current.rowTypeMask ?? 0xFFFFFFFF)) >>> 0,
  }
}
