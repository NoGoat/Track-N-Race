import type { TimeChartPlugin } from './tc'

// Y-axis labels use a centered text baseline. Keep the plot boundary at least
// half of the default 11 px axis font below the canvas edge so the top label's
// glyphs are not clipped.
export const AXIS_LABEL_TOP_PADDING = 6

// Axes and grids are redrawn every display frame while a chart scrolls. Using
// two stable canvas layers keeps that work out of the DOM: All Laps can have
// dozens of lap-boundary ticks per chart, which previously meant hundreds of
// SVG attribute/text mutations per frame across a page.

export interface AxisConfig {
  axisColor: string
  gridColor: string
  borderColor: string
  font: string
  /** min px between adjacent x ticks (uPlot `space`). */
  xTickSpacePx: number
  xTickFormat: (seconds: number) => string
  /** Optional exact x ticks (used by AL for lap-boundary splits). */
  xTickValues?: (min: number, max: number) => number[]
  /** Allow dense exact ticks to skip labels. Disable for the three sector labels. */
  cullXTickLabels?: boolean
  xTickAnchor?: 'start' | 'middle' | 'end'
  xLabelOffset?: number
  /** derive y tick values from the current y domain. */
  yTickValues: (min: number, max: number) => number[]
  yTickFormat: (v: number) => string
  yTickColor?: (v: number) => string
  xGap: number
  yGap: number
  /** dash pattern for grid lines (e.g. [3, 3]); solid when omitted. */
  gridDash?: number[]
  /** draw horizontal grid lines at the y ticks (uPlot y-grid). */
  showYGrid?: boolean
  /** small tick marks below the x axis (uPlot x ticks); omitted = none. */
  xTickMark?: { color: string; size: number } | null
  /** Additional normalized y axes, positioned relative to the plot edge. */
  extraYAxes?: Array<{
    side: 'left' | 'right'
    offset: number
    color: string
    colorForValue?: (v: number) => string
    values: number[]
    format: (v: number) => string
  }>
  yAxisColor?: string
  /** Independent vertical panels rendered by series sharing one WebGL canvas. */
  panels?: Array<{
    top: number
    bottom: number
    showYAxis: boolean
    yAxisColor: string
    yTickValues: number[]
    yTickFormat: (v: number) => string
    yTickColor?: (v: number) => string
    gapAfter?: number
  }>
}

function styleLayer(canvas: HTMLCanvasElement): void {
  Object.assign(canvas.style, {
    position: 'absolute',
    width: '100%',
    height: '100%',
    left: '0',
    right: '0',
    top: '0',
    bottom: '0',
    pointerEvents: 'none',
  })
}

function resizeCanvas(canvas: HTMLCanvasElement, ratio: number, width: number, height: number): CanvasRenderingContext2D {
  const pixelWidth = Math.max(1, Math.round(width * ratio))
  const pixelHeight = Math.max(1, Math.round(height * ratio))
  if (canvas.width !== pixelWidth) canvas.width = pixelWidth
  if (canvas.height !== pixelHeight) canvas.height = pixelHeight
  const context = canvas.getContext('2d')
  if (!context) throw new Error('Unable to initialize TimeChart axis canvas')
  context.setTransform(ratio, 0, 0, ratio, 0, 0)
  return context
}

function strokePath(
  context: CanvasRenderingContext2D,
  color: string,
  dash: number[] | undefined,
  build: () => void,
): void {
  context.beginPath()
  build()
  context.strokeStyle = color
  context.lineWidth = 1
  context.setLineDash(dash ?? [])
  context.stroke()
}

export function createAxisPlugin(cfg: { current: AxisConfig }): TimeChartPlugin {
  return {
    apply(chart) {
      const shadowRoot = chart.el.shadowRoot
      if (!shadowRoot) throw new Error('TimeChart shadow root is unavailable')

      const gridCanvas = document.createElement('canvas')
      const labelCanvas = document.createElement('canvas')
      styleLayer(gridCanvas)
      styleLayer(labelCanvas)

      // Grid remains behind the WebGL traces. Labels, tick marks and borders
      // remain above them but below the SVG interaction/crosshair layer.
      shadowRoot.insertBefore(gridCanvas, chart.canvasLayer.canvas)
      shadowRoot.insertBefore(labelCanvas, chart.svgLayer.svgNode)

      const draw = () => {
        // Use the chart's committed layout size. While ResizeObserver is
        // settling an interactive window drag, CSS stretches these layers;
        // reading the live element size here would still reallocate both axis
        // backing stores on every playback redraw and defeat resize coalescing.
        const width = chart.clientWidth
        const height = chart.clientHeight
        if (width <= 0 || height <= 0) return
        const ratio = chart.options.pixelRatio
        const grid = resizeCanvas(gridCanvas, ratio, width, height)
        const labels = resizeCanvas(labelCanvas, ratio, width, height)
        grid.clearRect(0, 0, width, height)
        labels.clearRect(0, 0, width, height)

        const c = cfg.current
        const xScale = chart.model.xScale
        const yScale = chart.model.yScale
        const [plotLeft, plotRight] = xScale.range().map(Number)
        const [plotBottom, plotTop] = yScale.range().map(Number)
        const [xMin, xMax] = xScale.domain().map(Number)
        const [yMin, yMax] = yScale.domain().map(Number)
        const usable = plotRight - plotLeft
        const tickCapacity = Math.max(2, Math.floor(usable / c.xTickSpacePx))
        const xTicks = c.xTickValues ? c.xTickValues(xMin, xMax) : xScale.ticks(tickCapacity)
        const labelEvery = c.xTickValues && c.cullXTickLabels !== false
          ? Math.max(1, Math.ceil(xTicks.length / tickCapacity))
          : 1
        const yTicks = c.yTickValues(yMin, yMax)

        if (c.panels) {
          strokePath(grid, c.gridColor, c.gridDash, () => {
            for (const panel of c.panels!) {
              const panelTop = plotTop + panel.top * (plotBottom - plotTop)
              const panelBottom = plotTop + panel.bottom * (plotBottom - plotTop) - (panel.gapAfter ?? 0)
              for (const tick of xTicks) {
                const x = xScale(tick)
                grid.moveTo(x, panelTop)
                grid.lineTo(x, panelBottom)
              }
              for (const tick of panel.yTickValues) {
                const y = panelBottom - tick * (panelBottom - panelTop)
                grid.moveTo(plotLeft, y)
                grid.lineTo(plotRight, y)
              }
            }
          })

          labels.font = c.font
          labels.textBaseline = 'top'
          labels.fillStyle = c.axisColor
          labels.textAlign = c.xTickAnchor === 'middle' || c.xTickAnchor == null ? 'center' : c.xTickAnchor
          const tickSize = c.xTickMark?.size ?? 0
          const xLabelY = plotBottom + c.xGap + tickSize
          for (let index = 0; index < xTicks.length; index++) {
            if (index !== 0 && index !== xTicks.length - 1 && index % labelEvery !== 0) continue
            const tick = xTicks[index]
            labels.fillText(c.xTickFormat(tick), xScale(tick) + (c.xLabelOffset ?? 0), xLabelY)
          }

          labels.textBaseline = 'middle'
          labels.textAlign = 'right'
          for (const panel of c.panels) {
            if (!panel.showYAxis) continue
            const panelTop = plotTop + panel.top * (plotBottom - plotTop)
            const panelBottom = plotTop + panel.bottom * (plotBottom - plotTop) - (panel.gapAfter ?? 0)
            for (const tick of panel.yTickValues) {
              labels.fillStyle = panel.yTickColor?.(tick) ?? panel.yAxisColor
              labels.fillText(panel.yTickFormat(tick), plotLeft - c.yGap, panelBottom - tick * (panelBottom - panelTop))
            }
          }

          strokePath(labels, c.borderColor, undefined, () => {
            for (const panel of c.panels!) {
              const panelTop = plotTop + panel.top * (plotBottom - plotTop)
              const panelBottom = plotTop + panel.bottom * (plotBottom - plotTop) - (panel.gapAfter ?? 0)
              labels.moveTo(plotLeft, panelTop)
              labels.lineTo(plotLeft, panelBottom)
              labels.moveTo(plotLeft, panelBottom)
              labels.lineTo(plotRight, panelBottom)
            }
          })
          return
        }

        strokePath(grid, c.gridColor, c.gridDash, () => {
          for (const tick of xTicks) {
            const x = xScale(tick)
            grid.moveTo(x, plotTop)
            grid.lineTo(x, plotBottom)
          }
        })
        if (c.showYGrid) {
          strokePath(grid, c.gridColor, c.gridDash, () => {
            for (const tick of yTicks) {
              const y = yScale(tick)
              grid.moveTo(plotLeft, y)
              grid.lineTo(plotRight, y)
            }
          })
        }

        labels.font = c.font
        labels.textBaseline = 'top'
        labels.fillStyle = c.axisColor
        labels.textAlign = c.xTickAnchor === 'middle' || c.xTickAnchor == null ? 'center' : c.xTickAnchor
        const tickSize = c.xTickMark?.size ?? 0
        const xLabelY = plotBottom + c.xGap + tickSize
        for (let index = 0; index < xTicks.length; index++) {
          if (index !== 0 && index !== xTicks.length - 1 && index % labelEvery !== 0) continue
          const tick = xTicks[index]
          labels.fillText(c.xTickFormat(tick), xScale(tick) + (c.xLabelOffset ?? 0), xLabelY)
        }

        if (c.xTickMark) {
          strokePath(labels, c.xTickMark.color, undefined, () => {
            for (const tick of xTicks) {
              const x = xScale(tick)
              labels.moveTo(x, plotBottom)
              labels.lineTo(x, plotBottom + c.xTickMark!.size)
            }
          })
        }

        labels.textBaseline = 'middle'
        labels.textAlign = 'right'
        for (const tick of yTicks) {
          labels.fillStyle = c.yTickColor?.(tick) ?? c.yAxisColor ?? c.axisColor
          labels.fillText(c.yTickFormat(tick), plotLeft - c.yGap, yScale(tick))
        }
        for (const axis of c.extraYAxes ?? []) {
          labels.textAlign = axis.side === 'left' ? 'right' : 'left'
          const x = axis.side === 'left' ? plotLeft - axis.offset : plotRight + axis.offset
          for (const value of axis.values) {
            labels.fillStyle = axis.colorForValue?.(value) ?? axis.color
            labels.fillText(axis.format(value), x, yScale(value))
          }
        }

        strokePath(labels, c.borderColor, undefined, () => {
          labels.moveTo(plotLeft, plotBottom)
          labels.lineTo(plotRight, plotBottom)
          labels.moveTo(plotLeft, plotTop)
          labels.lineTo(plotLeft, plotBottom)
        })
      }

      chart.model.updated.on(draw)
      chart.model.disposing.on(() => {
        gridCanvas.remove()
        labelCanvas.remove()
      })
      draw()
    },
  }
}
