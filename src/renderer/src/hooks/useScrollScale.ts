import { useCallback, useEffect, useRef } from 'react'
import type uPlot from 'uplot'

export interface ScrollScaleOpts {
  // How long the window keeps extrapolating after the newest sample stops
  // advancing. Must exceed the stream's sample period — the 0.25s default suits
  // the 60 Hz hot rows; sparse ~1-2 Hz streams (status/damage) should pass ~1.5
  // or the scroll stalls between samples.
  stallS?: number
  // Backward target jumps larger than this snap immediately (seek / session
  // restart); smaller ones hold the window until the data catches back up
  // (pause→resume), so the chart never visibly scrolls backwards.
  snapS?: number
}

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
  { stallS = 0.25, snapS = 0.5 }: ScrollScaleOpts = {},
) {
  const chartRef = useRef<uPlot | null>(null)
  const clockRef = useRef({ lastT: NaN, wallAtT: 0, est: NaN, firstT: NaN })

  useEffect(() => {
    if (latestT == null) return
    const c = clockRef.current
    c.firstT = firstT ?? NaN
    if (latestT !== c.lastT) {
      c.lastT = latestT
      c.wallAtT = performance.now()
    }
  }, [latestT, firstT])

  useEffect(() => {
    if (!enabled) return
    let raf = 0
    const loop = () => {
      raf = requestAnimationFrame(loop)
      const u = chartRef.current
      const c = clockRef.current
      if (!u || Number.isNaN(c.lastT)) return
      const age = (performance.now() - c.wallAtT) / 1000
      const target = c.lastT + Math.min(age, stallS)
      if (target > c.est || !(c.est - target < snapS)) c.est = target
      let min = c.est - windowSeconds
      if (!Number.isNaN(c.firstT) && min < c.firstT) min = c.firstT
      u.setScale('x', { min, max: min + windowSeconds })
    }
    raf = requestAnimationFrame(loop)
    return () => cancelAnimationFrame(raf)
  }, [enabled, windowSeconds, stallS, snapS])

  const attach = useCallback((u: uPlot) => { chartRef.current = u }, [])
  const detach = useCallback(() => { chartRef.current = null }, [])

  return { chartRef, attach, detach }
}
