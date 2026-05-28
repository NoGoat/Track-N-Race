import { useState, useEffect, useRef, useMemo } from 'react'
import { flushSync } from 'react-dom'
import type { TelemetryRow, MotionRow, MotionExRow, LapRow, StatusRow, DamageRow, TimingMsg, ParticipantsMsg, AllStatusMsg, RaceEventMsg, SessionMsg, TyreSetsMsg, GatewayMsg, LapData, SessionHistoryFastestMsg, ProtocolStatusMsg, ProtocolWarningMsg } from '../types'

const MAX_ROWS = 36000  // ~10 min at 60 fps

declare global {
  interface Window {
    telemetryBridge: {
      on: (callback: (row: unknown) => void) => (() => void)
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
  const fastestLapTimeRef = useRef<number>(Infinity)

  const telBufRef      = useRef<TelemetryRow[]>([])
  const motBufRef      = useRef<MotionRow[]>([])
  const stsBufRef      = useRef<StatusRow[]>([])
  const lapNumRef      = useRef<number | null>(null)
  const lapStartTimeRef = useRef<number>(0)
  const fastestLapSetRef      = useRef<boolean>(false)
  const sessionHistoryBestRef = useRef<Map<number, number>>(new Map())
  const isPlaybackRef = useRef<boolean>(false)
  const lastLoggedLapNumRef = useRef<number | null>(null)
  const lastSnapshotTimeRef = useRef<number>(0)

  useEffect(() => {
    const unsub = window.telemetryBridge.on((raw) => {
      const msg = raw as GatewayMsg
      switch (msg.type) {
        case 'playback_close': {
          isPlaybackRef.current = false
          lastLoggedLapNumRef.current = null
          lastSnapshotTimeRef.current = 0
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
            const base = last && msg.session_time < last.session_time
              ? prev.filter(d => d.session_time < msg.session_time)
              : prev.length >= MAX_ROWS ? prev.slice(1) : prev
            const next = [...base, msg]
            telBufRef.current = next
            return next
          })
          break
        }
        case 'motion':
          setMotBuf(prev => {
            const last = prev.at(-1)
            const base = last && msg.session_time < last.session_time
              ? prev.filter(d => d.session_time < msg.session_time)
              : prev.length >= MAX_ROWS ? prev.slice(1) : prev
            const next = [...base, msg]
            motBufRef.current = next
            return next
          })
          break
        case 'motion_ex':
          setMotExBuf(prev => {
            const last = prev[prev.length - 1]
            const base = last && msg.session_time < last.session_time
              ? prev.filter(d => d.session_time < msg.session_time)
              : prev.length >= MAX_ROWS ? prev.slice(1) : prev
            return [...base, msg]
          })
          break
        case 'status':
          setStatus(msg)
          setStsBuf(prev => {
            const last = prev.at(-1)
            const base = last && msg.session_time < last.session_time
              ? prev.filter(d => d.session_time < msg.session_time)
              : prev.length >= MAX_ROWS ? prev.slice(1) : prev
            const next = [...base, msg]
            stsBufRef.current = next
            return next
          })
          break
        case 'damage':
          setDamage(msg)
          setDmgBuf(prev => {
            const last = prev.at(-1)
            const base = last && msg.session_time < last.session_time
              ? prev.filter(d => d.session_time < msg.session_time)
              : prev.length >= MAX_ROWS ? prev.slice(1) : prev
            return [...base, msg]
          })
          break
        case 'lap': {
          const lapMsg = msg as any
          if (lapMsg.lap_num !== lastLoggedLapNumRef.current) {
            lastLoggedLapNumRef.current = lapMsg.lap_num
            window.debugBridge?.write(`[TELEMETRY] LAP_NUM_SEEN lap_num=${lapMsg.lap_num} current_lap_ms=${lapMsg.current_lap_ms} last_lap_ms=${lapMsg.last_lap_ms} session_time=${lapMsg.session_time?.toFixed(3)}`)
          }
          setLap(msg)
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
        case 'playback_fastest_lap':
          setFastestLap(msg.data)
          fastestLapTimeRef.current = msg.data.lapNum > 0 ? (msg.data.endSessionTime - msg.data.startSessionTime) * 1000 : Infinity
          break
        case 'playback_previous_lap':
          setLapHistoryBuf(prev => [...prev.filter(l => l.lapNum !== msg.data.lapNum), msg.data].sort((a, b) => a.lapNum - b.lapNum).slice(-3))
          break
        case 'playback_seek_flush': {
          const flush = msg as any
          setTelBuf(flush.telemetry)
          telBufRef.current = flush.telemetry
          setMotBuf(flush.motion)
          motBufRef.current = flush.motion
          setStsBuf(flush.status)
          stsBufRef.current = flush.status
          setDmgBuf(flush.damage)
          dmgBufRef.current = flush.damage
          lapStartTimeRef.current = flush.currentLapStart
          lapNumRef.current = flush.lapNum
          break
        }
        case 'playback_lap_blocks': {
          isPlaybackRef.current = true
          const data = msg as any
          setSpeedRpmBlocks(data.blocks)
          setFastestLapNum(data.fastestLapNum)
          setPlaybackEvents(data.events ?? [])
          window.debugBridge?.write(`[TELEMETRY] BLOCKS_RECEIVED count=${data.blocks?.length ?? 0} fastestLapNum=${data.fastestLapNum}`)
          for (const b of (data.blocks ?? [])) {
            const ft = b.telemetry?.[0]
            const lt = b.telemetry?.[b.telemetry.length - 1]
            window.debugBridge?.write(`[TELEMETRY] BLOCK lapNum=${b.lapNum} startST=${b.startSessionTime?.toFixed(3)} endST=${b.endSessionTime?.toFixed(3)} tel.count=${b.telemetry?.length ?? 0} sts.count=${b.statusHistory?.length ?? 0} tel.first=${ft ? ft.session_time.toFixed(3) : 'N/A'} tel.last=${lt ? lt.session_time.toFixed(3) : 'N/A'}`)
            // Dump session_times around the ~39.7s freeze point for lap 3 (index ~2350)
            if (b.lapNum === 3 && b.telemetry?.length > 2400) {
              const around = b.telemetry.slice(2330, 2400).map((p: any, i: number) => `[${2330+i}]=${p.session_time.toFixed(3)}`).join(' ')
              window.debugBridge?.write(`[TELEMETRY] LAP3_TEL_AROUND_2350 ${around}`)
              // Also check for non-monotonic entries in the whole block
              let firstBad = -1
              for (let i = 1; i < b.telemetry.length; i++) {
                if (b.telemetry[i].session_time <= b.telemetry[i-1].session_time) {
                  firstBad = i
                  break
                }
              }
              window.debugBridge?.write(`[TELEMETRY] LAP3_MONOTONIC_CHECK firstNonMonotonicIdx=${firstBad} ${firstBad >= 0 ? `st[${firstBad-1}]=${b.telemetry[firstBad-1].session_time.toFixed(3)} st[${firstBad}]=${b.telemetry[firstBad].session_time.toFixed(3)}` : 'all_ok'}`)
            }
          }
          break
        }
      }
    })

    return unsub
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
      window.debugBridge?.write(`[TELEMETRY] LAP_INIT lap_num=${lap.lap_num} latestST=${latestST.toFixed(3)} lapStartTimeRef=${lapStartTimeRef.current.toFixed(3)} current_lap_ms=${lap.current_lap_ms}`)
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
    window.debugBridge?.write(`[TELEMETRY] LAP_NUM_CHANGE from=${prevLapNum} to=${lap.lap_num} prevLapStart=${prevLapStart.toFixed(3)} endTime=${endTime.toFixed(3)} newLapData.tel.count=${newLapData.telemetry.length} last_lap_ms=${lap.last_lap_ms}`)
    setLapHistoryBuf(prev => [...prev, newLapData].slice(-3))
    const lapTimeMs = lap.last_lap_ms
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

  const lapTelLength = lapTelemetry.length
  useEffect(() => {
    if (!isPlayback) return
    window.debugBridge?.write(`[TELEMETRY] LAPTEL_LENGTH_CHANGE newLength=${lapTelLength} latestSessionTime=${latestSessionTime.toFixed(3)} activeLapNum=${activeLapNum} curBlock.lapNum=${curBlock?.lapNum ?? 'null'} curBlock.tel.count=${curBlock?.telemetry?.length ?? 'null'}`)
  }, [lapTelLength])

  useEffect(() => {
    if (!isPlayback) return
    const now = Date.now()
    if (now - lastSnapshotTimeRef.current < 1000) return
    lastSnapshotTimeRef.current = now
    window.debugBridge?.write(`[TELEMETRY] CURBLOCK_SNAPSHOT activeLapNum=${activeLapNum} curBlock.lapNum=${curBlock?.lapNum ?? 'null'} curBlock.tel.count=${curBlock?.telemetry?.length ?? 'null'} lapTelemetry.length=${lapTelemetry.length} latestSessionTime=${latestSessionTime.toFixed(3)} lapStatusHistory.length=${lapStatusHistory.length}`)
  }, [latestSessionTime])

  return {
    telemetry, motion, motionEx,
    status, statusHistory, damage, damageHistory,
    lap, timing, participants, allStatus,
    fastestLapCarIdx, raceEvent, raceEvents: resolvedRaceEvents, session, tyreSets,
    latest, lapHistory, fastestLap: resolvedFastLap, lapTelemetry, lapStatusHistory,
    speedRpmBlocks,
    isConnected: true, error: null, protocolStatus, protocolWarning,
  }
}
