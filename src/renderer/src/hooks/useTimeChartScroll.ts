import { useCallback, useEffect, useRef } from 'react'
import type { MutableRefObject } from 'react'
import type { TChart } from '../lib/timechart/tc'

// TimeChart port of useScrollScale. The clock / stall-detection / playback-pause
// logic is identical to the uPlot version (hooks/useScrollScale.ts); only the
// three renderer calls differ:
//   uPlot                              TimeChart
//   u.setScale('x', {min,max})    ->   chart.options.xRange = {min,max}; model.update()
//   u.redraw()                    ->   model.update()
//   u.data changed?               ->   dataDirtyRef (bridge sets it when it mutates)
// model.update() is TimeChart's synchronous redraw (updateModel + dispatch +
// buffer synchronization), so like uPlot's setScale it performs one draw per call.
// Keeping our own rAF loop (rather than TimeChart's realTime auto-scroll) lets us
// slide against wall-clock time and share the exact stall/pause behaviour.

export interface ScrollScaleOpts {
  // Backward target jumps larger than this snap immediately (seek / session
  // restart); smaller ones hold the window until the data catches back up
  // (pause->resume), so the chart never visibly scrolls backwards.
  snapS?: number
  /** Draw WebGL-only scroll frames between less frequent full model updates. */
  fastFrames?: boolean
  /** Frequency for axes/plugins during fast-frame scrolling. */
  fullFps?: number
}

// Playback pause must stop the scroll instantly. Main pushes playback_state on
// every play/pause/close; one module-level subscription mirrors "paused" for
// every chart instance.
let playbackHalted = false
let playbackSubscribed = false
function ensurePlaybackSub(): void {
  if (playbackSubscribed) return
  playbackSubscribed = true
  window.playerBridge.onStateChange((st) => {
    playbackHalted = !!st.filename && !st.isPlaying && !st.isScanning
  })
}

// See useScrollScale for the rationale on median-gap stall detection.
const STALL_PERIODS = 2
const MIN_STALL_S = 0.05

export function useTimeChartScroll(
  enabled: boolean,
  latestT: number | null,
  firstT: number | null,
  windowSeconds: number,
  dataDirtyRef: MutableRefObject<boolean>,
  { snapS = 0.5, fastFrames = false, fullFps = 60 }: ScrollScaleOpts = {},
  profilerLabel?: string,
) {
  const chartRef = useRef<TChart | null>(null)
  const clockRef = useRef<{
    lastT: number; wallAtT: number; est: number; firstT: number
    periodS: number; lastMin: number
    gaps: number[]; gapIdx: number
  }>({ lastT: NaN, wallAtT: 0, est: NaN, firstT: NaN, periodS: NaN, lastMin: NaN, gaps: [], gapIdx: 0 })
  const perfRef = useRef({ startedAt: performance.now(), calls: 0, total: 0, max: 0 })

  useEffect(() => {
    if (latestT == null) return
    const c = clockRef.current
    c.firstT = firstT ?? NaN
    if (latestT !== c.lastT) {
      const now = performance.now()
      if (!Number.isNaN(c.lastT) && latestT > c.lastT) {
        const gap = (now - c.wallAtT) / 1000
        c.gaps[c.gapIdx % 5] = gap
        c.gapIdx++
        const sorted = [...c.gaps].sort((a, b) => a - b)
        c.periodS = sorted[sorted.length >> 1]
      }
      c.lastT = latestT
      c.wallAtT = now
    }
  }, [latestT, firstT])

  useEffect(() => {
    if (!enabled) return
    ensurePlaybackSub()
    let raf = 0
    let lastFullAt = 0

    const applyDraw = (): void => {
      const chart = chartRef.current
      if (!chart) return
      const started = performance.now()
      chart.model.update()
      if (profilerLabel) {
        const duration = performance.now() - started
        const perf = perfRef.current
        perf.calls++
        perf.total += duration
        if (duration > perf.max) perf.max = duration
      }
    }

    const applyFastDraw = (): void => {
      const chart = chartRef.current
      if (!chart) return
      // TimeChart's line renderer and canvas layer are public. Updating the
      // scale directly avoids model.updated, so SVG axes/grid and other plugins
      // do not perform DOM work on this intermediate high-refresh frame.
      const plugins = chart.plugins as unknown as { lineChart?: { drawFrame: () => void } }
      if (!plugins.lineChart) { applyDraw(); return }
      chart.canvasLayer.clear()
      plugins.lineChart.drawFrame()
      // drawFrame synchronizes new GPU data. Mark the buffers clean just as
      // RenderModel.update() normally does, preventing repeat uploads.
      for (const series of chart.options.series) series.data.markSynced()
    }

    const loop = (frameTime: number) => {
      raf = requestAnimationFrame(loop)
      const chart = chartRef.current
      const c = clockRef.current
      if (!chart || Number.isNaN(c.lastT)) return
      if (Number.isNaN(c.est)) c.est = c.lastT
      if (playbackHalted) {
        c.est = c.lastT
      } else {
        const age = (performance.now() - c.wallAtT) / 1000
        const stallS = Math.max(Number.isNaN(c.periodS) ? 0 : c.periodS * STALL_PERIODS, MIN_STALL_S)
        if (age < stallS) {
          const target = c.lastT + age
          if (target > c.est || !(c.est - target < snapS)) c.est = target
        } else {
          c.est = c.lastT
        }
      }
      let min = c.est - windowSeconds
      if (!Number.isNaN(c.firstT) && min < c.firstT) min = c.firstT

      if (min !== c.lastMin) {
        // Window moved: reposition and draw.
        c.lastMin = min
        chart.options.xRange = { min, max: min + windowSeconds }
        const needsFull = !fastFrames || dataDirtyRef.current || lastFullAt === 0 || frameTime - lastFullAt >= 1000 / fullFps
        dataDirtyRef.current = false
        if (needsFull) {
          lastFullAt = frameTime
          applyDraw()
        } else {
          chart.model.xScale.domain([min, min + windowSeconds])
          applyFastDraw()
        }
      } else if (dataDirtyRef.current) {
        // Window anchored (data shorter than the window) but new points landed;
        // paint them. When nothing changed, neither branch runs and the frame
        // costs nothing.
        dataDirtyRef.current = false
        chart.options.xRange = { min, max: min + windowSeconds }
        lastFullAt = frameTime
        applyDraw()
      }

      if (profilerLabel) {
        const perf = perfRef.current
        const now = performance.now()
        const elapsed = now - perf.startedAt
        if (elapsed >= 2000 && perf.calls > 0) {
          // eslint-disable-next-line no-console
          console.log(`[perf:scroll] ${profilerLabel}: avg=${(perf.total / perf.calls).toFixed(2)}ms max=${perf.max.toFixed(2)}ms calls=${perf.calls} (${(perf.calls / elapsed * 1000).toFixed(1)}/s over ${(elapsed / 1000).toFixed(1)}s)`)
          perf.startedAt = now
          perf.calls = 0
          perf.total = 0
          perf.max = 0
        }
      }
    }
    raf = requestAnimationFrame(loop)
    return () => cancelAnimationFrame(raf)
  }, [enabled, windowSeconds, snapS, fastFrames, fullFps, profilerLabel, dataDirtyRef])

  const attach = useCallback((chart: TChart) => {
    chartRef.current = chart
    // A freshly created chart has no window applied yet — force the next loop
    // tick to reposition even if the computed min hasn't changed.
    clockRef.current.lastMin = NaN
  }, [])
  const detach = useCallback(() => { chartRef.current = null }, [])

  return { chartRef, attach, detach }
}
