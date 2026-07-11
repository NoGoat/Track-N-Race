import { useCallback, useEffect, useRef } from 'react'
import type uPlot from 'uplot'

export interface ScrollScaleOpts {
  // Backward target jumps larger than this snap immediately (seek / session
  // restart); smaller ones hold the window until the data catches back up
  // (pause→resume), so the chart never visibly scrolls backwards.
  snapS?: number
}

// Playback pause must stop the scroll instantly — not after a stall timeout.
// Main pushes playback_state on every play/pause/close, so one module-level
// subscription mirrors "paused" for every chart instance.
let playbackHalted = false
let playbackSubscribed = false
function ensurePlaybackSub(): void {
  if (playbackSubscribed) return
  playbackSubscribed = true
  window.playerBridge.onStateChange((st) => {
    playbackHalted = !!st.filename && !st.isPlaying && !st.isScanning
  })
}

// For live data there is no pause event, so stalls are detected from the
// stream's own measured rate (the engine sends at 20/40/60 Hz — the clock
// EMAs the arrival gaps to find it): if the next sample is more than
// STALL_PERIODS late, the stream is considered stopped and the window halts
// on the last real sample immediately.
const STALL_PERIODS = 1.5
const MIN_STALL_S = 0.05

// Smooth scrolling for time-windowed realtime charts. Data arrives at the
// telemetry rate (~60 Hz) with timer/IPC jitter, while the display may refresh
// at 120/180 Hz; sliding the x-window only when data lands therefore judders.
// Instead, a rAF loop slides the window against wall-clock time, extrapolating
// from the newest sample's session_time. New points still land at their exact
// positions when they arrive. Until a full window of data exists the window is
// anchored at the data start (the trace grows from the left) rather than
// showing pre-session time.
//
// Usage: pass the chart via attach/detach (UPlotReact onCreate/onDelete), give
// UPlotReact resetScales={false} so data updates don't fight the loop, and feed
// the newest/oldest visible sample times.
export function useScrollScale(
  enabled: boolean,
  latestT: number | null,
  firstT: number | null,
  windowSeconds: number,
  { snapS = 0.5 }: ScrollScaleOpts = {},
) {
  const chartRef = useRef<uPlot | null>(null)
  const clockRef = useRef<{
    lastT: number; wallAtT: number; est: number; firstT: number
    periodS: number; lastMin: number; lastData: uPlot.AlignedData | null
  }>({ lastT: NaN, wallAtT: 0, est: NaN, firstT: NaN, periodS: NaN, lastMin: NaN, lastData: null })

  useEffect(() => {
    if (latestT == null) return
    const c = clockRef.current
    c.firstT = firstT ?? NaN
    if (latestT !== c.lastT) {
      const now = performance.now()
      // Measure the stream's sample period from arrival gaps. Gaps ≥3x the
      // current estimate are stalls (pause, dropout), not cadence, and are
      // excluded so they don't inflate the stall threshold.
      if (!Number.isNaN(c.lastT) && latestT > c.lastT) {
        const gap = (now - c.wallAtT) / 1000
        if (Number.isNaN(c.periodS)) {
          if (gap > 0 && gap < 1) c.periodS = gap
        } else if (gap < c.periodS * 3) {
          c.periodS = c.periodS * 0.8 + gap * 0.2
        }
      }
      c.lastT = latestT
      c.wallAtT = now
    }
  }, [latestT, firstT])

  useEffect(() => {
    if (!enabled) return
    ensurePlaybackSub()
    let raf = 0
    const loop = () => {
      raf = requestAnimationFrame(loop)
      const u = chartRef.current
      const c = clockRef.current
      if (!u || Number.isNaN(c.lastT)) return
      if (Number.isNaN(c.est)) c.est = c.lastT
      if (playbackHalted) {
        // Explicit pause: stop on the last sample instantly.
        c.est = c.lastT
      } else {
        const age = (performance.now() - c.wallAtT) / 1000
        const stallS = Math.max(Number.isNaN(c.periodS) ? 0 : c.periodS * STALL_PERIODS, MIN_STALL_S)
        if (age < stallS) {
          const target = c.lastT + age
          if (target > c.est || !(c.est - target < snapS)) c.est = target
        } else {
          // The expected next sample didn't arrive — the stream is stopped
          // (live dropout / session end). Halt on the last real sample.
          c.est = c.lastT
        }
      }
      let min = c.est - windowSeconds
      if (!Number.isNaN(c.firstT) && min < c.firstT) min = c.firstT
      if (min !== c.lastMin) {
        c.lastMin = min
        c.lastData = u.data
        u.setScale('x', { min, max: min + windowSeconds })
      } else if (u.data !== c.lastData) {
        // setData(…, false) stores new data WITHOUT redrawing — redraws come
        // from setScale. While the window is anchored at the data start (data
        // shorter than the window) the scale doesn't move, so new points must
        // be painted explicitly. When nothing changed at all (halted stream)
        // neither branch runs and the chart costs nothing.
        c.lastData = u.data
        u.redraw()
      }
    }
    raf = requestAnimationFrame(loop)
    return () => cancelAnimationFrame(raf)
  }, [enabled, windowSeconds, snapS])

  const attach = useCallback((u: uPlot) => {
    chartRef.current = u
    // A freshly (re)created chart has no window applied yet — force the next
    // loop tick to setScale even if the computed min hasn't changed.
    clockRef.current.lastMin = NaN
  }, [])
  const detach = useCallback(() => { chartRef.current = null }, [])

  return { chartRef, attach, detach }
}
