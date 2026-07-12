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

      const gridG = document.createElementNS(SVGNS, 'g')
      const borderG = document.createElementNS(SVGNS, 'g')
      const xLabelG = document.createElementNS(SVGNS, 'g')
      const yLabelG = document.createElementNS(SVGNS, 'g')
      // grid first (behind), then borders, then labels
      for (const g of [gridG, borderG, xLabelG, yLabelG]) svg.appendChild(g)

      const gridPool: LinePool = { g: gridG, nodes: [] }
      const xLabels: TextPool = { g: xLabelG, nodes: [] }
      const yLabels: TextPool = { g: yLabelG, nodes: [] }
      const bottomBorder = document.createElementNS(SVGNS, 'line')
      const leftBorder = document.createElementNS(SVGNS, 'line')
      borderG.appendChild(bottomBorder)
      borderG.appendChild(leftBorder)

      const draw = () => {
        const c = cfg.current
        const xScale = chart.model.xScale
        const yScale = chart.model.yScale
        const [plotLeft, plotRight] = xScale.range()
        const [plotBottom, plotTop] = yScale.range()

        // --- x axis: grid verticals + labels ---
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
          line.style.display = ''

          const t = ensureText(xLabels, i)
          t.setAttribute('x', String(px))
          t.setAttribute('y', String(plotBottom + c.xGap))
          t.setAttribute('fill', c.axisColor)
          t.setAttribute('font', c.font)
          t.style.font = c.font
          t.setAttribute('text-anchor', 'middle')
          t.setAttribute('dominant-baseline', 'hanging')
          t.textContent = c.xTickFormat(xTicks[i])
          t.style.display = ''
        }
        for (let i = xTicks.length; i < gridPool.nodes.length; i++) gridPool.nodes[i].style.display = 'none'
        for (let i = xTicks.length; i < xLabels.nodes.length; i++) xLabels.nodes[i].style.display = 'none'

        // --- y axis: labels only (grid hidden, matching uPlot) ---
        const [yMin, yMax] = yScale.domain()
        const yTicks = c.yTickValues(yMin, yMax)
        for (let i = 0; i < yTicks.length; i++) {
          const py = yScale(yTicks[i])
          const t = ensureText(yLabels, i)
          t.setAttribute('x', String(plotLeft - c.yGap))
          t.setAttribute('y', String(py))
          t.setAttribute('fill', c.axisColor)
          t.style.font = c.font
          t.setAttribute('text-anchor', 'end')
          t.setAttribute('dominant-baseline', 'central')
          t.textContent = c.yTickFormat(yTicks[i])
          t.style.display = ''
        }
        for (let i = yTicks.length; i < yLabels.nodes.length; i++) yLabels.nodes[i].style.display = 'none'

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
