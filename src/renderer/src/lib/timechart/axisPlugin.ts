import type { TimeChartPlugin } from 'timechart/plugins'

// Custom axis + x-grid plugin, drawn into TimeChart's SVG overlay (shadow DOM).
// We deliberately don't use the bundled d3Axis plugin: it offers no control over
// tick values, formatting, hidden ticks, gaps or grid, and we need to reproduce
// uPlot's exact look (fixed y tick values, `m:ss` / `${v}g` / `${v}mm` labels,
// faint x-grid, L-frame borders, 11px Cascadia Code). We draw against the
// chart's own d3 scales (chart.model.xScale/yScale map data -> element pixels)
// and reposition on every `model.updated`, reusing pooled SVG nodes so scrolling
// only mutates attributes rather than thrashing the DOM.

const SVGNS = 'http://www.w3.org/2000/svg'

export interface AxisConfig {
  axisColor: string
  gridColor: string
  borderColor: string
  font: string
  /** min px between adjacent x ticks (uPlot `space`). */
  xTickSpacePx: number
  xTickFormat: (seconds: number) => string
  /** derive y tick values from the current y domain. */
  yTickValues: (min: number, max: number) => number[]
  yTickFormat: (v: number) => string
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
    values: number[]
    format: (v: number) => string
  }>
  yAxisColor?: string
}

interface TextPool {
  g: SVGGElement
  nodes: SVGTextElement[]
}

function ensureText(pool: TextPool, i: number): SVGTextElement {
  let t = pool.nodes[i]
  if (!t) {
    t = document.createElementNS(SVGNS, 'text')
    pool.g.appendChild(t)
    pool.nodes[i] = t
  }
  return t
}

interface LinePool {
  g: SVGGElement
  nodes: SVGLineElement[]
}

function ensureLine(pool: LinePool, i: number): SVGLineElement {
  let l = pool.nodes[i]
  if (!l) {
    l = document.createElementNS(SVGNS, 'line')
    pool.g.appendChild(l)
    pool.nodes[i] = l
  }
  return l
}

export function createAxisPlugin(cfg: { current: AxisConfig }): TimeChartPlugin {
  return {
    apply(chart) {
      const svg = chart.svgLayer.svgNode

      // TimeChart's built-in SVG layer is appended after its WebGL canvas, so
      // anything placed in it is composited above the series. Put Cartesian
      // grid lines in their own SVG immediately before the canvas; labels,
      // borders and interaction overlays remain in the foreground SVG.
      const gridSvg = document.createElementNS(SVGNS, 'svg')
      Object.assign(gridSvg.style, {
        position: 'absolute', width: '100%', height: '100%',
        left: '0', right: '0', top: '0', bottom: '0', pointerEvents: 'none',
      })
      const shadowRoot = chart.el.shadowRoot
      if (!shadowRoot) throw new Error('TimeChart shadow root is unavailable')
      shadowRoot.insertBefore(gridSvg, chart.canvasLayer.canvas)

      const gridG = document.createElementNS(SVGNS, 'g')
      const yGridG = document.createElementNS(SVGNS, 'g')
      const tickG = document.createElementNS(SVGNS, 'g')
      const borderG = document.createElementNS(SVGNS, 'g')
      const xLabelG = document.createElementNS(SVGNS, 'g')
      const yLabelG = document.createElementNS(SVGNS, 'g')
      gridSvg.appendChild(gridG)
      gridSvg.appendChild(yGridG)
      // Foreground decorations stay above the WebGL series.
      for (const g of [tickG, borderG, xLabelG, yLabelG]) svg.appendChild(g)

      chart.model.disposing.on(() => gridSvg.remove())

      const gridPool: LinePool = { g: gridG, nodes: [] }
      const yGridPool: LinePool = { g: yGridG, nodes: [] }
      const tickPool: LinePool = { g: tickG, nodes: [] }
      const xLabels: TextPool = { g: xLabelG, nodes: [] }
      const yLabels: TextPool = { g: yLabelG, nodes: [] }
      const bottomBorder = document.createElementNS(SVGNS, 'line')
      const leftBorder = document.createElementNS(SVGNS, 'line')
      borderG.appendChild(bottomBorder)
      borderG.appendChild(leftBorder)

      const applyDash = (line: SVGLineElement, dash: number[] | undefined) => {
        if (dash && dash.length) line.setAttribute('stroke-dasharray', dash.join(' '))
        else line.removeAttribute('stroke-dasharray')
      }

      const draw = () => {
        const c = cfg.current
        const xScale = chart.model.xScale
        const yScale = chart.model.yScale
        const [plotLeft, plotRight] = xScale.range()
        const [plotBottom, plotTop] = yScale.range()

        // --- x axis: grid verticals + optional tick marks + labels ---
        const usable = plotRight - plotLeft
        const count = Math.max(2, Math.floor(usable / c.xTickSpacePx))
        const xTicks: number[] = xScale.ticks(count)

        for (let i = 0; i < xTicks.length; i++) {
          const px = xScale(xTicks[i])
          const line = ensureLine(gridPool, i)
          line.setAttribute('x1', String(px))
          line.setAttribute('x2', String(px))
          line.setAttribute('y1', String(plotTop))
          line.setAttribute('y2', String(plotBottom))
          line.setAttribute('stroke', c.gridColor)
          line.setAttribute('stroke-width', '1')
          applyDash(line, c.gridDash)
          line.style.display = ''

          if (c.xTickMark) {
            const mark = ensureLine(tickPool, i)
            mark.setAttribute('x1', String(px))
            mark.setAttribute('x2', String(px))
            mark.setAttribute('y1', String(plotBottom))
            mark.setAttribute('y2', String(plotBottom + c.xTickMark.size))
            mark.setAttribute('stroke', c.xTickMark.color)
            mark.setAttribute('stroke-width', '1')
            mark.style.display = ''
          }

          const t = ensureText(xLabels, i)
          t.setAttribute('x', String(px))
          t.setAttribute('y', String(plotBottom + c.xGap + (c.xTickMark ? c.xTickMark.size : 0)))
          t.setAttribute('fill', c.axisColor)
          t.style.font = c.font
          t.setAttribute('text-anchor', 'middle')
          t.setAttribute('dominant-baseline', 'hanging')
          t.textContent = c.xTickFormat(xTicks[i])
          t.style.display = ''
        }
        for (let i = xTicks.length; i < gridPool.nodes.length; i++) gridPool.nodes[i].style.display = 'none'
        for (let i = xTicks.length; i < tickPool.nodes.length; i++) tickPool.nodes[i].style.display = 'none'
        for (let i = xTicks.length; i < xLabels.nodes.length; i++) xLabels.nodes[i].style.display = 'none'

        // --- y axis: optional grid horizontals + labels ---
        const [yMin, yMax] = yScale.domain()
        const yTicks = c.yTickValues(yMin, yMax)
        for (let i = 0; i < yTicks.length; i++) {
          const py = yScale(yTicks[i])
          if (c.showYGrid) {
            const line = ensureLine(yGridPool, i)
            line.setAttribute('x1', String(plotLeft))
            line.setAttribute('x2', String(plotRight))
            line.setAttribute('y1', String(py))
            line.setAttribute('y2', String(py))
            line.setAttribute('stroke', c.gridColor)
            line.setAttribute('stroke-width', '1')
            applyDash(line, c.gridDash)
            line.style.display = ''
          }
          const t = ensureText(yLabels, i)
          t.setAttribute('x', String(plotLeft - c.yGap))
          t.setAttribute('y', String(py))
          t.setAttribute('fill', c.yAxisColor ?? c.axisColor)
          t.style.font = c.font
          t.setAttribute('text-anchor', 'end')
          t.setAttribute('dominant-baseline', 'central')
          t.textContent = c.yTickFormat(yTicks[i])
          t.style.display = ''
        }
        const yGridShown = c.showYGrid ? yTicks.length : 0
        for (let i = yGridShown; i < yGridPool.nodes.length; i++) yGridPool.nodes[i].style.display = 'none'
        let labelIndex = yTicks.length
        for (const axis of c.extraYAxes ?? []) {
          for (const value of axis.values) {
            const t = ensureText(yLabels, labelIndex++)
            t.setAttribute('x', String(axis.side === 'left' ? plotLeft - axis.offset : plotRight + axis.offset))
            t.setAttribute('y', String(yScale(value)))
            t.setAttribute('fill', axis.color)
            t.style.font = c.font
            t.setAttribute('text-anchor', axis.side === 'left' ? 'end' : 'start')
            t.setAttribute('dominant-baseline', 'central')
            t.textContent = axis.format(value)
            t.style.display = ''
          }
        }
        for (let i = labelIndex; i < yLabels.nodes.length; i++) yLabels.nodes[i].style.display = 'none'

        // --- L-frame borders ---
        bottomBorder.setAttribute('x1', String(plotLeft))
        bottomBorder.setAttribute('x2', String(plotRight))
        bottomBorder.setAttribute('y1', String(plotBottom))
        bottomBorder.setAttribute('y2', String(plotBottom))
        bottomBorder.setAttribute('stroke', c.borderColor)
        bottomBorder.setAttribute('stroke-width', '1')

        leftBorder.setAttribute('x1', String(plotLeft))
        leftBorder.setAttribute('x2', String(plotLeft))
        leftBorder.setAttribute('y1', String(plotTop))
        leftBorder.setAttribute('y2', String(plotBottom))
        leftBorder.setAttribute('stroke', c.borderColor)
        leftBorder.setAttribute('stroke-width', '1')
      }

      chart.model.updated.on(draw)
      chart.model.resized.on(draw)
      draw()
    },
  }
}
