import type uPlot from 'uplot'

// Temporary diagnostic: measures actual wall-clock time uPlot spends drawing
// this specific chart instance (drawClear→draw span covers axes+series+plugin
// draw hooks), and logs per-chart stats to the console every 2s. Attach the
// same plugin to every chart under comparison so the numbers are apples-to-
// apples. Remove once the Misc-tab stutter is root-caused.
// Diagnostic: bracket a synchronous region so the longtask observer can
// attribute a main-thread block to it. Records a `sync:<label>` performance
// measure (start + duration) only when the region ran long enough to matter,
// so the measure buffer stays small. The longtask PerformanceObserver in App
// reports the `sync:` measures that overlap each longtask window. Pure timing —
// no behaviour change. Remove with the rest of the perf scaffolding once the
// Misc-tab stutter is root-caused.
export function profileSync<T>(label: string, fn: () => T): T {
  if (typeof performance === 'undefined' || typeof performance.measure !== 'function') return fn()
  const start = performance.now()
  try {
    return fn()
  } finally {
    const duration = performance.now() - start
    if (duration >= 1) {
      try { performance.measure(`sync:${label}`, { start, duration }) } catch { /* buffer full / unsupported */ }
    }
  }
}

export function createDrawProfilerPlugin(label: string): uPlot.Plugin {
  let start = 0
  let frames = 0
  let sum = 0
  let max = 0
  let lastLog = performance.now()

  return {
    hooks: {
      // Wrap this instance's setData so we can see how long uplot-react's
      // per-render setData (called from its layout effect, i.e. React commit
      // phase) actually takes — the prime suspect for the commit-phase cost the
      // render-vs-commit split points at. Shows up as a `setData:<label>` region
      // overlapping the longtask.
      init: (u: uPlot) => {
        const orig = u.setData.bind(u)
        ;(u as unknown as { setData: uPlot['setData'] }).setData = (data, resetScales) =>
          profileSync(`setData:${label}`, () => orig(data, resetScales))
      },
      drawClear: () => { start = performance.now() },
      draw: () => {
        const dur = performance.now() - start
        frames++
        sum += dur
        if (dur > max) max = dur
        const now = performance.now()
        if (now - lastLog >= 2000) {
          const avg = sum / frames
          const fps = (frames / (now - lastLog)) * 1000
          // eslint-disable-next-line no-console
          console.log(`[perf] ${label}: avg=${avg.toFixed(2)}ms max=${max.toFixed(2)}ms redraws=${frames} (${fps.toFixed(0)}/s over ${((now - lastLog) / 1000).toFixed(1)}s)`)
          frames = 0
          sum = 0
          max = 0
          lastLog = now
        }
      },
    },
  }
}
