import type { TimeChartPlugin } from 'timechart/plugins'

export interface AreaFillDef {
  seriesIndex: number
  color: string
  baseline: number
  stepped?: boolean
  stepLocation?: number
}

/**
 * Draw translucent areas on a background canvas. Samples are binned to CSS
 * pixels, so drawing cost is bounded by chart width rather than history size.
 */
export function createAreaFillPlugin(defs: AreaFillDef[]): TimeChartPlugin {
  return {
    apply(chart) {
      const canvas = document.createElement('canvas')
      Object.assign(canvas.style, {
        position: 'absolute', width: '100%', height: '100%',
        left: '0', right: '0', top: '0', bottom: '0', pointerEvents: 'none',
      })
      const shadowRoot = chart.el.shadowRoot
      if (!shadowRoot) throw new Error('TimeChart shadow root is unavailable')
      // Axis grid layers are inserted first. This canvas therefore sits above
      // the grid and below TimeChart's WebGL series canvas.
      shadowRoot.insertBefore(canvas, chart.canvasLayer.canvas)
      const ctx = canvas.getContext('2d')
      if (!ctx) throw new Error('Unable to initialize area-fill canvas')

      const resize = () => {
        const ratio = chart.options.pixelRatio
        const width = chart.el.clientWidth
        const height = chart.el.clientHeight
        if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
          canvas.width = Math.round(width * ratio)
          canvas.height = Math.round(height * ratio)
        }
        ctx.setTransform(ratio, 0, 0, ratio, 0, 0)
      }

      const draw = () => {
        resize()
        const width = chart.el.clientWidth
        const height = chart.el.clientHeight
        ctx.clearRect(0, 0, width, height)

        const xScale = chart.model.xScale
        const yScale = chart.model.yScale
        const [plotLeft, plotRight] = xScale.range()
        const [plotBottom, plotTop] = yScale.range()
        const domain = xScale.domain()

        ctx.save()
        ctx.beginPath()
        ctx.rect(plotLeft, plotTop, plotRight - plotLeft, plotBottom - plotTop)
        ctx.clip()

        for (const def of defs) {
          const data = chart.options.series[def.seriesIndex]?.data
          if (!data || data.length === 0) continue

          let lo = 0, hi = data.length
          while (lo < hi) { const mid = (lo + hi) >> 1; if (data[mid].x < domain[0]) lo = mid + 1; else hi = mid }
          const start = Math.max(0, lo - 1)
          lo = start; hi = data.length
          while (lo < hi) { const mid = (lo + hi) >> 1; if (data[mid].x <= domain[1]) lo = mid + 1; else hi = mid }
          const end = Math.min(data.length - 1, lo)
          if (end < start) continue

          const baselineY = Math.max(plotTop, Math.min(plotBottom, yScale(def.baseline)))
          const first = data[start]
          const firstX = xScale(first.x)
          ctx.beginPath()
          ctx.moveTo(firstX, baselineY)
          ctx.lineTo(firstX, yScale(first.y))

          let previous = first
          let previousPixel = Math.floor(firstX)
          for (let i = start + 1; i <= end; i++) {
            const current = data[i]
            const currentX = xScale(current.x)
            const currentPixel = Math.floor(currentX)
            // Multiple telemetry samples landing on one screen pixel cannot
            // add visible detail. Keep the last value and emit once per pixel.
            if (currentPixel === previousPixel && i !== end) {
              previous = current
              continue
            }
            if (def.stepped) {
              const step = def.stepLocation ?? 1
              const transitionX = xScale(previous.x * (1 - step) + current.x * step)
              ctx.lineTo(transitionX, yScale(previous.y))
              ctx.lineTo(transitionX, yScale(current.y))
            } else {
              ctx.lineTo(currentX, yScale(current.y))
            }
            previous = current
            previousPixel = currentPixel
          }
          ctx.lineTo(xScale(previous.x), baselineY)
          ctx.closePath()
          ctx.fillStyle = def.color
          ctx.fill()
        }
        ctx.restore()
      }

      chart.model.updated.on(draw)
      chart.model.resized.on(draw)
      chart.model.disposing.on(() => canvas.remove())
      draw()
    },
  }
}
