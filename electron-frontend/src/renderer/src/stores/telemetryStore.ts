import { create } from 'zustand'
import type {
  TelemetryRow, MotionRow, MotionExRow, LapRow, StatusRow, DamageRow, TimingMsg,
  ParticipantsMsg, AllStatusMsg, RaceEventMsg, SessionMsg, TyreSetsMsg, GatewayMsg,
  LapData, LapProgressPoint, SessionHistoryFastestMsg, ProtocolStatusMsg, ProtocolWarningMsg,
  AnalyzeLapData, PlaybackLapDataMsg,
} from '../types'
import { decodeBinaryBatch } from '../lib/decodeBinaryBatch'

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
const RETENTION_S = 600 // 10 minutes
const MAX_RACE_EVENTS = 1000
const MAX_ANALYZE_LAP_CACHE = 6
// Rows older than the retention window are only dropped once this many have
// accumulated, so trimming is one slice every ~30s instead of work on every row.
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

// Append a row to a buffer in place — O(1) amortized. The buffer may briefly hold
// up to TRIM_CHUNK rows older than the retention window; consumers slice by time
// so the excess is invisible. A session_time reversal rebuilds the buffer.
function appendRow<T extends { session_time: number }>(ref: { current: T[] }, msg: T, maxRows: number): void {
  const buf = ref.current
  const last = buf[buf.length - 1]
  const cutoff = msg.session_time - RETENTION_S
  if (last && msg.session_time < last.session_time) {
    const rebuilt = buf.filter(d => d.session_time < msg.session_time && d.session_time >= cutoff)
    rebuilt.push(msg)
    ref.current = rebuilt
    return
  }
  buf.push(msg)
  const firstValid = lowerBound(buf, cutoff, true)
  if (firstValid >= TRIM_CHUNK || buf.length > maxRows) {
    ref.current = buf.slice(Math.max(firstValid, buf.length - maxRows))
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
  latest: TelemetryRow | null
  lapHistory: LapData[]
  fastestLap: LapData | null
  lapTelemetry: TelemetryRow[]
  lapStatusHistory: StatusRow[]
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
  playbackLapDataCache: Record<number, AnalyzeLapData>
  lapTimesByNum: Record<number, number>
  speedRpmBlocks: any[] | null
  isConnected: boolean
  error: string | null
  protocolStatus: ProtocolStatusMsg | null
  protocolWarning: ProtocolWarningMsg | null
  fuelUpperLimit: number | null
  seconds: number
}

export const useTelemetryStore = create<TelemetryStoreState>()(() => ({
  telemetry: [], motion: [], motionEx: [],
  status: null, statusHistory: [], damage: null, damageHistory: [],
  lap: null, timing: null, participants: null, allStatus: null,
  fastestLapCarIdx: null, raceEvents: [], session: null, tyreSets: null,
  latest: null, lapHistory: [], fastestLap: null, lapTelemetry: [], lapStatusHistory: [],
  analyzeLapTelemetry: [], analyzeLapMotion: [], analyzeLapMotionEx: [],
  analyzeLapStatusHistory: [], analyzeLapDamageHistory: [], analyzeLapProgress: [], analyzeLapStartTime: 0,
  analyzeLapRevision: 0,
  analyzeDeltaAvailable: false, analyzeTrackLengthM: 0, playbackTnrdVersion: null,
  playbackLapDataCache: {},
  lapTimesByNum: {}, speedRpmBlocks: null, isConnected: true, error: null,
  protocolStatus: null, protocolWarning: null, fuelUpperLimit: null, seconds: 30,
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
  lapTel: makeWindowPool<TelemetryRow>(),
  lapSts: makeWindowPool<StatusRow>(),
  analyzeTel:   makeWindowPool<TelemetryRow>(),
  analyzeMot:   makeWindowPool<MotionRow>(),
  analyzeMotEx: makeWindowPool<MotionExRow>(),
  analyzeSts:   makeWindowPool<StatusRow>(),
  analyzeDmg:   makeWindowPool<DamageRow>(),
  analyzeLapProgress: makeWindowPool<LapProgressPoint>(),
}

const raceEventListeners = new Set<(e: RaceEventMsg) => void>()

let lapState: LapRow | null = null
let lapNum: number | null = null
let lapStartTime = 0
let fastestLapTime = Infinity
let fastestLapSet = false
const sessionHistoryBest = new Map<number, number>()
let isPlaybackFlag = false
let fuelMaxReceived = -Infinity

let lapHistoryBuf: LapData[] = []
let fastestLapVal: LapData | null = null
let raceEventsArr: RaceEventMsg[] = []
let speedRpmBlocksVal: any[] | null = null
let fastestLapNum = 0
let playbackEvents: RaceEventMsg[] = []
let playbackLapTimes: Record<number, number> = {}
let liveLapTimes: Record<number, number> = {}
let playbackLapCacheOrder: number[] = []
let analyzeLapRevisionVal = 0

let secondsVal = 30

function resetSession(): void {
  analyzeLapRevisionVal++
  telBufRef.current = []
  motBufRef.current = []
  motExBufRef.current = []
  stsBufRef.current = []
  dmgBufRef.current = []
  lapProgressBufRef.current = []
  lapState = null; lapNum = null; lapStartTime = 0
  fastestLapTime = Infinity; fastestLapSet = false
  sessionHistoryBest.clear()
  lapHistoryBuf = []; fastestLapVal = null; raceEventsArr = []
  speedRpmBlocksVal = null; fastestLapNum = 0
  playbackEvents = []; playbackLapTimes = {}; liveLapTimes = {}
  playbackLapCacheOrder = []
  fuelMaxReceived = -Infinity
  set({
    status: null, damage: null, lap: null, timing: null, allStatus: null,
    participants: null, session: null, fastestLapCarIdx: null, tyreSets: null,
    lapHistory: [], fastestLap: null, speedRpmBlocks: null, raceEvents: [], fuelUpperLimit: null,
    analyzeLapTelemetry: [], analyzeLapMotion: [], analyzeLapMotionEx: [],
    analyzeLapStatusHistory: [], analyzeLapDamageHistory: [], analyzeLapProgress: [], analyzeLapStartTime: 0,
    analyzeLapRevision: analyzeLapRevisionVal,
    analyzeDeltaAvailable: false, analyzeTrackLengthM: 0, playbackTnrdVersion: null,
    playbackLapDataCache: {},
  })
}

// The old useEffect([lap]): on a lap-number change, snapshot the completed lap
// and update live lap times / fastest lap. Runs in the 'lap' handler now.
function onLap(lap: LapRow): void {
  if (Number.isFinite(lap.lap_distance_m)) appendRow(lapProgressBufRef, lap, MAX_ROWS)
  lapState = lap
  set({ lap })
  const prevLapNum = lapNum
  lapNum = lap.lap_num

  if (prevLapNum === null) {
    const buf = telBufRef.current
    const latestST = buf[buf.length - 1]?.session_time ?? 0
    lapStartTime = lap.current_lap_ms > 0 ? latestST - lap.current_lap_ms / 1000 : latestST
    // Initial lap metadata (including metadata re-emitted after a playback
    // backfill) establishes the lap origin; it is not a chart reset boundary.
    // Recording load/seek/session reset already publish an explicit revision.
    return
  }
  if (lap.lap_num === prevLapNum) return

  analyzeLapRevisionVal++

  const prevLapStart = lapStartTime
  const buf = telBufRef.current
  const endTime = buf[buf.length - 1]?.session_time ?? 0
  const newLapData: LapData = {
    lapNum:           prevLapNum,
    startSessionTime: prevLapStart,
    endSessionTime:   endTime,
    telemetry:        telBufRef.current.filter(d => d.session_time >= prevLapStart),
    motion:           motBufRef.current.filter(d => d.session_time >= prevLapStart),
    statusHistory:    stsBufRef.current.filter(d => d.session_time >= prevLapStart),
  }

  lapHistoryBuf = [...lapHistoryBuf, newLapData].slice(-3)
  const lapTimeMs = lap.last_lap_ms
  if (lapTimeMs > 0 && lapTimeMs < 300_000) {
    if (liveLapTimes[prevLapNum] !== lapTimeMs) liveLapTimes = { ...liveLapTimes, [prevLapNum]: lapTimeMs }
  }
  if (lapTimeMs > 0 && lapTimeMs < 300_000 && lapTimeMs < fastestLapTime) {
    fastestLapTime = lapTimeMs
    fastestLapVal = newLapData
  }
  lapStartTime = endTime
}

function handleMsg(msg: GatewayMsg): void {
  switch (msg.type) {
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
        lapHistoryBuf = lapHistoryBuf.filter(l => l.startSessionTime < msg.session_time)
        if (!(fastestLapVal && fastestLapVal.startSessionTime < msg.session_time)) {
          fastestLapTime = Infinity
          fastestLapVal = null
        }
        raceEventsArr = raceEventsArr.filter(e => e.session_time == null || e.session_time <= msg.session_time)
        lapStartTime = msg.session_time
        lapNum = null
      }
      appendRow(telBufRef, msg, MAX_ROWS)
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
      if (!isPlaybackFlag && Number.isFinite(msg.fuel_kg) && msg.fuel_kg >= 0 && msg.fuel_kg > fuelMaxReceived) {
        fuelMaxReceived = msg.fuel_kg
        // Keep exactly one kilogram of breathing room above the highest value
        // received in this live session.
        next.fuelUpperLimit = fuelMaxReceived + 1
      }
      set(next)
      appendRow(stsBufRef, msg, MAX_ROWS)
      break
    }
    case 'damage':
      set({ damage: msg })
      appendRow(dmgBufRef, msg, MAX_ROWS)
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
    case 'race_event':
      for (const cb of raceEventListeners) cb(msg as RaceEventMsg)
      raceEventsArr = [...raceEventsArr, msg as RaceEventMsg]
      if (raceEventsArr.length > MAX_RACE_EVENTS) raceEventsArr = raceEventsArr.slice(-MAX_RACE_EVENTS)
      if ((msg as any).code === 'SEND' && !isPlaybackFlag) {
        resetSession()
      }
      break
    case 'session': set({ session: msg }); break
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
      }
      playbackLapCacheOrder = [...playbackLapCacheOrder.filter(lapNum => lapNum !== lapData.lapNum), lapData.lapNum]
      set(state => {
        const cache = { ...state.playbackLapDataCache, [lapData.lapNum]: lapData }
        while (playbackLapCacheOrder.length > MAX_ANALYZE_LAP_CACHE) {
          const evicted = playbackLapCacheOrder.shift()
          if (evicted !== undefined) delete cache[evicted]
        }
        return { playbackLapDataCache: cache }
      })
      break
    }
    case 'playback_fastest_lap_raw':
    case 'playback_previous_lap_raw': {
      const payload = msg as any
      const batchStr = payload.data as string
      const lapInfo = payload.lapInfo
      let start = 0
      const tel: any[] = [], mot: any[] = [], sts: any[] = []
      while (start < batchStr.length) {
        let end = batchStr.indexOf('\n', start)
        if (end === -1) end = batchStr.length
        if (end > start) {
          try {
            const row = JSON.parse(batchStr.slice(start, end))
            if (row.type === 'telemetry') tel.push(row)
            else if (row.type === 'motion') mot.push(row)
            else if (row.type === 'status') sts.push(row)
          } catch (e) {}
        }
        start = end + 1
      }
      const lapData: any = {
        lapNum: lapInfo.lapNum, startSessionTime: lapInfo.startSessionTime,
        endSessionTime: lapInfo.endSessionTime, telemetry: tel, motion: mot, statusHistory: sts,
      }
      if ((msg as any).type === 'playback_fastest_lap_raw') {
        fastestLapVal = lapData
        fastestLapTime = lapData.lapNum > 0 ? (lapData.endSessionTime - lapData.startSessionTime) * 1000 : Infinity
      } else {
        lapHistoryBuf = [...lapHistoryBuf.filter(l => l.lapNum !== lapData.lapNum), lapData]
          .sort((a, b) => a.lapNum - b.lapNum).slice(-3)
      }
      break
    }
    case 'playback_seek_flush_bin': {
      const payload = msg as any
      analyzeLapRevisionVal++
      lapStartTime = payload.currentLapStart
      lapNum = payload.lapNum
      const tel: any[] = [], mot: any[] = [], motEx: any[] = []
      for (const row of decodeBinaryBatch(payload.binary)) {
        if (row.type === 'telemetry') tel.push(row)
        else if (row.type === 'motion') mot.push(row)
        else if (row.type === 'motion_ex') motEx.push(row)
      }
      const sts: any[] = [], dmg: any[] = [], lapProgress: LapProgressPoint[] = []
      let lastLap: any = null
      const coldJson = (payload.coldJson as string) || ''
      let start = 0
      while (start < coldJson.length) {
        let end = coldJson.indexOf('\n', start)
        if (end === -1) end = coldJson.length
        if (end > start) {
          try {
            const row = JSON.parse(coldJson.slice(start, end))
            if (row.type === 'status') sts.push(row)
            else if (row.type === 'damage') dmg.push(row)
            else if (row.type === 'lap') { lastLap = row; lapProgress.push(row) }
          } catch (e) {}
        }
        start = end + 1
      }
      telBufRef.current = tel
      motBufRef.current = mot
      motExBufRef.current = motEx
      stsBufRef.current = sts
      dmgBufRef.current = dmg
      lapProgressBufRef.current = lapProgress
      if (sts.length) set({ status: sts[sts.length - 1] })
      if (dmg.length) set({ damage: dmg[dmg.length - 1] })
      if (lastLap) { lapState = lastLap; set({ lap: lastLap }) }
      break
    }
    case 'playback_lap_blocks': {
      isPlaybackFlag = true
      analyzeLapRevisionVal++
      const data = msg as any
      fuelMaxReceived = -Infinity
      playbackLapCacheOrder = []
      speedRpmBlocksVal = data.blocks
      fastestLapNum = data.fastestLapNum
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
        playbackLapDataCache: {},
        fuelUpperLimit: Number.isFinite(initialFuelKg) && initialFuelKg >= 0
          ? initialFuelKg + 1
          : null,
        analyzeDeltaAvailable: data.deltaAvailable === true && data.tnrdVersion === 'TNRD_V3',
        analyzeTrackLengthM: Number.isFinite(trackLengthM) && trackLengthM > 0 ? trackLengthM : 0,
        playbackTnrdVersion: typeof data.tnrdVersion === 'string' ? data.tnrdVersion : null,
      })
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
    case 'playback_fastest_lap_raw':
    case 'playback_previous_lap_raw':
    case 'playback_lap_blocks': return DirtySlice.Derived
    case 'playback_close':
    case 'playback_seek_flush_bin': return DirtySlice.All
    default: return DirtySlice.None
  }
}

// Recompute only the window groups touched by a delivered batch. Unchanged
// groups retain their array identity, so their Zustand subscribers stay cold.
function recompute(dirty: DirtySlice): void {
  if (dirty === DirtySlice.None) return
  const telBuf = telBufRef.current
  const latestSessionTime = telBuf[telBuf.length - 1]?.session_time ?? 0
  const cutoff = latestSessionTime - secondsVal
  const lapStartSessionTime = lapState && lapState.current_lap_ms > 0 ? lapStartTime : 0
  const isPlayback = speedRpmBlocksVal !== null
  const activeLapNum = lapState ? lapState.lap_num : 1
  const curBlock  = isPlayback ? speedRpmBlocksVal!.find(b => b.lapNum === activeLapNum) : null
  const prevBlock = isPlayback ? speedRpmBlocksVal!.find(b => b.lapNum === activeLapNum - 1) : null
  const fastBlock = isPlayback ? speedRpmBlocksVal!.find(b => b.lapNum === fastestLapNum) : null
  const lapTimesByNum = isPlayback ? playbackLapTimes : liveLapTimes

  const next: Partial<TelemetryStoreState> = {}
  if (dirty & DirtySlice.Telemetry) {
    next.telemetry = fillRange(pools.tel, telBuf, lowerBound(telBuf, cutoff, false), telBuf.length)
    next.latest = telBuf.length > 0 ? telBuf[telBuf.length - 1] : null
    next.analyzeLapTelemetry = fillRange(pools.analyzeTel, telBuf, lowerBound(telBuf, lapStartSessionTime, true), telBuf.length)
  }
  if (dirty & DirtySlice.Motion) {
    const buf = motBufRef.current
    next.motion = fillRange(pools.mot, buf, lowerBound(buf, cutoff, false), buf.length)
    next.analyzeLapMotion = fillRange(pools.analyzeMot, buf, lowerBound(buf, lapStartSessionTime, true), buf.length)
  }
  if (dirty & DirtySlice.MotionEx) {
    const buf = motExBufRef.current
    next.motionEx = fillRange(pools.motEx, buf, lowerBound(buf, cutoff, false), buf.length)
    next.analyzeLapMotionEx = fillRange(pools.analyzeMotEx, buf, lowerBound(buf, lapStartSessionTime, true), buf.length)
  }
  if (dirty & DirtySlice.Status) {
    const buf = stsBufRef.current
    next.statusHistory = fillRange(pools.sts, buf, lowerBound(buf, cutoff, false), buf.length)
    next.analyzeLapStatusHistory = fillRange(pools.analyzeSts, buf, lowerBound(buf, lapStartSessionTime, true), buf.length)
  }
  if (dirty & DirtySlice.Damage) {
    const buf = dmgBufRef.current
    next.damageHistory = fillRange(pools.dmg, buf, lowerBound(buf, cutoff, false), buf.length)
    next.analyzeLapDamageHistory = fillRange(pools.analyzeDmg, buf, lowerBound(buf, lapStartSessionTime, true), buf.length)
  }
  if (dirty & DirtySlice.Lap) {
    const buf = lapProgressBufRef.current
    next.analyzeLapProgress = fillRange(
      pools.analyzeLapProgress, buf, lowerBound(buf, lapStartSessionTime, true), buf.length)
  }
  if (dirty & DirtySlice.Derived) {
    next.lapHistory = isPlayback && prevBlock ? [prevBlock] : lapHistoryBuf
    next.fastestLap = isPlayback && fastBlock ? fastBlock : fastestLapVal
    if (isPlayback && curBlock) {
      const srcT = curBlock.telemetry as TelemetryRow[]
      next.lapTelemetry = fillRange(pools.lapTel, srcT, 0, lowerBound(srcT, latestSessionTime, false))
      const srcS = curBlock.statusHistory as StatusRow[]
      next.lapStatusHistory = fillRange(pools.lapSts, srcS, 0, lowerBound(srcS, latestSessionTime, false))
    } else {
      next.lapTelemetry = fillRange(pools.lapTel, telBuf, lowerBound(telBuf, lapStartSessionTime, true), telBuf.length)
      next.lapStatusHistory = fillRange(pools.lapSts, stsBufRef.current, lowerBound(stsBufRef.current, lapStartSessionTime, true), stsBufRef.current.length)
    }
    next.lapTimesByNum = lapTimesByNum
    next.raceEvents = isPlayback
      ? playbackEvents.filter(e => (e.session_time ?? 0) <= latestSessionTime)
      : raceEventsArr
    next.analyzeLapStartTime = lapStartSessionTime
    next.analyzeLapRevision = analyzeLapRevisionVal
  }
  set(next)
}

// ── Public API ────────────────────────────────────────────────────────────────

// Set the visible time window (seconds). Recomputes the windowed slices at once.
export function setTelemetrySeconds(s: number): void {
  if (s === secondsVal) return
  secondsVal = s
  set({ seconds: s })
  recompute(DirtySlice.All)
}

// Subscribe to live race events (transient banners). Delivered synchronously as
// they stream in, so none are lost to React batching when several arrive at once.
export function subscribeRaceEvent(cb: (e: RaceEventMsg) => void): () => void {
  raceEventListeners.add(cb)
  return () => { raceEventListeners.delete(cb) }
}

// Start the IPC subscription exactly once, at module load. The preload bridge is
// present before the bundle runs, and the store lives for the app's lifetime, so
// there is nothing to tear down.
let started = false
export function startTelemetryBridge(): void {
  if (started) return
  started = true

  window.telemetryBridge.onBatch((batchStr: string) => {
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
    let dirty = DirtySlice.None
    try {
      for (const row of decodeBinaryBatch(batch)) {
        const msg = row as GatewayMsg
        dirty |= dirtySliceFor(msg)
        handleMsg(msg)
      }
    } catch (e) { console.error('Failed to decode binary batch:', e) }
    recompute(dirty)
  })

  window.telemetryBridge.onResume(({ binary, coldJson }) => {
    let dirty = DirtySlice.None
    try {
      for (const row of decodeBinaryBatch(binary)) {
        const msg = row as GatewayMsg
        dirty |= dirtySliceFor(msg)
        handleMsg(msg)
      }
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
