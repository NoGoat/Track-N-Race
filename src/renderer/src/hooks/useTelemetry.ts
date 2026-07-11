import { useState, useEffect, useRef, useMemo, useCallback } from 'react'
import type { TelemetryRow, MotionRow, MotionExRow, LapRow, StatusRow, DamageRow, TimingMsg, ParticipantsMsg, AllStatusMsg, RaceEventMsg, SessionMsg, TyreSetsMsg, GatewayMsg, LapData, SessionHistoryFastestMsg, ProtocolStatusMsg, ProtocolWarningMsg } from '../types'
import { decodeBinaryBatch } from '../lib/decodeBinaryBatch'

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

// Append a row to a ref-held buffer in place — O(1) amortized. The buffer may
// briefly hold up to TRIM_CHUNK rows older than the retention window; consumers
// slice by time so the excess is invisible. A session_time reversal (backward
// seek / session restart) rebuilds the buffer, matching the old getNextBuffer.
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

// Double-buffered window views: the visible time-window arrays are rebuilt every
// frame, so instead of slicing a fresh array each time we refill one of two
// persistent arrays (two, so consumers' identity-based memo deps still see a
// change each frame and the previous frame's array is never mutated under a
// holder mid-comparison).
interface WindowPool<T> { a: T[]; b: T[]; flip: boolean }

function makeWindowPool<T>(): WindowPool<T> {
  return { a: [], b: [], flip: false }
}

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

export interface TelemetryState {
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
  // Subscribe to live race events (used for transient banners). Events are
  // delivered synchronously as they stream in, so none are lost to React's
  // state batching when several arrive in one batch.
  onRaceEvent: (cb: (e: RaceEventMsg) => void) => () => void
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
}

export function useTelemetry(seconds: number): TelemetryState {
  // The hot 60 Hz buffers live in refs and are mutated in place (appendRow);
  // dataVersion is bumped once per delivered batch to publish them to React.
  // Keeping them in useState meant a full array copy per row plus a state
  // update per row, which was the dominant GC/render cost during playback.
  const [dataVersion, setDataVersion] = useState(0)
  const [status, setStatus]     = useState<StatusRow | null>(null)
  const [damage, setDamage]     = useState<DamageRow | null>(null)
  const [lap, setLap]           = useState<LapRow | null>(null)
  const [timing, setTiming]     = useState<TimingMsg | null>(null)
  const [participants, setParticipants] = useState<ParticipantsMsg | null>(null)
  const [allStatus, setAllStatus]       = useState<AllStatusMsg | null>(null)
  const [fastestLapCarIdx, setFastestLapCarIdx] = useState<number | null>(null)
  const [raceEvents, setRaceEvents] = useState<RaceEventMsg[]>([])
  const [session, setSession]       = useState<SessionMsg | null>(null)
  const [tyreSets, setTyreSets]   = useState<TyreSetsMsg | null>(null)
  const [lapHistoryBuf, setLapHistoryBuf] = useState<LapData[]>([])
  const [fastestLap, setFastestLap]       = useState<LapData | null>(null)
  const [protocolStatus, setProtocolStatus] = useState<ProtocolStatusMsg | null>(null)
  const [protocolWarning, setProtocolWarning] = useState<ProtocolWarningMsg | null>(null)

  // The engine emits protocol_status (labels/cardColors/format/aero_mode) only
  // once per detection change, so a renderer that mounts or reloads after the
  // format has already settled would never receive it and would fall back to
  // default labels with no card colours. Whenever we're in that catalog-less
  // state, pull the last status from main. Live format *changes* are still pushed
  // by the engine automatically, so this only fires on the fallback/initial case.
  useEffect(() => {
    if (!protocolStatus) window.protocolBridge.requestStatus()
  }, [protocolStatus])
  const [speedRpmBlocks, setSpeedRpmBlocks] = useState<any[] | null>(null)
  const [fastestLapNum, setFastestLapNum] = useState<number>(0)
  const [playbackEvents, setPlaybackEvents] = useState<RaceEventMsg[]>([])
  // Authoritative completed-lap times keyed by lap number. Playback comes from the
  // main-process pre-scan (seek-correct); live is accumulated as laps complete.
  const [playbackLapTimes, setPlaybackLapTimes] = useState<Record<number, number>>({})
  const [liveLapTimes, setLiveLapTimes] = useState<Record<number, number>>({})
  const fastestLapTimeRef = useRef<number>(Infinity)

  const telBufRef      = useRef<TelemetryRow[]>([])
  const motBufRef      = useRef<MotionRow[]>([])
  const motExBufRef    = useRef<MotionExRow[]>([])
  const dmgBufRef      = useRef<DamageRow[]>([])
  const stsBufRef      = useRef<StatusRow[]>([])
  const raceEventListenersRef = useRef(new Set<(e: RaceEventMsg) => void>())
  const poolsRef = useRef({
    tel:    makeWindowPool<TelemetryRow>(),
    mot:    makeWindowPool<MotionRow>(),
    motEx:  makeWindowPool<MotionExRow>(),
    sts:    makeWindowPool<StatusRow>(),
    dmg:    makeWindowPool<DamageRow>(),
    lapTel: makeWindowPool<TelemetryRow>(),
    lapSts: makeWindowPool<StatusRow>(),
  })
  const lapNumRef      = useRef<number | null>(null)
  const lapStartTimeRef = useRef<number>(0)
  const fastestLapSetRef      = useRef<boolean>(false)
  const sessionHistoryBestRef = useRef<Map<number, number>>(new Map())
  const isPlaybackRef = useRef<boolean>(false)

  useEffect(() => {
    const handleMsg = (msg: GatewayMsg) => {
      switch (msg.type) {
        case 'playback_close': {
          isPlaybackRef.current = false

          telBufRef.current = []
          motBufRef.current = []
          motExBufRef.current = []
          stsBufRef.current = []
          dmgBufRef.current = []
          setStatus(null)
          setDamage(null)
          setLap(null)
          setTiming(null)
          setAllStatus(null)
          setParticipants(null)
          setSession(null)
          setFastestLapCarIdx(null)
          setTyreSets(null)
          setLapHistoryBuf([])
          setFastestLap(null)
          fastestLapTimeRef.current = Infinity
          lapNumRef.current = null
          lapStartTimeRef.current = 0
          fastestLapSetRef.current = false
          sessionHistoryBestRef.current.clear()
          setSpeedRpmBlocks(null)
          setFastestLapNum(0)
          setPlaybackEvents([])
          setPlaybackLapTimes({})
          setLiveLapTimes({})
          break
        }
        case 'telemetry': {
          const buf = telBufRef.current
          const last = buf[buf.length - 1]
          if (last && msg.session_time < last.session_time) {
            setLapHistoryBuf(lapPrev =>
              lapPrev.filter(l => l.startSessionTime < msg.session_time)
            )
            setFastestLap(prev => {
              if (prev && prev.startSessionTime < msg.session_time) return prev
              fastestLapTimeRef.current = Infinity
              return null
            })
            setRaceEvents(prev =>
              prev.filter(e => e.session_time == null || e.session_time <= msg.session_time)
            )
            lapStartTimeRef.current = msg.session_time
            lapNumRef.current = null
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
          setStatus(msg)
          appendRow(stsBufRef, msg, MAX_ROWS)
          break
        case 'damage':
          setDamage(msg)
          appendRow(dmgBufRef, msg, MAX_ROWS)
          break
        case 'lap': {
          const lapMsg = msg as any
          setLap(lapMsg)
          break
        }
        case 'timing':       setTiming(msg);                       break
        case 'participants': setParticipants(msg);                 break
        case 'all_status':   setAllStatus(msg);                    break
        case 'fastest_lap':
          setFastestLapCarIdx(msg.car_idx)
          fastestLapSetRef.current = true
          break
        case 'session_history_fastest': {
          if (fastestLapSetRef.current) break
          const shMsg = msg as SessionHistoryFastestMsg
          sessionHistoryBestRef.current.set(shMsg.car_idx, shMsg.best_lap_time_ms)
          let minMs = Infinity
          let minIdx: number | null = null
          for (const [idx, ms] of sessionHistoryBestRef.current) {
            if (ms < minMs) { minMs = ms; minIdx = idx }
          }
          setFastestLapCarIdx(prev => prev === minIdx ? prev : minIdx)
          break
        }
        case 'tyre_sets':    setTyreSets(msg);                     break
        case 'race_event':
          // Banners are delivered through the listener channel so multiple events
          // in one batch each produce a banner without flushSync forcing a
          // synchronous whole-app render per event (a guaranteed frame hitch).
          for (const cb of raceEventListenersRef.current) cb(msg)
          setRaceEvents(prev => {
            const next = [...prev, msg]
            return next.length > MAX_RACE_EVENTS ? next.slice(-MAX_RACE_EVENTS) : next
          })
          if (msg.code === 'SEND') {
            if (!isPlaybackRef.current) {
              telBufRef.current = []
              motBufRef.current = []
              motExBufRef.current = []
              stsBufRef.current = []
              dmgBufRef.current = []
              setStatus(null)
              setDamage(null)
              setLap(null)
              setTiming(null)
              setAllStatus(null)
              setFastestLapCarIdx(null)
              setTyreSets(null)
              setLapHistoryBuf([])
              setFastestLap(null)
              fastestLapTimeRef.current = Infinity
              lapNumRef.current = null
              lapStartTimeRef.current = 0
              fastestLapSetRef.current = false
              sessionHistoryBestRef.current.clear()
              setSpeedRpmBlocks(null)
              setFastestLapNum(0)
              setLiveLapTimes({})
            }
          }
          break
        case 'session': setSession(msg); break
        case 'protocol_status': setProtocolStatus(msg); break
        case 'protocol_warning':
          if (msg.detected_format === null || msg.detected_format === undefined) {
            setProtocolWarning(null)
          } else {
            setProtocolWarning(msg)
          }
          break
        case 'playback_fastest_lap_raw':
        case 'playback_previous_lap_raw': {
          const payload = msg as any
          const batchStr = payload.data as string
          const lapInfo = payload.lapInfo
          
          let start = 0
          const tel: any[] = []
          const mot: any[] = []
          const sts: any[] = []
          
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
          
          const lapData = {
            lapNum: lapInfo.lapNum,
            startSessionTime: lapInfo.startSessionTime,
            endSessionTime: lapInfo.endSessionTime,
            telemetry: tel,
            motion: mot,
            statusHistory: sts
          }
          
          if (msg.type === 'playback_fastest_lap_raw') {
            setFastestLap(lapData as any)
            fastestLapTimeRef.current = lapData.lapNum > 0 ? (lapData.endSessionTime - lapData.startSessionTime) * 1000 : Infinity
          } else {
            setLapHistoryBuf(prev => [...prev.filter(l => l.lapNum !== lapData.lapNum), lapData as any].sort((a, b) => a.lapNum - b.lapNum).slice(-3))
          }
          break
        }
        case 'playback_seek_flush_bin': {
          const payload = msg as any
          lapStartTimeRef.current = payload.currentLapStart
          lapNumRef.current = payload.lapNum

          // Hot rows (telemetry/motion) arrive as binary — fast decode, no per-row
          // JSON.parse. Sparse cold rows (status/damage/lap) come as a tiny JSON blob.
          const tel: any[] = []
          const mot: any[] = []
          const motEx: any[] = []
          for (const row of decodeBinaryBatch(payload.binary)) {
            if (row.type === 'telemetry') tel.push(row)
            else if (row.type === 'motion') mot.push(row)
            else if (row.type === 'motion_ex') motEx.push(row)
          }

          const sts: any[] = []
          const dmg: any[] = []
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

          if (sts.length) setStatus(sts[sts.length - 1])
          if (dmg.length) setDamage(dmg[dmg.length - 1])
          if (lastLap) setLap(lastLap)
          break
        }
        case 'playback_lap_blocks': {
          isPlaybackRef.current = true
          const data = msg as any
          setSpeedRpmBlocks(data.blocks)
          setFastestLapNum(data.fastestLapNum)
          setPlaybackEvents(data.events ?? [])
          {
            const map: Record<number, number> = {}
            for (const l of (data.laps ?? []) as { lapNum: number; lapTimeMs: number }[]) {
              if (l.lapTimeMs > 0) map[l.lapNum] = l.lapTimeMs
            }
            setPlaybackLapTimes(map)
          }
          break
        }
      }
    }

    // The ref-held buffers are published to React with a single dataVersion bump
    // per delivered batch (batches arrive at the frame cadence), instead of a
    // state update per row.
    const unsubBatch = window.telemetryBridge.onBatch((batchStr: string) => {
      let start = 0
      while (start < batchStr.length) {
        let end = batchStr.indexOf('\n', start)
        if (end === -1) end = batchStr.length
        if (end > start) {
          try {
            const raw = JSON.parse(batchStr.slice(start, end))
            handleMsg(raw as GatewayMsg)
          } catch (e) {
            console.error('Failed to parse batch JSON:', e)
          }
        }
        start = end + 1
      }
      setDataVersion(v => v + 1)
    })

    const unsubOn = window.telemetryBridge.on((raw) => {
      handleMsg(raw as GatewayMsg)
      setDataVersion(v => v + 1)
    })

    // Live hot 60 Hz rows (telemetry/motion/motion_ex) arrive packed as binary.
    // Playback still delivers these as JSON via onBatch, so both paths feed handleMsg.
    const unsubBinary = window.telemetryBridge.onBinary((batch) => {
      try {
        const rows = decodeBinaryBatch(batch)
        for (const row of rows) handleMsg(row as GatewayMsg)
      } catch (e) {
        console.error('Failed to decode binary batch:', e)
      }
      setDataVersion(v => v + 1)
    })

    return () => {
      unsubBatch()
      unsubOn()
      unsubBinary()
    }
  }, [])

  const onRaceEvent = useCallback((cb: (e: RaceEventMsg) => void) => {
    raceEventListenersRef.current.add(cb)
    return () => { raceEventListenersRef.current.delete(cb) }
  }, [])

  useEffect(() => {
    if (!lap) return
    const prevLapNum = lapNumRef.current
    lapNumRef.current = lap.lap_num

    if (prevLapNum === null) {
      const buf = telBufRef.current
      const latestST = buf[buf.length - 1]?.session_time ?? 0
      lapStartTimeRef.current = lap.current_lap_ms > 0
        ? latestST - lap.current_lap_ms / 1000
        : latestST

      return
    }
    if (lap.lap_num === prevLapNum) return

    const prevLapStart = lapStartTimeRef.current
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

    setLapHistoryBuf(prev => [...prev, newLapData].slice(-3))
    const lapTimeMs = lap.last_lap_ms
    // last_lap_ms on this transition is the just-completed lap (prevLapNum).
    if (lapTimeMs > 0 && lapTimeMs < 300_000) {
      setLiveLapTimes(prev => prev[prevLapNum] === lapTimeMs ? prev : { ...prev, [prevLapNum]: lapTimeMs })
    }
    if (lapTimeMs > 0 && lapTimeMs < 300_000 && lapTimeMs < fastestLapTimeRef.current) {
      fastestLapTimeRef.current = lapTimeMs
      setFastestLap(newLapData)
    }
    lapStartTimeRef.current = endTime
  }, [lap])

  const latestSessionTime = telBufRef.current[telBufRef.current.length - 1]?.session_time ?? 0
  const cutoff = latestSessionTime - seconds
  const lapStartSessionTime = lap && lap.current_lap_ms > 0 ? lapStartTimeRef.current : 0
  const isPlayback = speedRpmBlocks !== null
  const activeLapNum = lap ? lap.lap_num : 1
  const curBlock = isPlayback ? speedRpmBlocks.find(b => b.lapNum === activeLapNum) : null
  const prevBlock = isPlayback ? speedRpmBlocks.find(b => b.lapNum === activeLapNum - 1) : null
  const fastBlock = isPlayback ? speedRpmBlocks.find(b => b.lapNum === fastestLapNum) : null
  // Playback uses the seek-correct pre-scan; live uses the accumulated map.
  const lapTimesByNum = isPlayback ? playbackLapTimes : liveLapTimes

  // Windowed views over the ref-held buffers: binary-search the boundary, then
  // refill a pooled array (no per-frame allocation). Keyed on dataVersion, which
  // bumps once per delivered batch.
  const pools = poolsRef.current
  const telemetry        = useMemo(() => {
    const buf = telBufRef.current
    return fillRange(pools.tel, buf, lowerBound(buf, cutoff, false), buf.length)
  }, [dataVersion, cutoff])
  const motion           = useMemo(() => {
    const buf = motBufRef.current
    return fillRange(pools.mot, buf, lowerBound(buf, cutoff, false), buf.length)
  }, [dataVersion, cutoff])
  const motionEx         = useMemo(() => {
    const buf = motExBufRef.current
    return fillRange(pools.motEx, buf, lowerBound(buf, cutoff, false), buf.length)
  }, [dataVersion, cutoff])
  const statusHistory    = useMemo(() => {
    const buf = stsBufRef.current
    return fillRange(pools.sts, buf, lowerBound(buf, cutoff, false), buf.length)
  }, [dataVersion, cutoff])
  const damageHistory    = useMemo(() => {
    const buf = dmgBufRef.current
    return fillRange(pools.dmg, buf, lowerBound(buf, cutoff, false), buf.length)
  }, [dataVersion, cutoff])
  const latest           = useMemo(() => {
    const buf = telBufRef.current
    return buf.length > 0 ? buf[buf.length - 1] : null
  }, [dataVersion])
  const lapHistory       = useMemo(() => isPlayback && prevBlock ? [prevBlock] : lapHistoryBuf, [isPlayback, prevBlock, lapHistoryBuf])
  const resolvedFastLap  = useMemo(() => isPlayback && fastBlock ? fastBlock : fastestLap,      [isPlayback, fastBlock, fastestLap])
  const lapTelemetry     = useMemo(() => {
    if (isPlayback && curBlock) {
      const src = curBlock.telemetry as TelemetryRow[]
      return fillRange(pools.lapTel, src, 0, lowerBound(src, latestSessionTime, false))
    }
    const buf = telBufRef.current
    return fillRange(pools.lapTel, buf, lowerBound(buf, lapStartSessionTime, true), buf.length)
  }, [dataVersion, isPlayback, curBlock, lapStartSessionTime, latestSessionTime])
  const lapStatusHistory = useMemo(() => {
    if (isPlayback && curBlock) {
      const src = curBlock.statusHistory as StatusRow[]
      return fillRange(pools.lapSts, src, 0, lowerBound(src, latestSessionTime, false))
    }
    const buf = stsBufRef.current
    return fillRange(pools.lapSts, buf, lowerBound(buf, lapStartSessionTime, true), buf.length)
  }, [dataVersion, isPlayback, curBlock, lapStartSessionTime, latestSessionTime])

  const resolvedRaceEvents = useMemo(
    () => isPlayback
      ? playbackEvents.filter(e => (e.session_time ?? 0) <= latestSessionTime)
      : raceEvents,
    [isPlayback, playbackEvents, raceEvents, latestSessionTime]
  )


  return {
    telemetry, motion, motionEx,
    status, statusHistory, damage, damageHistory,
    lap, timing, participants, allStatus,
    fastestLapCarIdx, onRaceEvent, raceEvents: resolvedRaceEvents, session, tyreSets,
    latest, lapHistory, fastestLap: resolvedFastLap, lapTelemetry, lapStatusHistory,
    speedRpmBlocks, lapTimesByNum,
    isConnected: true, error: null, protocolStatus, protocolWarning,
  }
}
