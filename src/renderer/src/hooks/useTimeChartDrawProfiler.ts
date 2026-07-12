import type { TimeChartPlugin } from 'timechart/plugins'
import { profileSync } from './useDrawProfiler'

// TimeChart port of createDrawProfilerPlugin. The uPlot version bracketed
// drawClear->draw (uPlot's real canvas draw span) and wrapped setData. TimeChart
// does all of its work — model recompute + WebGL draw + buffer sync — inside a
// single synchronous `model.update()`, so we wrap that one call: it is the exact
// analogue of the uPlot draw span. Logs per-chart avg/max/fps every 2s, and
// records a `tc-update:<label>` performance measure so the longtask observer in
// App can attribute a main-thread block to it. Remove with the rest of the perf
// scaffolding once the Misc-tab render cost is settled.
export function createTimeChartDrawProfilerPlugin(label: string): TimeChartPlugin {
  return {
    apply(chart) {
      const model = chart.model
      const orig = model.update.bind(model)
      let frames = 0
      let sum = 0
      let max = 0
      let lastLog = performance.now()

      ;(model as unknown as { update: () => void }).update = () => {
        const start = performance.now()
        profileSync(`tc-update:${label}`, orig)
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
      }
    },
  }
}
