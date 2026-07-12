import { create } from 'zustand'
import type {
  TelemetryRow, MotionRow, MotionExRow, LapRow, StatusRow, DamageRow, TimingMsg,
  ParticipantsMsg, AllStatusMsg, RaceEventMsg, SessionMsg, TyreSetsMsg, GatewayMsg,
  LapData, SessionHistoryFastestMsg, ProtocolStatusMsg, ProtocolWarningMsg,
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
  lapTimesByNum: Record<number, number>
  speedRpmBlocks: any[] | null
  isConnected: boolean
  error: string | null
  protocolStatus: ProtocolStatusMsg | null
  protocolWarning: ProtocolWarningMsg | null
  seconds: number
}

export const useTelemetryStore = create<TelemetryStoreState>()(() => ({
  telemetry: [], motion: [], motionEx: [],
  status: null, statusHistory: [], damage: null, damageHistory: [],
  lap: null, timing: null, participants: null, allStatus: null,
  fastestLapCarIdx: null, raceEvents: [], session: null, tyreSets: null,
  latest: null, lapHistory: [], fastestLap: null, lapTelemetry: [], lapStatusHistory: [],
  lapTimesByNum: {}, speedRpmBlocks: null, isConnected: true, error: null,
  protocolStatus: null, protocolWarning: null, seconds: 30,
}))

const set = useTelemetryStore.setState

// ── Working state (never published; the source of truth for computation) ──────
const telBufRef   = { current: [] as TelemetryRow[] }
const motBufRef   = { current: [] as MotionRow[] }
const motExBufRef = { current: [] as MotionExRow[] }
const dmgBufRef   = { current: [] as DamageRow[] }
const stsBufRef   = { current: [] as StatusRow[] }

const pools = {
  tel:    makeWindowPool<TelemetryRow>(),
  mot:    makeWindowPool<MotionRow>(),
  motEx:  makeWindowPool<MotionExRow>(),
  sts:    makeWindowPool<StatusRow>(),
  dmg:    makeWindowPool<DamageRow>(),
  lapTel: makeWindowPool<TelemetryRow>(),
  lapSts: makeWindowPool<StatusRow>(),
}

const raceEventListeners = new Set<(e: RaceEventMsg) => void>()

let lapState: LapRow | null = null
let lapNum: number | null = null
let lapStartTime = 0
let fastestLapTime = Infinity
let fastestLapSet = false
const sessionHistoryBest = new Map<number, number>()
let isPlaybackFlag = false

let lapHistoryBuf: LapData[] = []
let fastestLapVal: LapData | null = null
let raceEventsArr: RaceEventMsg[] = []
let speedRpmBlocksVal: any[] | null = null
let fastestLapNum = 0
let playbackEvents: RaceEventMsg[] = []
let playbackLapTimes: Record<number, number> = {}
let liveLapTimes: Record<number, number> = {}

let secondsVal = 30

function resetSession(): void {
  telBufRef.current = []
  motBufRef.current = []
  motExBufRef.current = []
  stsBufRef.current = []
  dmgBufRef.current = []
  lapState = null; lapNum = null; lapStartTime = 0
  fastestLapTime = Infinity; fastestLapSet = false
  sessionHistoryBest.clear()
  lapHistoryBuf = []; fastestLapVal = null; raceEventsArr = []
  speedRpmBlocksVal = null; fastestLapNum = 0
  playbackEvents = []; playbackLapTimes = {}; liveLapTimes = {}
  set({
    status: null, damage: null, lap: null, timing: null, allStatus: null,
    participants: null, session: null, fastestLapCarIdx: null, tyreSets: null,
    lapHistory: [], fastestLap: null, speedRpmBlocks: null, raceEvents: [],
  })
}

// The old useEffect([lap]): on a lap-number change, snapshot the completed lap
// and update live lap times / fastest lap. Runs in the 'lap' handler now.
function onLap(lap: LapRow): void {
  lapState = lap
  set({ lap })
  const prevLapNum = lapNum
  lapNum = lap.lap_num

  if (prevLapNum === null) {
    const buf = telBufRef.current
    const latestST = buf[buf.length - 1]?.session_time ?? 0
    lapStartTime = lap.current_lap_ms > 0 ? latestST - lap.current_lap_ms / 1000 : latestST
    return
  }
  if (lap.lap_num === prevLapNum) return

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
      if (last && msg.session_time < last.session_time) {
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
    case 'status':
      set({ status: msg })
      appendRow(stsBufRef, msg, MAX_ROWS)
      break
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
      lapStartTime = payload.currentLapStart
      lapNum = payload.lapNum
      const tel: any[] = [], mot: any[] = [], motEx: any[] = []
      for (const row of decodeBinaryBatch(payload.binary)) {
        if (row.type === 'telemetry') tel.push(row)
        else if (row.type === 'motion') mot.push(row)
        else if (row.type === 'motion_ex') motEx.push(row)
      }
      const sts: any[] = [], dmg: any[] = []
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
            else if (row.type === 'lap') lastLap = row
          } catch (e) {}
        }
        start = end + 1
      }
      telBufRef.current = tel
      motBufRef.current = mot
      motExBufRef.current = motEx
      stsBufRef.current = sts
      dmgBufRef.current = dmg
      if (sts.length) set({ status: sts[sts.length - 1] })
      if (dmg.length) set({ damage: dmg[dmg.length - 1] })
      if (lastLap) { lapState = lastLap; set({ lap: lastLap }) }
      break
    }
    case 'playback_lap_blocks': {
      isPlaybackFlag = true
      const data = msg as any
      speedRpmBlocksVal = data.blocks
      fastestLapNum = data.fastestLapNum
      playbackEvents = data.events ?? []
      const map: Record<number, number> = {}
      for (const l of (data.laps ?? []) as { lapNum: number; lapTimeMs: number }[]) {
        if (l.lapTimeMs > 0) map[l.lapNum] = l.lapTimeMs
      }
      playbackLapTimes = map
      set({ speedRpmBlocks: speedRpmBlocksVal })
      break
    }
  }
}

// Recompute the windowed + derived slices from the buffers and publish them.
// Replaces the old per-render useMemos + the dataVersion bump.
function recompute(): void {
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

  const telemetry   = fillRange(pools.tel,   telBuf, lowerBound(telBuf, cutoff, false), telBuf.length)
  const motion      = fillRange(pools.mot,   motBufRef.current, lowerBound(motBufRef.current, cutoff, false), motBufRef.current.length)
  const motionEx    = fillRange(pools.motEx, motExBufRef.current, lowerBound(motExBufRef.current, cutoff, false), motExBufRef.current.length)
  const statusHistory = fillRange(pools.sts, stsBufRef.current, lowerBound(stsBufRef.current, cutoff, false), stsBufRef.current.length)
  const damageHistory = fillRange(pools.dmg, dmgBufRef.current, lowerBound(dmgBufRef.current, cutoff, false), dmgBufRef.current.length)
  const latest = telBuf.length > 0 ? telBuf[telBuf.length - 1] : null

  const lapHistory = isPlayback && prevBlock ? [prevBlock] : lapHistoryBuf
  const fastestLap = isPlayback && fastBlock ? fastBlock : fastestLapVal

  let lapTelemetry: TelemetryRow[]
  let lapStatusHistory: StatusRow[]
  if (isPlayback && curBlock) {
    const srcT = curBlock.telemetry as TelemetryRow[]
    lapTelemetry = fillRange(pools.lapTel, srcT, 0, lowerBound(srcT, latestSessionTime, false))
    const srcS = curBlock.statusHistory as StatusRow[]
    lapStatusHistory = fillRange(pools.lapSts, srcS, 0, lowerBound(srcS, latestSessionTime, false))
  } else {
    lapTelemetry = fillRange(pools.lapTel, telBuf, lowerBound(telBuf, lapStartSessionTime, true), telBuf.length)
    lapStatusHistory = fillRange(pools.lapSts, stsBufRef.current, lowerBound(stsBufRef.current, lapStartSessionTime, true), stsBufRef.current.length)
  }

  const raceEvents = isPlayback
    ? playbackEvents.filter(e => (e.session_time ?? 0) <= latestSessionTime)
    : raceEventsArr

  set({
    telemetry, motion, motionEx, statusHistory, damageHistory, latest,
    lapHistory, fastestLap, lapTelemetry, lapStatusHistory, lapTimesByNum, raceEvents,
  })
}

// ── Public API ────────────────────────────────────────────────────────────────

// Set the visible time window (seconds). Recomputes the windowed slices at once.
export function setTelemetrySeconds(s: number): void {
  if (s === secondsVal) return
  secondsVal = s
  set({ seconds: s })
  recompute()
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
    let start = 0
    while (start < batchStr.length) {
      let end = batchStr.indexOf('\n', start)
      if (end === -1) end = batchStr.length
      if (end > start) {
        try { handleMsg(JSON.parse(batchStr.slice(start, end)) as GatewayMsg) }
        catch (e) { console.error('Failed to parse batch JSON:', e) }
      }
      start = end + 1
    }
    recompute()
  })

  window.telemetryBridge.on((raw) => {
    handleMsg(raw as GatewayMsg)
    recompute()
  })

  window.telemetryBridge.onBinary((batch) => {
    try {
      for (const row of decodeBinaryBatch(batch)) handleMsg(row as GatewayMsg)
    } catch (e) { console.error('Failed to decode binary batch:', e) }
    recompute()
  })
}

startTelemetryBridge()
