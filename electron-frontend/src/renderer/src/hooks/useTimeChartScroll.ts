import { useCallback, useEffect, useRef } from 'react'
import type { MutableRefObject } from 'react'
import type { TChart } from '../lib/timechart/tc'
import { timeChartFrameScheduler } from '../lib/timechart/engine/core/frameScheduler'
import type { FrameScheduleHandle } from '../lib/timechart/engine/core/frameScheduler'
import { useTelemetryStore } from '../stores/telemetryStore'

export interface ScrollScaleOpts {
  snapS?: number
  /** Draw WebGL-only scroll frames between less frequent full model updates. */
  fastFrames?: boolean
  /** Frequency for axes/plugins during fast-frame scrolling. */
  fullFps?: number
  /** Follow the dense session clock while rendering a sparse row series. */
  followSessionClock?: boolean
  /** Minimum time to coast between sparse samples before declaring a stall. */
  minStallS?: number
  /** Grow the range from the first sample to the live playhead. */
  accumulateFromStart?: boolean
}

// Playback pause must stop every scrolling chart on the same shared frame.
let playbackHalted = false
let playbackRate = 1
let playbackSubscribed = false
const playbackWakeups = new Set<() => void>()
function ensurePlaybackSub(): void {
  if (playbackSubscribed) return
  playbackSubscribed = true
  window.playerBridge.onStateChange((st) => {
    playbackHalted = !!st.filename && !st.isPlaying && !st.isScanning
    playbackRate = st.filename ? Math.max(st.speed ?? 1, 0) : 1
    for (const wake of playbackWakeups) wake()
  })
}

const STALL_PERIODS = 2
const MIN_STALL_S = 0.05

type SessionClockFollower = (sessionTime: number, wallTime: number) => void
const sessionClockFollowers = new Set<SessionClockFollower>()
let sessionClockUnsubscribe: (() => void) | null = null
let sharedSessionTime = NaN

function publishSessionClock(sessionTime: number): void {
  if (!Number.isFinite(sessionTime) || sessionTime === sharedSessionTime) return
  sharedSessionTime = sessionTime
  const now = performance.now()
  for (const follower of sessionClockFollowers) follower(sessionTime, now)
}

function addSessionClockFollower(follower: SessionClockFollower): () => void {
  sessionClockFollowers.add(follower)
  if (!sessionClockUnsubscribe) {
    sessionClockUnsubscribe = useTelemetryStore.subscribe((state) => {
      const latest = state.latest?.session_time
      if (latest != null) publishSessionClock(latest)
    })
  }
  const latest = useTelemetryStore.getState().latest?.session_time
  if (latest != null) {
    if (latest === sharedSessionTime) follower(latest, performance.now())
    else publishSessionClock(latest)
  }
  else if (Number.isFinite(sharedSessionTime)) follower(sharedSessionTime, performance.now())
  return () => {
    sessionClockFollowers.delete(follower)
    if (sessionClockFollowers.size === 0) {
      sessionClockUnsubscribe?.()
      sessionClockUnsubscribe = null
      sharedSessionTime = NaN
    }
  }
}

interface ScrollClock {
  lastT: number
  wallAtT: number
  est: number
  firstT: number
  periodS: number
  lastMin: number
  lastMax: number
  gaps: number[]
  gapIdx: number
}

export function useTimeChartScroll(
  enabled: boolean,
  latestT: number | null,
  firstT: number | null,
  windowSeconds: number,
  dataDirtyRef: MutableRefObject<boolean>,
  {
    snapS = 0.5, fastFrames = false, fullFps = 60,
    followSessionClock = false, minStallS = MIN_STALL_S, accumulateFromStart = false,
  }: ScrollScaleOpts = {},
) {
  const chartRef = useRef<TChart | null>(null)
  const registrationRef = useRef<FrameScheduleHandle | null>(null)
  const enabledRef = useRef(enabled)
  enabledRef.current = enabled
  const configRef = useRef({ windowSeconds, snapS, fastFrames, fullFps, minStallS, accumulateFromStart })
  configRef.current = { windowSeconds, snapS, fastFrames, fullFps, minStallS, accumulateFromStart }
  const clockRef = useRef<ScrollClock>({
    lastT: NaN, wallAtT: 0, est: NaN, firstT: NaN,
    periodS: NaN, lastMin: NaN, lastMax: NaN, gaps: [], gapIdx: 0,
  })
  const lastFullAtRef = useRef(0)
  const frameCallbackRef = useRef<(frameTime: number) => boolean>(() => false)

  frameCallbackRef.current = (frameTime: number) => {
    const chart = chartRef.current
    const c = clockRef.current
    if (!chart || Number.isNaN(c.lastT)) return false
    const config = configRef.current

    if (Number.isNaN(c.est)) c.est = c.lastT
    const age = Math.max(0, (frameTime - c.wallAtT) / 1000)
    const stallS = Math.max(Number.isNaN(c.periodS) ? 0 : c.periodS * STALL_PERIODS, config.minStallS)
    if (playbackHalted) {
      c.est = c.lastT
    } else if (age < stallS) {
      const target = c.lastT + age * playbackRate
      if (target > c.est || !(c.est - target < config.snapS)) c.est = target
    }

    let min = config.accumulateFromStart && !Number.isNaN(c.firstT)
      ? c.firstT
      : c.est - config.windowSeconds
    if (!Number.isNaN(c.firstT) && min < c.firstT) min = c.firstT
    const max = config.accumulateFromStart ? Math.max(c.est, min + 0.001) : min + config.windowSeconds

    const applyDraw = (): void => chart.model.update()

    const applyFastDraw = (): void => {
      const plugins = chart.plugins as unknown as { lineChart?: { drawFrame: () => void } }
      if (!plugins.lineChart) { applyDraw(); return }
      chart.canvasLayer.clear()
      plugins.lineChart.drawFrame()
      for (const series of chart.options.series) series.data.markSynced()
    }

    if (min !== c.lastMin || max !== c.lastMax) {
      c.lastMin = min
      c.lastMax = max
      chart.options.xRange = { min, max }
      const needsFull = !config.fastFrames || dataDirtyRef.current || lastFullAtRef.current === 0 ||
        frameTime - lastFullAtRef.current >= 1000 / config.fullFps
      dataDirtyRef.current = false
      if (needsFull) {
        lastFullAtRef.current = frameTime
        applyDraw()
      } else {
        chart.model.xScale.domain([min, max])
        applyFastDraw()
      }
    } else if (dataDirtyRef.current) {
      dataDirtyRef.current = false
      chart.options.xRange = { min, max }
      lastFullAtRef.current = frameTime
      applyDraw()
    }

    return !playbackHalted && age < stallS
  }

  const registerIfReady = useCallback(() => {
    const chart = chartRef.current
    if (!enabledRef.current || !chart || registrationRef.current) return
    registrationRef.current = timeChartFrameScheduler.register(
      chart.el,
      (frameTime) => frameCallbackRef.current(frameTime),
    )
    registrationRef.current.wake()
  }, [])

  const acceptLatest = useCallback((value: number, now: number) => {
    const c = clockRef.current
    if (value < c.lastT) {
      c.est = value
      c.periodS = NaN
      c.gaps.length = 0
      c.gapIdx = 0
      c.lastMin = NaN
      c.lastMax = NaN
    }
    if (value === c.lastT) return
    if (!Number.isNaN(c.lastT) && value > c.lastT) {
      const gap = (now - c.wallAtT) / 1000
      c.gaps[c.gapIdx % 5] = gap
      c.gapIdx++
      const sorted = [...c.gaps].sort((a, b) => a - b)
      c.periodS = sorted[sorted.length >> 1]
    }
    c.lastT = value
    c.wallAtT = now
    registrationRef.current?.wake()
  }, [])

  useEffect(() => {
    if (latestT == null) return
    const c = clockRef.current
    c.firstT = firstT ?? NaN
    if (!followSessionClock || !Number.isFinite(sharedSessionTime)) {
      acceptLatest(latestT, performance.now())
    }
  }, [latestT, firstT, followSessionClock, acceptLatest])

  useEffect(() => {
    if (!followSessionClock) return
    return addSessionClockFollower(acceptLatest)
  }, [followSessionClock, acceptLatest])

  useEffect(() => {
    ensurePlaybackSub()
    if (enabled) registerIfReady()
    else {
      registrationRef.current?.unregister()
      registrationRef.current = null
    }
  }, [enabled, registerIfReady])

  useEffect(() => {
    clockRef.current.lastMin = NaN
    clockRef.current.lastMax = NaN
    lastFullAtRef.current = 0
    registrationRef.current?.wake()
  }, [windowSeconds, snapS, fastFrames, fullFps, minStallS, accumulateFromStart])

  useEffect(() => {
    const wake = () => registrationRef.current?.wake()
    playbackWakeups.add(wake)
    return () => {
      playbackWakeups.delete(wake)
      registrationRef.current?.unregister()
      registrationRef.current = null
    }
  }, [])

  const attach = useCallback((chart: TChart) => {
    chartRef.current = chart
    clockRef.current.lastMin = NaN
    clockRef.current.lastMax = NaN
    lastFullAtRef.current = 0
    registerIfReady()
  }, [registerIfReady])

  const detach = useCallback(() => {
    registrationRef.current?.unregister()
    registrationRef.current = null
    chartRef.current = null
  }, [])

  const wake = useCallback(() => registrationRef.current?.wake(), [])

  return { chartRef, attach, detach, wake }
}
