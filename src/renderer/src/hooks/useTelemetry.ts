import { useState, useEffect, useRef, useMemo } from 'react'
import { flushSync } from 'react-dom'
import type { TelemetryRow, MotionRow, MotionExRow, LapRow, StatusRow, DamageRow, TimingMsg, ParticipantsMsg, AllStatusMsg, RaceEventMsg, SessionMsg, TyreSetsMsg, GatewayMsg, LapData, SessionHistoryFastestMsg, ProtocolStatusMsg, ProtocolWarningMsg } from '../types'
import { decodeBinaryBatch } from '../lib/decodeBinaryBatch'

const MAX_ROWS = 750000

function getNextBuffer<T extends { session_time: number }>(prev: T[], msg: T, maxRows: number): T[] {
  const last = prev[prev.length - 1]
  const cutoffTime = msg.session_time - 600 // 10 minutes

  let validPrev = prev
  if (last && msg.session_time < last.session_time) {
    validPrev = prev.filter(d => d.session_time < msg.session_time && d.session_time >= cutoffTime)
  } else {
    let l = 0, r = prev.length - 1
    let firstValidIdx = prev.length
    while (l <= r) {
      const mid = (l + r) >> 1
      if (prev[mid].session_time >= cutoffTime) {
        firstValidIdx = mid
        r = mid - 1
      } else {
        l = mid + 1
      }
    }
    let dropCount = firstValidIdx
    const overflow = prev.length + 1 - maxRows
    if (dropCount < overflow) dropCount = overflow

    validPrev = dropCount > 0 ? prev.slice(dropCount) : prev
  }

  return [...validPrev, msg]
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
  raceEvent: RaceEventMsg | null
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
  const [telBuf, setTelBuf]     = useState<TelemetryRow[]>([])
  const [motBuf, setMotBuf]     = useState<MotionRow[]>([])
  const [motExBuf, setMotExBuf] = useState<MotionExRow[]>([])
  const [dmgBuf, setDmgBuf]     = useState<DamageRow[]>([])
  const [stsBuf, setStsBuf]     = useState<StatusRow[]>([])
  const [status, setStatus]     = useState<StatusRow | null>(null)
  const [damage, setDamage]     = useState<DamageRow | null>(null)
  const [lap, setLap]           = useState<LapRow | null>(null)
  const [timing, setTiming]     = useState<TimingMsg | null>(null)
  const [participants, setParticipants] = useState<ParticipantsMsg | null>(null)
  const [allStatus, setAllStatus]       = useState<AllStatusMsg | null>(null)
  const [fastestLapCarIdx, setFastestLapCarIdx] = useState<number | null>(null)
  const [raceEvent, setRaceEvent]   = useState<RaceEventMsg | null>(null)
  const [raceEvents, setRaceEvents] = useState<RaceEventMsg[]>([])
  const [session, setSession]       = useState<SessionMsg | null>(null)
  const [tyreSets, setTyreSets]   = useState<TyreSetsMsg | null>(null)
  const [lapHistoryBuf, setLapHistoryBuf] = useState<LapData[]>([])
  const [fastestLap, setFastestLap]       = useState<LapData | null>(null)
  const [protocolStatus, setProtocolStatus] = useState<ProtocolStatusMsg | null>(null)
  const [protocolWarning, setProtocolWarning] = useState<ProtocolWarningMsg | null>(null)
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
  const dmgBufRef      = useRef<DamageRow[]>([])
  const stsBufRef      = useRef<StatusRow[]>([])
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

          setTelBuf([]); telBufRef.current = []
          setMotBuf([]); motBufRef.current = []
          setMotExBuf([])
          setStsBuf([]); stsBufRef.current = []
          setDmgBuf([])
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
          setTelBuf(prev => {
            const last = prev.at(-1)
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
            const next = getNextBuffer(prev, msg, MAX_ROWS)
            telBufRef.current = next
            return next
          })
          break
        }
        case 'motion':
          setMotBuf(prev => {
            const next = getNextBuffer(prev, msg, MAX_ROWS)
            motBufRef.current = next
            return next
          })
          break
        case 'motion_ex':
          setMotExBuf(prev => {
            return getNextBuffer(prev, msg, MAX_ROWS)
          })
          break
        case 'status':
          setStatus(msg)
          setStsBuf(prev => {
            const next = getNextBuffer(prev, msg, MAX_ROWS)
            stsBufRef.current = next
            return next
          })
          break
        case 'damage':
          setDamage(msg)
          setDmgBuf(prev => {
            return getNextBuffer(prev, msg, MAX_ROWS)
          })
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
          flushSync(() => setRaceEvent(msg))
          setRaceEvents(prev => [...prev, msg])
          if (msg.code === 'SEND') {
            if (!isPlaybackRef.current) {
              setTelBuf([]); telBufRef.current = []
              setMotBuf([]); motBufRef.current = []
              setMotExBuf([])
              setStsBuf([]); stsBufRef.current = []
              setDmgBuf([])
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

          setTelBuf(tel); telBufRef.current = tel;
          setMotBuf(mot); motBufRef.current = mot;
          setMotExBuf(motEx);
          setStsBuf(sts); stsBufRef.current = sts;
          setDmgBuf(dmg); dmgBufRef.current = dmg;

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
    })

    const unsubOn = window.telemetryBridge.on((raw) => {
      handleMsg(raw as GatewayMsg)
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
    })

    return () => {
      unsubBatch()
      unsubOn()
      unsubBinary()
    }
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

  const latestSessionTime = telBuf.at(-1)?.session_time ?? 0
  const cutoff = latestSessionTime - seconds
  const lapStartSessionTime = lap && lap.current_lap_ms > 0 ? lapStartTimeRef.current : 0
  const isPlayback = speedRpmBlocks !== null
  const activeLapNum = lap ? lap.lap_num : 1
  const curBlock = isPlayback ? speedRpmBlocks.find(b => b.lapNum === activeLapNum) : null
  const prevBlock = isPlayback ? speedRpmBlocks.find(b => b.lapNum === activeLapNum - 1) : null
  const fastBlock = isPlayback ? speedRpmBlocks.find(b => b.lapNum === fastestLapNum) : null
  // Playback uses the seek-correct pre-scan; live uses the accumulated map.
  const lapTimesByNum = isPlayback ? playbackLapTimes : liveLapTimes

  const telemetry        = useMemo(() => telBuf.filter(d => d.session_time > cutoff),           [telBuf, cutoff])
  const motion           = useMemo(() => motBuf.filter(d => d.session_time > cutoff),           [motBuf, cutoff])
  const motionEx         = useMemo(() => motExBuf.filter(d => d.session_time > cutoff),         [motExBuf, cutoff])
  const statusHistory    = useMemo(() => stsBuf.filter(d => d.session_time > cutoff),           [stsBuf, cutoff])
  const damageHistory    = useMemo(() => dmgBuf.filter(d => d.session_time > cutoff),           [dmgBuf, cutoff])
  const latest           = useMemo(() => telBuf.length > 0 ? telBuf[telBuf.length - 1] : null, [telBuf])
  const lapHistory       = useMemo(() => isPlayback && prevBlock ? [prevBlock] : lapHistoryBuf, [isPlayback, prevBlock, lapHistoryBuf])
  const resolvedFastLap  = useMemo(() => isPlayback && fastBlock ? fastBlock : fastestLap,      [isPlayback, fastBlock, fastestLap])
  const lapTelemetry     = useMemo(
    () => isPlayback && curBlock
      ? curBlock.telemetry.filter((p: any) => p.session_time <= latestSessionTime)
      : telBuf.filter(d => d.session_time >= lapStartSessionTime),
    [isPlayback, curBlock, telBuf, lapStartSessionTime, latestSessionTime]
  )
  const lapStatusHistory = useMemo(
    () => isPlayback && curBlock
      ? curBlock.statusHistory.filter((p: any) => p.session_time <= latestSessionTime)
      : stsBuf.filter(d => d.session_time >= lapStartSessionTime),
    [isPlayback, curBlock, stsBuf, lapStartSessionTime, latestSessionTime]
  )

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
    fastestLapCarIdx, raceEvent, raceEvents: resolvedRaceEvents, session, tyreSets,
    latest, lapHistory, fastestLap: resolvedFastLap, lapTelemetry, lapStatusHistory,
    speedRpmBlocks, lapTimesByNum,
    isConnected: true, error: null, protocolStatus, protocolWarning,
  }
}
