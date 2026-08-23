import { create } from 'zustand'
import type {
  TelemetryRow, MotionRow, MotionExRow, LapRow, StatusRow, DamageRow, TimingMsg,
  ParticipantsMsg, AllStatusMsg, RaceEventMsg, SessionMsg, TyreSetsMsg, GatewayMsg,
  LapProgressPoint, SessionHistoryFastestMsg, ProtocolStatusMsg, ProtocolWarningMsg,
  AnalyzeLapData, PlaybackLapDataMsg,
  StrategySnapshotMsg,
} from '../types'
import { decodeBinaryBatchRange, forEachDecodedBinaryRow } from '../lib/decodeBinaryBatch'
import { scheduleCooperativeTask, yieldToMainThread } from '../lib/cooperativeTask'
import { playbackDebug } from '../lib/playbackDebug'
import { HISTORY_ROW } from '../lib/historyDependencies'
import { mergeAnalyzeLapData } from '../lib/analyzeLapData'

// ─────────────────────────────────────────────────────────────────────────────
// Telemetry store.
//
// This is a straight relocation of the old useTelemetry hook. The reason it is a
// module-level store rather than a hook living in <App> is performance: the hot
// 60–120 Hz stream used to bump state at the App root, re-rendering the entire
// component tree ~120×/s. Here the IPC subscription writes to the store OUTSIDE
// React, and components subscribe to only the slices they read — so a telemetry
// frame re-renders just the handful of leaves that display it, never all of App.
//
// The buffer/window/lap/playback logic below is IDENTICAL to the old hook; only
// the plumbing (refs → module vars, useState → store set()) changed.
// ─────────────────────────────────────────────────────────────────────────────

const MAX_ROWS = 750000
const RETENTION_S = 600 // reconciliation window used only after a clock reversal
const MAX_RACE_EVENTS = 1000
const MAX_ANALYZE_LAP_CACHE = 6
// Keep a little slack above the hard cap so trimming is chunked instead of
// copying a large source array on every appended row.
const TRIM_CHUNK = 4096

// Buffers are sorted by session_time (appendRow enforces ordering), so the
// windowed views are contiguous suffixes/prefixes — binary-search the boundary
// instead of filtering the whole buffer every frame.
function lowerBound<T extends { session_time: number }>(arr: T[], t: number, inclusive: boolean): number {
  let l = 0, r = arr.length
  while (l < r) {
    const mid = (l + r) >> 1
    const keep = inclusive ? arr[mid].session_time >= t : arr[mid].session_time > t
    if (keep) r = mid; else l = mid + 1
  }
  return l
}

// Append a row to a buffer in place — O(1) amortized. Source buffers retain the
// complete session (up to MAX_ROWS) so AL can be selected at any point without
// having already discarded earlier laps. Ordinary consumers still receive only
// their selected time-window suffix. A session_time reversal rebuilds the buffer.
function appendRow<T extends { session_time: number }>(ref: { current: T[] }, msg: T, maxRows: number): void {
  const buf = ref.current
  const last = buf[buf.length - 1]
  if (last && msg.session_time < last.session_time) {
    if (allLapsMode) {
      // A rapid seek can leave a few superseded future rows in flight. AL owns
      // the full prefix, so reconcile by truncating only that future tail;
      // applying the ordinary 600-second recovery window here destroys laps.
      const rebuilt = buf.slice(0, lowerBound(buf, msg.session_time, true))
      rebuilt.push(msg)
      ref.current = rebuilt
      return
    }
    const cutoff = msg.session_time - RETENTION_S
    const rebuilt = buf.filter(d => d.session_time < msg.session_time && d.session_time >= cutoff)
    rebuilt.push(msg)
    ref.current = rebuilt
    return
  }
  buf.push(msg)
  if (buf.length > maxRows + TRIM_CHUNK) {
    ref.current = buf.slice(buf.length - maxRows)
  }
}

// Double-buffered window views: refill one of two persistent arrays each frame
// (two, so consumers' identity-based memo deps still see a change and the
// previous frame's array is never mutated under a holder mid-comparison).
interface WindowPool<T> { a: T[]; b: T[]; flip: boolean }
function makeWindowPool<T>(): WindowPool<T> { return { a: [], b: [], flip: false } }
function fillRange<T>(pool: WindowPool<T>, src: T[], start: number, end: number): T[] {
  pool.flip = !pool.flip
  const out = pool.flip ? pool.a : pool.b
  const n = Math.max(0, end - start)
  out.length = n
  for (let i = 0; i < n; i++) out[i] = src[start + i]
  return out
}

declare global {
  interface Window {
    telemetryBridge: {
      on: (callback: (row: unknown) => void) => (() => void)
      onBatch: (callback: (batch: string) => void) => (() => void)
      onBinary: (callback: (batch: Uint8Array) => void) => (() => void)
      onResume: (callback: (payload: { binary: Uint8Array; coldJson: string }) => void) => (() => void)
    }
  }
}

// The reactive, published view. Components select from this. Everything else is
// working state kept in the module vars below (the store never publishes it).
export interface TelemetryStoreState {
  telemetry: TelemetryRow[]
  motion: MotionRow[]
  motionEx: MotionExRow[]
  status: StatusRow | null
  statusHistory: StatusRow[]
  damage: DamageRow | null
  damageHistory: DamageRow[]
  lap: LapRow | null
  timing: TimingMsg | null
  participants: ParticipantsMsg | null
  allStatus: AllStatusMsg | null
  fastestLapCarIdx: number | null
  raceEvents: RaceEventMsg[]
  session: SessionMsg | null
  tyreSets: TyreSetsMsg | null
  strategy: StrategySnapshotMsg | null
  latest: TelemetryRow | null
  fastestLapNum: number | null
  analyzeLapTelemetry: TelemetryRow[]
  analyzeLapMotion: MotionRow[]
  analyzeLapMotionEx: MotionExRow[]
  analyzeLapStatusHistory: StatusRow[]
  analyzeLapDamageHistory: DamageRow[]
  analyzeLapProgress: LapProgressPoint[]
  analyzeLapStartTime: number
  analyzeLapRevision: number
  analyzeDeltaAvailable: boolean
  analyzeTrackLengthM: number
  playbackTnrdVersion: string | null
  playbackTrackId: number | null
  playbackTrackName: string | null
  playbackLapDataCache: Record<number, AnalyzeLapData>
  livePreviousLapData: AnalyzeLapData | null
  liveFastestLapData: AnalyzeLapData | null
  lapTimesByNum: Record<number, number>
  speedRpmBlocks: any[] | null
  isConnected: boolean
  error: string | null
  protocolStatus: ProtocolStatusMsg | null
  protocolWarning: ProtocolWarningMsg | null
  fuelUpperLimit: number | null
  seconds: number
  lapBoundaries: Array<{ lapNum: number; sessionTime: number }>
  currentStintStartTime: number
}

export const useTelemetryStore = create<TelemetryStoreState>()(() => ({
  telemetry: [], motion: [], motionEx: [],
  status: null, statusHistory: [], damage: null, damageHistory: [],
  lap: null, timing: null, participants: null, allStatus: null, strategy: null,
  fastestLapCarIdx: null, raceEvents: [], session: null, tyreSets: null,
  latest: null, fastestLapNum: null,
  analyzeLapTelemetry: [], analyzeLapMotion: [], analyzeLapMotionEx: [],
  analyzeLapStatusHistory: [], analyzeLapDamageHistory: [], analyzeLapProgress: [], analyzeLapStartTime: 0,
  analyzeLapRevision: 0,
  analyzeDeltaAvailable: false, analyzeTrackLengthM: 0, playbackTnrdVersion: null,
  playbackTrackId: null, playbackTrackName: null,
  playbackLapDataCache: {},
  livePreviousLapData: null,
  liveFastestLapData: null,
  lapTimesByNum: {}, speedRpmBlocks: null, isConnected: true, error: null,
  protocolStatus: null, protocolWarning: null, fuelUpperLimit: null, seconds: 30,
  lapBoundaries: [],
  currentStintStartTime: -Infinity,
}))

const set = useTelemetryStore.setState

// ── Working state (never published; the source of truth for computation) ──────
const telBufRef   = { current: [] as TelemetryRow[] }
const motBufRef   = { current: [] as MotionRow[] }
const motExBufRef = { current: [] as MotionExRow[] }
const dmgBufRef   = { current: [] as DamageRow[] }
const stsBufRef   = { current: [] as StatusRow[] }
const lapProgressBufRef = { current: [] as LapProgressPoint[] }

const pools = {
  tel:    makeWindowPool<TelemetryRow>(),
  mot:    makeWindowPool<MotionRow>(),
  motEx:  makeWindowPool<MotionExRow>(),
  sts:    makeWindowPool<StatusRow>(),
  dmg:    makeWindowPool<DamageRow>(),
  analyzeTel:   makeWindowPool<TelemetryRow>(),
  analyzeMot:   makeWindowPool<MotionRow>(),
  analyzeMotEx: makeWindowPool<MotionExRow>(),
  analyzeSts:   makeWindowPool<StatusRow>(),
  analyzeDmg:   makeWindowPool<DamageRow>(),
  analyzeLapProgress: makeWindowPool<LapProgressPoint>(),
}

const raceEventListeners = new Set<(e: RaceEventMsg) => void>()
const allLapsDataListeners = new Set<() => void>()
let allLapsNotificationRunning = false
let allLapsNotificationPending = false

function scheduleAllLapsDataNotification(): void {
  allLapsNotificationPending = true
  if (allLapsNotificationRunning) return
  allLapsNotificationRunning = true
  const runCycle = (): void => {
    allLapsNotificationPending = false
    const listeners = [...allLapsDataListeners]
    let index = 0
    const runNext = (): void => {
      if (index < listeners.length) {
        try {
          listeners[index++]()
        } finally {
          scheduleCooperativeTask(runNext)
        }
      } else if (allLapsNotificationPending) {
        scheduleCooperativeTask(runCycle)
      } else {
        allLapsNotificationRunning = false
      }
    }
    scheduleCooperativeTask(runNext)
  }
  runCycle()
}

let lapState: LapRow | null = null
let lapNum: number | null = null
let lapStartTime = 0
let lapTrackingActive = false
let fastestLapTime = Infinity
let fastestLapSet = false
const sessionHistoryBest = new Map<number, number>()
let isPlaybackFlag = false
let fuelMaxReceived = -Infinity

let raceEventsArr: RaceEventMsg[] = []
let speedRpmBlocksVal: any[] | null = null
let playbackFastestLapNum = 0
let playbackEvents: RaceEventMsg[] = []
let playbackLapTimes: Record<number, number> = {}
let liveLapTimes: Record<number, number> = {}
let playbackLapCacheOrder: number[] = []
let analyzeLapRevisionVal = 0
let pendingAnalyzeLapReset = false
let liveLapBoundaries: Array<{ lapNum: number; sessionTime: number }> = []
let currentStintStartTime = -Infinity

let secondsVal = 30
let allLapsMode = false
let finiteWindowBackfillEnabled = true
let waitingForAllLapsHistory = false
let historyRowMask = 0xFFFFFFFF
let requestedHistoryRowMask = 0
let seekTimelineGeneration = 0
let seekRendererPending = false
const historyCoverageStart = new Map<number, number>()
const HISTORY_ROW_BITS = [
  HISTORY_ROW.telemetry,
  HISTORY_ROW.status,
  HISTORY_ROW.damage,
  HISTORY_ROW.lap,
  HISTORY_ROW.motion,
  HISTORY_ROW.motionEx,
]

function markHistoryCoverage(maskValue: unknown, startValue: unknown): void {
  const mask = Number(maskValue) >>> 0
  const start = Number(startValue)
  if (!Number.isFinite(start)) return
  for (const bit of HISTORY_ROW_BITS) {
    if (mask & bit) historyCoverageStart.set(bit, Math.min(historyCoverageStart.get(bit) ?? Infinity, start))
  }
}

function historyCovers(bit: number, requiredStart: number): boolean {
  return (historyCoverageStart.get(bit) ?? Infinity) <= requiredStart + 1
}

function resetSession(): void {
  // Invalidate a seek payload that is still being cooperatively decoded.
  seekTimelineGeneration++
  historyCoverageStart.clear()
  seekRendererPending = false
  analyzeLapRevisionVal++
  telBufRef.current = []
  motBufRef.current = []
  motExBufRef.current = []
  stsBufRef.current = []
  dmgBufRef.current = []
  lapProgressBufRef.current = []
  lapState = null; lapNum = null; lapStartTime = 0; lapTrackingActive = false
  fastestLapTime = Infinity; fastestLapSet = false
  sessionHistoryBest.clear()
  raceEventsArr = []
  speedRpmBlocksVal = null; playbackFastestLapNum = 0
  playbackEvents = []; playbackLapTimes = {}; liveLapTimes = {}
  playbackLapCacheOrder = []
  liveLapBoundaries = []
  currentStintStartTime = -Infinity
  pendingAnalyzeLapReset = false
  waitingForAllLapsHistory = false
  requestedHistoryRowMask = 0
  fuelMaxReceived = -Infinity
  set({
    status: null, damage: null, lap: null, timing: null, allStatus: null,
    participants: null, session: null, fastestLapCarIdx: null, tyreSets: null, strategy: null,
    fastestLapNum: null, speedRpmBlocks: null, raceEvents: [], fuelUpperLimit: null,
    analyzeLapTelemetry: [], analyzeLapMotion: [], analyzeLapMotionEx: [],
    analyzeLapStatusHistory: [], analyzeLapDamageHistory: [], analyzeLapProgress: [], analyzeLapStartTime: 0,
    analyzeLapRevision: analyzeLapRevisionVal,
    analyzeDeltaAvailable: false, analyzeTrackLengthM: 0, playbackTnrdVersion: null,
    playbackTrackId: null, playbackTrackName: null,
    playbackLapDataCache: {},
    livePreviousLapData: null,
    liveFastestLapData: null,
    lapBoundaries: [],
    currentStintStartTime: -Infinity,
  })
}

function isNewTyreStint(previous: StatusRow, current: StatusRow): boolean {
  const validCompounds = previous.tyre_compound > 0 && current.tyre_compound > 0
  const compoundChanged = validCompounds && (
    current.tyre_compound !== previous.tyre_compound ||
    current.visual_compound !== previous.visual_compound
  )
  const ageDelta = current.tyre_age_laps - previous.tyre_age_laps
  // A larger age can mean a used set was fitted. Only treat a short-interval
  // jump as a stop; a long gap can legitimately span several completed laps
  // after the renderer was hidden or status streaming was interrupted.
  const usedSetFitted = ageDelta > 1 && current.session_time - previous.session_time < 30
  return compoundChanged || ageDelta < 0 || usedSetFitted
}

function findCurrentStintStart(rows: readonly StatusRow[]): number {
  if (rows.length === 0) return -Infinity
  let start = rows[0].session_time
  for (let i = 1; i < rows.length; i++) {
    if (isNewTyreStint(rows[i - 1], rows[i])) start = rows[i].session_time
  }
  return start
}

function truncateAt<T extends { session_time: number }>(ref: { current: T[] }, target: number): void {
  ref.current = ref.current.slice(0, lowerBound(ref.current, target, false))
}

function liveLapData(
  boundary: { lapNum: number; sessionTime: number } | undefined,
  endSessionTime: number,
): AnalyzeLapData | null {
  if (!boundary || endSessionTime < boundary.sessionTime) return null
  const range = <T extends { session_time: number }>(rows: T[]): T[] => rows.slice(
    lowerBound(rows, boundary.sessionTime, true),
    lowerBound(rows, endSessionTime, false),
  )
  return {
    lapNum: boundary.lapNum,
    startSessionTime: boundary.sessionTime,
    endSessionTime,
    telemetry: range(telBufRef.current),
    motion: range(motBufRef.current),
    motionEx: range(motExBufRef.current),
    statusHistory: range(stsBufRef.current),
    damageHistory: range(dmgBufRef.current),
    lapProgress: range(lapProgressBufRef.current),
    playerPositions: [],
  }
}

function applyLiveRewind(target: number): void {
  if (isPlaybackFlag || !Number.isFinite(target) || target < 0) return

  truncateAt(telBufRef, target)
  truncateAt(motBufRef, target)
  truncateAt(motExBufRef, target)
  truncateAt(stsBufRef, target)
  truncateAt(dmgBufRef, target)
  truncateAt(lapProgressBufRef, target)
  raceEventsArr = raceEventsArr.filter(e => e.session_time == null || e.session_time <= target)
  liveLapBoundaries = liveLapBoundaries.filter(boundary => boundary.sessionTime <= target)

  const currentIndex = liveLapBoundaries.length - 1
  const currentBoundary = liveLapBoundaries[currentIndex]
  const previousBoundary = liveLapBoundaries[currentIndex - 1]
  lapNum = currentBoundary?.lapNum ?? null
  lapStartTime = currentBoundary?.sessionTime ?? target
  const lastLap = lapProgressBufRef.current[lapProgressBufRef.current.length - 1] as LapRow | undefined
  lapState = lastLap ?? null
  const sessionType = useTelemetryStore.getState().session?.session_type
  const garageAware = sessionType != null && sessionType >= 1 && sessionType <= 14 &&
    lastLap?.driver_status != null && lastLap.driver_status >= 0
  lapTrackingActive = !garageAware || lastLap?.driver_status === 1
  if (!lapTrackingActive) lapNum = null

  const survivingTimes: Record<number, number> = {}
  for (let i = 0; i + 1 < liveLapBoundaries.length; i++) {
    const n = liveLapBoundaries[i].lapNum
    if (liveLapTimes[n] != null) survivingTimes[n] = liveLapTimes[n]
  }
  liveLapTimes = survivingTimes
  let fastestNum: number | null = null
  fastestLapTime = Infinity
  for (const [n, ms] of Object.entries(liveLapTimes)) {
    if (ms > 0 && ms < fastestLapTime) { fastestLapTime = ms; fastestNum = Number(n) }
  }

  const previousData = liveLapData(previousBoundary, currentBoundary?.sessionTime ?? target)
  const fastestIndex = fastestNum == null
    ? -1 : liveLapBoundaries.findIndex(boundary => boundary.lapNum === fastestNum)
  const fastestBoundary = fastestIndex >= 0 ? liveLapBoundaries[fastestIndex] : undefined
  const fastestEnd = fastestIndex >= 0 && fastestIndex + 1 < liveLapBoundaries.length
    ? liveLapBoundaries[fastestIndex + 1].sessionTime : target
  const fastestData = fastestNum == null ? null : liveLapData(fastestBoundary, fastestEnd)
  currentStintStartTime = findCurrentStintStart(stsBufRef.current)
  analyzeLapRevisionVal++
  pendingAnalyzeLapReset = false

  set({
    lap: lapState,
    status: stsBufRef.current[stsBufRef.current.length - 1] ?? null,
    damage: dmgBufRef.current[dmgBufRef.current.length - 1] ?? null,
    lapBoundaries: liveLapBoundaries,
    livePreviousLapData: previousData,
    liveFastestLapData: fastestData,
    fastestLapNum: fastestNum,
    lapTimesByNum: liveLapTimes,
    raceEvents: raceEventsArr,
    currentStintStartTime,
    analyzeLapRevision: analyzeLapRevisionVal,
  })
}

// The old useEffect([lap]): on a lap-number change, snapshot the completed lap
// and update live lap times / fastest lap. Runs in the 'lap' handler now.
function onLap(lap: LapRow): void {
  if (Number.isFinite(lap.lap_distance_m)) appendRow(lapProgressBufRef, lap, MAX_ROWS)
  lapState = lap
  set({ lap })
  const packetLapStart = lap.session_time - Math.max(0, lap.current_lap_ms) / 1000
  const sessionType = useTelemetryStore.getState().session?.session_type
  const timedSession = sessionType != null && sessionType >= 1 && sessionType <= 14
  const hasDriverStatus = lap.driver_status != null && lap.driver_status >= 0
  const garageAware = timedSession && hasDriverStatus

  if (garageAware && lap.driver_status !== 1) {
    if (lapTrackingActive) {
      liveLapBoundaries = liveLapBoundaries.filter(boundary => boundary.lapNum !== lapNum)
      lapNum = null
      lapStartTime = lap.session_time
      lapTrackingActive = false
      pendingAnalyzeLapReset = false
      analyzeLapRevisionVal++
      set({
        lapBoundaries: liveLapBoundaries,
        analyzeLapTelemetry: [], analyzeLapMotion: [], analyzeLapMotionEx: [],
        analyzeLapStatusHistory: [], analyzeLapDamageHistory: [], analyzeLapProgress: [],
        analyzeLapStartTime: lapStartTime,
        analyzeLapRevision: analyzeLapRevisionVal,
      })
    }
    return
  }

  if (garageAware && !lapTrackingActive) {
    lapNum = lap.lap_num
    lapStartTime = packetLapStart
    lapTrackingActive = true
    liveLapBoundaries = [
      ...liveLapBoundaries.filter(boundary => boundary.lapNum !== lap.lap_num),
      { lapNum: lap.lap_num, sessionTime: packetLapStart },
    ].sort((a, b) => a.sessionTime - b.sessionTime)
    analyzeLapRevisionVal++
    pendingAnalyzeLapReset = true
    set({
      lapBoundaries: liveLapBoundaries,
      analyzeLapTelemetry: [], analyzeLapMotion: [], analyzeLapMotionEx: [],
      analyzeLapStatusHistory: [], analyzeLapDamageHistory: [], analyzeLapProgress: [],
      analyzeLapStartTime: lapStartTime,
      analyzeLapRevision: analyzeLapRevisionVal,
    })
    return
  }

  const prevLapNum = lapNum
  // A replay opened without FLBK can emit one stale lap snapshot after the
  // timeline has already advanced. Explicit rewind handling lowers lapNum
  // before packets resume; an otherwise decreasing number is just stale and
  // must not create a backwards chart boundary.
  if (prevLapNum !== null && lap.lap_num < prevLapNum) return
  lapNum = lap.lap_num
  lapTrackingActive = true

  if (prevLapNum === null) {
    lapStartTime = packetLapStart
    liveLapBoundaries = [{ lapNum: lap.lap_num, sessionTime: packetLapStart }]
    set({ lapBoundaries: liveLapBoundaries })
    // Initial lap metadata (including metadata re-emitted after a playback
    // backfill) establishes the lap origin; it is not a chart reset boundary.
    // Recording load/seek/session reset already publish an explicit revision.
    return
  }
  if (lap.lap_num === prevLapNum) return

  liveLapBoundaries = [
    ...liveLapBoundaries.filter(boundary => boundary.lapNum !== lap.lap_num),
    { lapNum: lap.lap_num, sessionTime: packetLapStart },
  ].sort((a, b) => a.sessionTime - b.sessionTime)
  set({ lapBoundaries: liveLapBoundaries })

  let completedLapData: AnalyzeLapData | null = null
  if (!isPlaybackFlag) {
    const completed = useTelemetryStore.getState()
    completedLapData = {
      lapNum: prevLapNum,
      startSessionTime: lapStartTime,
      endSessionTime: packetLapStart,
      telemetry: [...completed.analyzeLapTelemetry],
      motion: [...completed.analyzeLapMotion],
      motionEx: [...completed.analyzeLapMotionEx],
      statusHistory: [...completed.analyzeLapStatusHistory],
      damageHistory: [...completed.analyzeLapDamageHistory],
      lapProgress: [...completed.analyzeLapProgress],
      playerPositions: [],
    }
    set({ livePreviousLapData: completedLapData })
  }

  analyzeLapRevisionVal++
  pendingAnalyzeLapReset = true

  // Playback has an authoritative session-wide fastest lap from its index.
  // Never let whichever lap happens to cross a boundary after a seek replace it.
  if (!isPlaybackFlag && completedLapData) {
    const lapTimeMs = lap.last_lap_ms
    if (lapTimeMs > 0 && lapTimeMs < 300_000) {
      if (liveLapTimes[prevLapNum] !== lapTimeMs) liveLapTimes = { ...liveLapTimes, [prevLapNum]: lapTimeMs }
    }
    if (lapTimeMs > 0 && lapTimeMs < 300_000 && lapTimeMs < fastestLapTime) {
      fastestLapTime = lapTimeMs
      set({ fastestLapNum: prevLapNum, liveFastestLapData: completedLapData })
    }
  }
  lapStartTime = packetLapStart
}

function handleMsg(msg: GatewayMsg): void {
  switch (msg.type) {
    case 'playback_loaded': {
      set({
        playbackTrackId: msg.ok ? msg.header?.track_id ?? null : null,
        playbackTrackName: msg.ok ? msg.header?.track_name ?? null : null,
      })
      break
    }
    case 'playback_close': {
      isPlaybackFlag = false
      resetSession()
      break
    }
    case 'telemetry': {
      const buf = telBufRef.current
      const last = buf[buf.length - 1]
      if (last && msg.session_time < last.session_time && !isPlaybackFlag) {
        // Playback can deliver a slightly older hot row around a seek/backfill
        // boundary. appendRow reconciles the renderer history below, but this
        // must not clear the Analyze GPU buffers. Explicit seek flushes carry
        // the revision that identifies a real timeline reset.
        applyLiveRewind(msg.session_time)
      }
      if (historyRowMask & HISTORY_ROW.telemetry) appendRow(telBufRef, msg, MAX_ROWS)
      else telBufRef.current = [msg]
      break
    }
    case 'motion':
      appendRow(motBufRef, msg, MAX_ROWS)
      break
    case 'motion_ex':
      appendRow(motExBufRef, msg, MAX_ROWS)
      break
    case 'status': {
      const next: Partial<TelemetryStoreState> = { status: msg }
      const previous = stsBufRef.current[stsBufRef.current.length - 1]
      const previousStintStartTime = currentStintStartTime
      if (!previous || msg.session_time < previous.session_time) {
        currentStintStartTime = msg.session_time
      } else if (isNewTyreStint(previous, msg)) {
        currentStintStartTime = msg.session_time
      }
      if (previousStintStartTime !== currentStintStartTime) next.currentStintStartTime = currentStintStartTime
      if (!isPlaybackFlag && Number.isFinite(msg.fuel_kg) && msg.fuel_kg >= 0 && msg.fuel_kg > fuelMaxReceived) {
        fuelMaxReceived = msg.fuel_kg
        // Keep exactly one kilogram of breathing room above the highest value
        // received in this live session.
        next.fuelUpperLimit = fuelMaxReceived + 1
      }
      set(next)
      if (historyRowMask & HISTORY_ROW.status) appendRow(stsBufRef, msg, MAX_ROWS)
      else stsBufRef.current = [msg]
      break
    }
    case 'damage':
      set({ damage: msg })
      if (historyRowMask & HISTORY_ROW.damage) appendRow(dmgBufRef, msg, MAX_ROWS)
      else dmgBufRef.current = [msg]
      break
    case 'lap':
      onLap(msg as unknown as LapRow)
      break
    case 'timing':       set({ timing: msg }); break
    case 'participants': set({ participants: msg }); break
    case 'all_status':   set({ allStatus: msg }); break
    case 'fastest_lap':
      set({ fastestLapCarIdx: (msg as any).car_idx })
      fastestLapSet = true
      break
    case 'session_history_fastest': {
      if (fastestLapSet) break
      const shMsg = msg as SessionHistoryFastestMsg
      sessionHistoryBest.set(shMsg.car_idx, shMsg.best_lap_time_ms)
      let minMs = Infinity
      let minIdx: number | null = null
      for (const [idx, ms] of sessionHistoryBest) {
        if (ms < minMs) { minMs = ms; minIdx = idx }
      }
      if (useTelemetryStore.getState().fastestLapCarIdx !== minIdx) set({ fastestLapCarIdx: minIdx })
      break
    }
    case 'tyre_sets':    set({ tyreSets: msg }); break
    case 'strategy':     set({ strategy: msg as StrategySnapshotMsg }); break
    case 'race_event':
      if ((msg as RaceEventMsg).code === 'FLBK') {
        const target = Number((msg as RaceEventMsg).flashback_session_time)
        if (Number.isFinite(target) && target >= 0) applyLiveRewind(target)
      }
      for (const cb of raceEventListeners) cb(msg as RaceEventMsg)
      raceEventsArr = [...raceEventsArr, msg as RaceEventMsg]
      if (raceEventsArr.length > MAX_RACE_EVENTS) raceEventsArr = raceEventsArr.slice(-MAX_RACE_EVENTS)
      if ((msg as any).code === 'SEND' && !isPlaybackFlag) {
        resetSession()
      }
      break
    case 'session':
      set({
        session: msg,
        ...(!isPlaybackFlag && Number.isFinite(msg.track_length_m) && msg.track_length_m > 0
          ? { analyzeTrackLengthM: msg.track_length_m }
          : {}),
      })
      break
    case 'protocol_status': set({ protocolStatus: msg }); break
    case 'protocol_warning': {
      const pw = msg as ProtocolWarningMsg
      set({ protocolWarning: (pw.detected_format === null || pw.detected_format === undefined) ? null : pw })
      break
    }
    case 'playback_lap_data': {
      const payload = msg as PlaybackLapDataMsg
      const lapData: AnalyzeLapData = {
        lapNum: payload.lapNum,
        startSessionTime: payload.startSessionTime,
        endSessionTime: payload.endSessionTime,
        telemetry: payload.telemetry ?? [],
        motion: payload.motionHistory ?? [],
        motionEx: payload.motionExHistory ?? [],
        statusHistory: payload.statusHistory ?? [],
        damageHistory: payload.damageHistory ?? [],
        lapProgress: payload.lapProgress ?? [],
        playerPositions: payload.playerPositions ?? [],
        rowTypeMask: payload.rowTypeMask ?? 0xFFFFFFFF,
      }
      playbackLapCacheOrder = [...playbackLapCacheOrder.filter(lapNum => lapNum !== lapData.lapNum), lapData.lapNum]
      set(state => {
        const prior = state.playbackLapDataCache[lapData.lapNum]
        const cache = {
          ...state.playbackLapDataCache,
          [lapData.lapNum]: prior ? mergeAnalyzeLapData(prior, lapData) : lapData,
        }
        while (playbackLapCacheOrder.length > MAX_ANALYZE_LAP_CACHE) {
          const evicted = playbackLapCacheOrder.shift()
          if (evicted !== undefined) delete cache[evicted]
        }
        return { playbackLapDataCache: cache }
      })
      break
    }
    case 'playback_seek_flush_bin': {
      void processPlaybackSeekFlush(msg as any).catch(error => {
        console.error('Failed to decode playback seek flush:', error)
        waitingForAllLapsHistory = false
        if ((msg as any).authoritativeSeek !== false) seekRendererPending = false
        recompute(DirtySlice.All)
        const requestId = Number((msg as any).requestId)
        if (Number.isFinite(requestId) && requestId > 0)
          window.playerBridge.seekInstalled(requestId)
      })
      break
    }
    case 'playback_seek_flush_failed':
      waitingForAllLapsHistory = false
      seekRendererPending = false
      requestedHistoryRowMask = 0
      break
    case 'playback_lap_blocks': {
      isPlaybackFlag = true
      analyzeLapRevisionVal++
      const data = msg as any
      fuelMaxReceived = -Infinity
      playbackLapCacheOrder = []
      speedRpmBlocksVal = data.blocks
      playbackFastestLapNum = data.fastestLapNum
      playbackEvents = data.events ?? []
      const map: Record<number, number> = {}
      for (const l of (data.laps ?? []) as { lapNum: number; lapTimeMs: number }[]) {
        if (l.lapTimeMs > 0) map[l.lapNum] = l.lapTimeMs
      }
      playbackLapTimes = map
      const initialFuelKg = Number(data.initialFuelKg)
      const trackLengthM = Number(data.trackLengthM)
      set({
        speedRpmBlocks: speedRpmBlocksVal,
        fastestLapNum: playbackFastestLapNum || null,
        playbackLapDataCache: {},
        liveFastestLapData: null,
        fuelUpperLimit: Number.isFinite(initialFuelKg) && initialFuelKg >= 0
          ? initialFuelKg + 1
          : null,
        analyzeDeltaAvailable: data.lapDistanceAvailable === true || data.deltaAvailable === true,
        analyzeTrackLengthM: Number.isFinite(trackLengthM) && trackLengthM > 0 ? trackLengthM : 0,
        playbackTnrdVersion: typeof data.tnrdVersion === 'string' ? data.tnrdVersion : null,
      })
      const missingHistory = historyRowMask & ~requestedHistoryRowMask
      if (allLapsMode && missingHistory !== 0) {
        waitingForAllLapsHistory = true
        requestedHistoryRowMask |= missingHistory
        window.playerBridge.getAllLapsData(missingHistory)
      }
      break
    }
  }
}

const enum DirtySlice {
  None = 0,
  Telemetry = 1 << 0,
  Motion = 1 << 1,
  MotionEx = 1 << 2,
  Status = 1 << 3,
  Damage = 1 << 4,
  Lap = 1 << 5,
  Derived = 1 << 6,
  All = Telemetry | Motion | MotionEx | Status | Damage | Lap | Derived,
}

function dirtySliceFor(msg: { type: string }): DirtySlice {
  switch (msg.type) {
    case 'telemetry': return DirtySlice.Telemetry | DirtySlice.Derived
    case 'motion': return DirtySlice.Motion
    case 'motion_ex': return DirtySlice.MotionEx
    case 'status': return DirtySlice.Status | DirtySlice.Derived
    case 'damage': return DirtySlice.Damage
    case 'lap': return DirtySlice.Lap | DirtySlice.Derived
    case 'race_event':
      return (msg as RaceEventMsg).code === 'FLBK' ? DirtySlice.All : DirtySlice.Derived
    case 'playback_lap_blocks': return DirtySlice.Derived
    case 'playback_close':
    case 'playback_seek_flush_failed': return DirtySlice.All
    case 'playback_seek_flush_bin': return DirtySlice.None
    default: return DirtySlice.None
  }
}

// Recompute only the window groups touched by a delivered batch. Unchanged
// groups retain their array identity, so their Zustand subscribers stay cold.
function recompute(dirty: DirtySlice): void {
  if (dirty === DirtySlice.None) return
  // A normal seek flush only contains the current lap. In AL keep the last
  // complete chart publication visible until the single prefix flush arrives,
  // otherwise the graph briefly collapses to one lap and then expands again.
  if (waitingForAllLapsHistory) return
  const currentTelBuf = telBufRef.current
  const latestSessionTime = currentTelBuf[currentTelBuf.length - 1]?.session_time ?? 0
  const telBuf = currentTelBuf
  const motBuf = motBufRef.current
  const motExBuf = motExBufRef.current
  const stsBuf = stsBufRef.current
  const dmgBuf = dmgBufRef.current
  const cutoff = allLapsMode ? -Infinity : latestSessionTime - secondsVal
  // `current_lap_ms` is legitimately zero at rollover. Falling back to session
  // origin in that state prepends the previous lap to every Analyze/CL slice,
  // so distance mapping stops at the stale prefix and charts retain one point.
  const lapStartSessionTime = lapTrackingActive ? lapStartTime : latestSessionTime
  const isPlayback = speedRpmBlocksVal !== null
  const lapTimesByNum = isPlayback ? playbackLapTimes : liveLapTimes

  let publishAnalyze = lapTrackingActive && !pendingAnalyzeLapReset
  if (pendingAnalyzeLapReset) {
    const countSince = <T extends { session_time: number }>(rows: T[]) => rows.length - lowerBound(rows, lapStartSessionTime, true)
    const ready = (bit: number, count: number, minimum: number) =>
      !(historyRowMask & bit) || count >= minimum
    publishAnalyze = ready(HISTORY_ROW.telemetry, countSince(telBuf), 2) &&
      ready(HISTORY_ROW.motion, countSince(motBufRef.current), 1) &&
      ready(HISTORY_ROW.motionEx, countSince(motExBufRef.current), 1) &&
      ready(HISTORY_ROW.status, countSince(stsBufRef.current), 1) &&
      ready(HISTORY_ROW.damage, countSince(dmgBufRef.current), 1) &&
      ready(HISTORY_ROW.lap, countSince(lapProgressBufRef.current), 2)
    if (publishAnalyze) {
      pendingAnalyzeLapReset = false
      dirty |= DirtySlice.All
    }
  }

  const next: Partial<TelemetryStoreState> = {}
  if (dirty & DirtySlice.Telemetry) {
    next.latest = currentTelBuf.length > 0 ? currentTelBuf[currentTelBuf.length - 1] : null
    if (historyRowMask & HISTORY_ROW.telemetry) {
      next.telemetry = allLapsMode ? telBuf : fillRange(pools.tel, telBuf, lowerBound(telBuf, cutoff, false), telBuf.length)
      if (publishAnalyze) next.analyzeLapTelemetry = fillRange(pools.analyzeTel, currentTelBuf, lowerBound(currentTelBuf, lapStartSessionTime, true), currentTelBuf.length)
    }
  }
  if (dirty & DirtySlice.Motion) {
    const buf = motBuf
    next.motion = allLapsMode ? buf : fillRange(pools.mot, buf, lowerBound(buf, cutoff, false), buf.length)
    const current = motBufRef.current
    if (publishAnalyze) next.analyzeLapMotion = fillRange(pools.analyzeMot, current, lowerBound(current, lapStartSessionTime, true), current.length)
  }
  if (dirty & DirtySlice.MotionEx) {
    const buf = motExBuf
    next.motionEx = allLapsMode ? buf : fillRange(pools.motEx, buf, lowerBound(buf, cutoff, false), buf.length)
    const current = motExBufRef.current
    if (publishAnalyze) next.analyzeLapMotionEx = fillRange(pools.analyzeMotEx, current, lowerBound(current, lapStartSessionTime, true), current.length)
  }
  if (dirty & DirtySlice.Status) {
    if (historyRowMask & HISTORY_ROW.status) {
      const buf = stsBuf
      next.statusHistory = allLapsMode ? buf : fillRange(pools.sts, buf, lowerBound(buf, cutoff, false), buf.length)
      const current = stsBufRef.current
      if (publishAnalyze) next.analyzeLapStatusHistory = fillRange(pools.analyzeSts, current, Math.max(0, lowerBound(current, lapStartSessionTime, true) - 1), current.length)
    }
  }
  if (dirty & DirtySlice.Damage) {
    if (historyRowMask & HISTORY_ROW.damage) {
      const buf = dmgBuf
      next.damageHistory = allLapsMode ? buf : fillRange(pools.dmg, buf, lowerBound(buf, cutoff, false), buf.length)
      const current = dmgBufRef.current
      if (publishAnalyze) next.analyzeLapDamageHistory = fillRange(pools.analyzeDmg, current, Math.max(0, lowerBound(current, lapStartSessionTime, true) - 1), current.length)
    }
  }
  if (dirty & DirtySlice.Lap) {
    const buf = lapProgressBufRef.current
    if (publishAnalyze) next.analyzeLapProgress = fillRange(
      pools.analyzeLapProgress, buf, lowerBound(buf, lapStartSessionTime, true), buf.length)
  }
  if (dirty & DirtySlice.Derived) {
    next.lapTimesByNum = lapTimesByNum
    next.raceEvents = isPlayback
      ? playbackEvents.filter(e => (e.session_time ?? 0) <= latestSessionTime)
      : raceEventsArr
    if (publishAnalyze) {
      next.analyzeLapStartTime = lapStartSessionTime
      next.analyzeLapRevision = analyzeLapRevisionVal
    }
  }
  set(next)
  if (allLapsMode && (dirty & (DirtySlice.Telemetry | DirtySlice.Motion | DirtySlice.MotionEx | DirtySlice.Status | DirtySlice.Damage))) {
    scheduleAllLapsDataNotification()
  }
}

function dirtySliceForHistoryMask(value: unknown): DirtySlice {
  const mask = Number(value)
  if (!Number.isFinite(mask)) return DirtySlice.All
  let dirty = DirtySlice.Derived
  if (mask & HISTORY_ROW.telemetry) dirty |= DirtySlice.Telemetry
  if (mask & HISTORY_ROW.motion) dirty |= DirtySlice.Motion
  if (mask & HISTORY_ROW.motionEx) dirty |= DirtySlice.MotionEx
  if (mask & HISTORY_ROW.status) dirty |= DirtySlice.Status
  if (mask & HISTORY_ROW.damage) dirty |= DirtySlice.Damage
  if (mask & HISTORY_ROW.lap) dirty |= DirtySlice.Lap
  return dirty
}

async function processPlaybackSeekFlush(payload: any): Promise<void> {
  const allHistory = payload.allHistory === true
  const authoritative = payload.authoritativeSeek !== false
  const generation = authoritative ? ++seekTimelineGeneration : seekTimelineGeneration
  const cancelled = () => generation !== seekTimelineGeneration

  playbackDebug('seek-flush-received', {
    lapNum: payload.lapNum,
    currentLapStart: payload.currentLapStart,
    binaryBytes: payload.binary?.byteLength ?? payload.binary?.length ?? null,
    coldJsonChars: typeof payload.coldJson === 'string' ? payload.coldJson.length : null,
    fastestLapNum: playbackFastestLapNum || null,
  })

  if (authoritative) {
    historyCoverageStart.clear()
    // AL cleared these at seek-start. Other modes retain the last publication
    // for display, but their working buffers must start collecting only rows
    // from the newly committed timeline while the backfill decodes.
    if (!allHistory) {
      telBufRef.current = []
      motBufRef.current = []
      motExBufRef.current = []
      stsBufRef.current = []
      dmgBufRef.current = []
      lapProgressBufRef.current = []
    }
    analyzeLapRevisionVal++
    pendingAnalyzeLapReset = false
    lapStartTime = payload.currentLapStart
    lapNum = payload.lapNum
    lapTrackingActive = true
    set({ fastestLapNum: playbackFastestLapNum || null })
  }

  // Let Chromium service the input/paint that delivered this IPC message
  // before allocating and decoding the history payload.
  await yieldToMainThread()
  if (cancelled()) return

  const tel: TelemetryRow[] = []
  const mot: MotionRow[] = []
  const motEx: MotionExRow[] = []
  const binary = payload.binary as Uint8Array | ArrayBuffer
  const binaryLength = binary instanceof Uint8Array ? binary.byteLength : binary?.byteLength ?? 0
  let binaryOffset = 0
  while (binaryOffset < binaryLength) {
    binaryOffset = decodeBinaryBatchRange(binary, row => {
      if (row.type === 'telemetry') tel.push(row)
      else if (row.type === 'motion') mot.push(row)
      else if (row.type === 'motion_ex') motEx.push(row)
    }, binaryOffset, 4096)
    if (binaryOffset < binaryLength) {
      await yieldToMainThread()
      if (cancelled()) return
    }
  }

  const sts: StatusRow[] = []
  const dmg: DamageRow[] = []
  const lapProgress: LapProgressPoint[] = []
  let lastLap: LapRow | null = null
  const coldJson = (payload.coldJson as string) || ''
  let start = 0
  let rowsSinceYield = 0
  while (start < coldJson.length) {
    let end = coldJson.indexOf('\n', start)
    if (end === -1) end = coldJson.length
    if (end > start) {
      try {
        const row = JSON.parse(coldJson.slice(start, end)) as StatusRow | DamageRow | LapRow
        if (row.type === 'status') sts.push(row)
        else if (row.type === 'damage') dmg.push(row)
        else if (row.type === 'lap') { lastLap = row; lapProgress.push(row) }
      } catch (e) {}
    }
    start = end + 1
    if (++rowsSinceYield >= 512 && start < coldJson.length) {
      rowsSinceYield = 0
      await yieldToMainThread()
      if (cancelled()) return
    }
  }
  if (cancelled()) return

  // Rows streamed after the native seek committed accumulated while decoding.
  // Append only that newer tail to the authoritative or additive backfill.
  const appendNewer = <T extends { session_time: number }>(base: T[], trailing: T[]): T[] => {
    const lastTime = base[base.length - 1]?.session_time ?? -Infinity
    for (const row of trailing) if (row.session_time > lastTime) base.push(row)
    return base.length > MAX_ROWS ? base.slice(-MAX_ROWS) : base
  }
  telBufRef.current = appendNewer(tel, telBufRef.current)
  motBufRef.current = appendNewer(mot, motBufRef.current)
  motExBufRef.current = appendNewer(motEx, motExBufRef.current)
  stsBufRef.current = appendNewer(sts, stsBufRef.current)
  dmgBufRef.current = appendNewer(dmg, dmgBufRef.current)
  lapProgressBufRef.current = appendNewer(lapProgress, lapProgressBufRef.current)
  currentStintStartTime = findCurrentStintStart(stsBufRef.current)
  if (allHistory) waitingForAllLapsHistory = false
  markHistoryCoverage(payload.rowTypeMask, payload.historyStart)

  playbackDebug('seek-flush-decoded', {
    lapNum,
    lapStartTime,
    revision: analyzeLapRevisionVal,
    telemetryRows: tel.length,
    telemetryFirstTime: tel[0]?.session_time ?? null,
    telemetryLastTime: tel[tel.length - 1]?.session_time ?? null,
    motionRows: mot.length,
    motionExRows: motEx.length,
    statusRows: sts.length,
    damageRows: dmg.length,
    lapProgressRows: lapProgress.length,
    lastLapNumber: lastLap?.lap_num ?? null,
    lastLapTimeMs: lastLap?.current_lap_ms ?? null,
  })
  set({
    ...(stsBufRef.current.length ? { status: stsBufRef.current[stsBufRef.current.length - 1] } : {}),
    ...(dmgBufRef.current.length ? { damage: dmgBufRef.current[dmgBufRef.current.length - 1] } : {}),
    currentStintStartTime,
  })
  const latestLap = lapProgressBufRef.current[lapProgressBufRef.current.length - 1] as LapRow | undefined
  if (latestLap) { lapState = latestLap; set({ lap: latestLap }) }
  recompute(dirtySliceForHistoryMask(payload.rowTypeMask))
  requestVisibleWindowHistory()
  if (authoritative) seekRendererPending = false
  if (authoritative && Number.isFinite(Number(payload.requestId)) && Number(payload.requestId) > 0)
    window.playerBridge.seekInstalled(Number(payload.requestId))
}

// ── Public API ────────────────────────────────────────────────────────────────

// Set the visible time window (seconds). Infinity selects the full-session
// publication used by All Laps and Stint Laps. Recomputes slices at once.
function requestVisibleWindowHistory(): void {
  if (allLapsMode || speedRpmBlocksVal === null) return
  if (finiteWindowBackfillEnabled && (!Number.isFinite(secondsVal) || secondsVal <= 0)) return
  const fileStart = Math.min(...speedRpmBlocksVal.map(block => Number(block.startSessionTime)).filter(Number.isFinite))
  const currentTime = Math.max(
    telBufRef.current[telBufRef.current.length - 1]?.session_time ?? 0,
    motBufRef.current[motBufRef.current.length - 1]?.session_time ?? 0,
    motExBufRef.current[motExBufRef.current.length - 1]?.session_time ?? 0,
    stsBufRef.current[stsBufRef.current.length - 1]?.session_time ?? 0,
    dmgBufRef.current[dmgBufRef.current.length - 1]?.session_time ?? 0,
    lapProgressBufRef.current[lapProgressBufRef.current.length - 1]?.session_time ?? 0,
  )
  if (!Number.isFinite(fileStart) || currentTime <= fileStart) return
  const requiredStart = finiteWindowBackfillEnabled
    ? Math.max(fileStart, currentTime - secondsVal)
    : Math.max(fileStart, lapStartTime)
  let missingMask = 0
  const missing = (bit: number, firstTime: number | undefined): void => {
    if ((historyRowMask & bit) && !historyCovers(bit, requiredStart) &&
        (firstTime ?? Infinity) > requiredStart + 1) missingMask |= bit
  }
  missing(HISTORY_ROW.telemetry, telBufRef.current[0]?.session_time)
  missing(HISTORY_ROW.status, stsBufRef.current[0]?.session_time)
  missing(HISTORY_ROW.damage, dmgBufRef.current[0]?.session_time)
  missing(HISTORY_ROW.motion, motBufRef.current[0]?.session_time)
  missing(HISTORY_ROW.motionEx, motExBufRef.current[0]?.session_time)
  missing(HISTORY_ROW.lap, lapProgressBufRef.current[0]?.session_time)
  if (missingMask !== 0) window.playerBridge.getWindowData(
    finiteWindowBackfillEnabled ? secondsVal : 0, missingMask)
}

export function setTelemetrySeconds(s: number, backfillFiniteWindow = true): void {
  const nextAllLapsMode = !Number.isFinite(s)
  const sameConfiguration = s === secondsVal && nextAllLapsMode === allLapsMode &&
    finiteWindowBackfillEnabled === backfillFiniteWindow
  finiteWindowBackfillEnabled = backfillFiniteWindow
  if (sameConfiguration) {
    requestVisibleWindowHistory()
    return
  }
  const wasAllLapsMode = allLapsMode
  secondsVal = s
  allLapsMode = nextAllLapsMode
  if (!allLapsMode) {
    waitingForAllLapsHistory = false
    // Normal playback can evict the old prefix from bounded renderer buffers.
    // A later AL entry must therefore be allowed to request it again.
    requestedHistoryRowMask = 0
  }
  set({ seconds: s })
  let requestHistory = false
  let entryMissingMask = 0
  if (allLapsMode && !wasAllLapsMode && speedRpmBlocksVal !== null) {
    const firstBlockStart = Math.min(...speedRpmBlocksVal.map(block => Number(block.startSessionTime)).filter(Number.isFinite))
    if (Number.isFinite(firstBlockStart)) {
      const missing = (bit: number, firstTime: number | undefined): void => {
        if ((historyRowMask & bit) && !historyCovers(bit, firstBlockStart) &&
            (firstTime ?? Infinity) > firstBlockStart + 1) entryMissingMask |= bit
      }
      missing(HISTORY_ROW.telemetry, telBufRef.current[0]?.session_time)
      missing(HISTORY_ROW.status, stsBufRef.current[0]?.session_time)
      missing(HISTORY_ROW.damage, dmgBufRef.current[0]?.session_time)
      missing(HISTORY_ROW.motion, motBufRef.current[0]?.session_time)
      missing(HISTORY_ROW.motionEx, motExBufRef.current[0]?.session_time)
      missing(HISTORY_ROW.lap, lapProgressBufRef.current[0]?.session_time)
    }
    requestedHistoryRowMask |= historyRowMask & ~entryMissingMask
    if (entryMissingMask !== 0) {
      waitingForAllLapsHistory = true
      requestHistory = true
    }
  }
  recompute(DirtySlice.All)
  if (requestHistory) {
    requestedHistoryRowMask |= entryMissingMask
    window.playerBridge.getAllLapsData(entryMissingMask)
  }
  requestVisibleWindowHistory()
}

// Sets the single coordinated minimum history requirement. While AL is active,
// newly-visible row families are requested additively; already-loaded families
// are not decompressed or delivered again.
export function setHistoryRowMask(mask: number): void {
  const normalized = mask >>> 0
  const disabled = historyRowMask & ~normalized
  historyRowMask = normalized
  requestedHistoryRowMask &= normalized
  const cleared: Partial<TelemetryStoreState> = {}
  if (disabled & HISTORY_ROW.telemetry) {
    telBufRef.current = telBufRef.current.length ? [telBufRef.current[telBufRef.current.length - 1]] : []
    cleared.telemetry = []
    cleared.analyzeLapTelemetry = []
  }
  if (disabled & HISTORY_ROW.status) {
    stsBufRef.current = stsBufRef.current.length ? [stsBufRef.current[stsBufRef.current.length - 1]] : []
    cleared.statusHistory = []
    cleared.analyzeLapStatusHistory = []
  }
  if (disabled & HISTORY_ROW.damage) {
    dmgBufRef.current = dmgBufRef.current.length ? [dmgBufRef.current[dmgBufRef.current.length - 1]] : []
    cleared.damageHistory = []
    cleared.analyzeLapDamageHistory = []
  }
  if (disabled & HISTORY_ROW.motion) {
    motBufRef.current = []
    cleared.motion = []
    cleared.analyzeLapMotion = []
  }
  if (disabled & HISTORY_ROW.motionEx) {
    motExBufRef.current = []
    cleared.motionEx = []
    cleared.analyzeLapMotionEx = []
  }
  if (Object.keys(cleared).length) set(cleared)
  if (!allLapsMode || speedRpmBlocksVal === null) return
  const missing = normalized & ~requestedHistoryRowMask
  if (missing === 0) return
  waitingForAllLapsHistory = true
  requestedHistoryRowMask = (requestedHistoryRowMask | missing) >>> 0
  window.playerBridge.getAllLapsData(missing)
}

// Subscribe to live race events (transient banners). Delivered synchronously as
// they stream in, so none are lost to React batching when several arrive at once.
export function subscribeRaceEvent(cb: (e: RaceEventMsg) => void): () => void {
  raceEventListeners.add(cb)
  return () => { raceEventListeners.delete(cb) }
}

// Full-lap charts consume the in-place full-session arrays directly. This
// imperative signal lets their WebGL bridges append new rows without cloning
// the growing arrays through Zustand/React on every telemetry batch.
export function subscribeAllLapsData(cb: () => void): () => void {
  allLapsDataListeners.add(cb)
  return () => { allLapsDataListeners.delete(cb) }
}

// Start the IPC subscription exactly once, at module load. The preload bridge is
// present before the bundle runs, and the store lives for the app's lifetime, so
// there is nothing to tear down.
let started = false
export function startTelemetryBridge(): void {
  if (started) return
  started = true

  window.playerBridge.onSeekStart((allHistory) => {
    // Freeze the published timeline immediately, before the seek IPC can race
    // with already-queued playback batches. Electron main holds all rows from
    // the new cursor until processPlaybackSeekFlush acknowledges installation.
    seekRendererPending = true
    if (!allHistory) return
    // Keep the currently published arrays intact while the worker extracts the
    // new prefix. Fresh post-seek rows accumulate separately and are merged by
    // the authoritative response.
    waitingForAllLapsHistory = true
    telBufRef.current = []
    motBufRef.current = []
    motExBufRef.current = []
    stsBufRef.current = []
    dmgBufRef.current = []
    lapProgressBufRef.current = []
  })

  window.telemetryBridge.onBatch((batchStr: string) => {
    if (seekRendererPending &&
        !batchStr.includes('"type":"playback_close"') &&
        !batchStr.includes('"type":"playback_loaded"')) return
    let dirty = DirtySlice.None
    let start = 0
    while (start < batchStr.length) {
      let end = batchStr.indexOf('\n', start)
      if (end === -1) end = batchStr.length
      if (end > start) {
        try {
          const msg = JSON.parse(batchStr.slice(start, end)) as GatewayMsg
          dirty |= dirtySliceFor(msg)
          handleMsg(msg)
        }
        catch (e) { console.error('Failed to parse batch JSON:', e) }
      }
      start = end + 1
    }
    recompute(dirty)
  })

  window.telemetryBridge.on((raw) => {
    const msg = raw as GatewayMsg
    handleMsg(msg)
    recompute(dirtySliceFor(msg))
  })

  window.telemetryBridge.onBinary((batch) => {
    if (seekRendererPending) return
    let dirty = DirtySlice.None
    try {
      forEachDecodedBinaryRow(batch, row => {
        const msg = row as GatewayMsg
        dirty |= dirtySliceFor(msg)
        handleMsg(msg)
      })
    } catch (e) { console.error('Failed to decode binary batch:', e) }
    recompute(dirty)
  })

  window.telemetryBridge.onResume(({ binary, coldJson }) => {
    if (seekRendererPending) return
    let dirty = DirtySlice.None
    try {
      forEachDecodedBinaryRow(binary, row => {
        const msg = row as GatewayMsg
        dirty |= dirtySliceFor(msg)
        handleMsg(msg)
      })
    } catch (e) { console.error('Failed to decode resume binary batch:', e) }

    let latestStatus: StatusRow | null = null
    let latestDamage: DamageRow | null = null
    let start = 0
    while (start < coldJson.length) {
      let end = coldJson.indexOf('\n', start)
      if (end === -1) end = coldJson.length
      if (end > start) {
        try {
          const msg = JSON.parse(coldJson.slice(start, end)) as StatusRow | DamageRow
          if (msg.type === 'status') {
            latestStatus = msg
            if (!isPlaybackFlag && Number.isFinite(msg.fuel_kg) && msg.fuel_kg >= 0 && msg.fuel_kg > fuelMaxReceived) {
              fuelMaxReceived = msg.fuel_kg
            }
            appendRow(stsBufRef, msg, MAX_ROWS)
            dirty |= DirtySlice.Status | DirtySlice.Derived
          } else if (msg.type === 'damage') {
            latestDamage = msg
            appendRow(dmgBufRef, msg, MAX_ROWS)
            dirty |= DirtySlice.Damage
          }
        }
        catch (e) { console.error('Failed to parse resume JSON:', e) }
      }
      start = end + 1
    }

    // Publish cold current-state values once after the bulk history append;
    // per-row Zustand writes here would turn a long resume window into a render
    // storm. Each source buffer is independently chronological, so the hot and
    // cold channels do not need a combined O(n log n) sort.
    const current: Partial<TelemetryStoreState> = {}
    if (latestStatus) current.status = latestStatus
    if (latestDamage) current.damage = latestDamage
    if (!isPlaybackFlag && fuelMaxReceived > -Infinity) current.fuelUpperLimit = fuelMaxReceived + 1
    if (Object.keys(current).length > 0) {
      set(current)
    }
    recompute(dirty)
  })
}

startTelemetryBridge()
