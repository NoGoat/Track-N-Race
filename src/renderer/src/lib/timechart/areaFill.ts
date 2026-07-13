import type { TimeChartPlugin } from './tc'
import type { AlignedDataBuffer } from './engine/core/alignedData'

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

        // Adjacent fill series normally share one aligned timeline. Resolve
        // its visible ring interval once instead of repeating the same X
        // binary searches for every Y channel.
        let rangeBuffer: AlignedDataBuffer | null = null
        let rangeStart = 0
        let rangeEnd = -1
        for (const def of defs) {
          const data = chart.options.series[def.seriesIndex]?.data
          if (!data || data.length === 0) continue

          if (data.buffer !== rangeBuffer) {
            rangeBuffer = data.buffer
            rangeStart = Math.max(0, data.lowerBoundX(Number(domain[0])) - 1)
            rangeEnd = Math.min(data.length - 1, data.lowerBoundX(Number(domain[1]), rangeStart))
          }
          const start = rangeStart
          const end = rangeEnd
          if (end < start) continue

          const baselineY = Math.max(plotTop, Math.min(plotBottom, yScale(def.baseline)))
          let previousX = data.xAt(start)
          let previousY = data.yAt(start)
          const firstX = xScale(previousX)
          ctx.beginPath()
          ctx.moveTo(firstX, baselineY)
          ctx.lineTo(firstX, yScale(previousY))

          let previousPixel = Math.floor(firstX)
          for (let i = start + 1; i <= end; i++) {
            const x = data.xAt(i)
            const y = data.yAt(i)
            const currentX = xScale(x)
            const currentPixel = Math.floor(currentX)
            // Multiple telemetry samples landing on one screen pixel cannot
            // add visible detail. Keep the last value and emit once per pixel.
            if (currentPixel === previousPixel && i !== end) {
              previousX = x
              previousY = y
              continue
            }
            if (def.stepped) {
              const step = def.stepLocation ?? 1
              const transitionX = xScale(previousX * (1 - step) + x * step)
              ctx.lineTo(transitionX, yScale(previousY))
              ctx.lineTo(transitionX, yScale(y))
            } else {
              ctx.lineTo(currentX, yScale(y))
            }
            previousX = x
            previousY = y
            previousPixel = currentPixel
          }
          ctx.lineTo(xScale(previousX), baselineY)
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
