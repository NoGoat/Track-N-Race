import type { TimeChartPlugin } from './tc'

// Horizontal reference lines drawn into the SVG overlay (e.g. G-Force's solid
// zero line + dashed +/-4g lines). uPlot drew these on its 2D canvas in a `draw`
// hook; TimeChart renders series in WebGL, so we draw them as SVG lines above the
// canvas (same visual layering as uPlot's post-series draw hook) and reposition
// on every `model.updated` using the chart's y scale.

const SVGNS = 'http://www.w3.org/2000/svg'

export interface RefLine {
  y: number
  dashed: boolean
}

export interface RefLinesConfig {
  lines: RefLine[]
  solidColor: string
  dashedColor: string
}

export function createReferenceLinesPlugin(cfg: { current: RefLinesConfig }): TimeChartPlugin {
  return {
    apply(chart) {
      const g = document.createElementNS(SVGNS, 'g')
      chart.svgLayer.svgNode.appendChild(g)
      const nodes: SVGLineElement[] = []
      let lastConfig: RefLinesConfig | null = null
      let lastPlotLeft = NaN
      let lastPlotRight = NaN
      let lastYMin = NaN
      let lastYMax = NaN
      let lastPlotTop = NaN
      let lastPlotBottom = NaN

      const draw = () => {
        const c = cfg.current
        const xScale = chart.model.xScale
        const yScale = chart.model.yScale
        const [plotLeft, plotRight] = xScale.range()
        const [plotBottom, plotTop] = yScale.range()
        const [yMin, yMax] = yScale.domain()
        // Horizontal references do not depend on the scrolling x domain.
        // Most charts keep the same y range for their lifetime, so avoid
        // rewriting identical SVG attributes every display frame.
        if (c === lastConfig && plotLeft === lastPlotLeft && plotRight === lastPlotRight &&
            plotTop === lastPlotTop && plotBottom === lastPlotBottom &&
            yMin === lastYMin && yMax === lastYMax) return
        lastConfig = c
        lastPlotLeft = plotLeft
        lastPlotRight = plotRight
        lastPlotTop = plotTop
        lastPlotBottom = plotBottom
        lastYMin = yMin
        lastYMax = yMax

        for (let i = 0; i < c.lines.length; i++) {
          const ref = c.lines[i]
          let line = nodes[i]
          if (!line) {
            line = document.createElementNS(SVGNS, 'line')
            g.appendChild(line)
            nodes[i] = line
          }
          const py = yScale(ref.y)
          line.setAttribute('x1', String(plotLeft))
          line.setAttribute('x2', String(plotRight))
          line.setAttribute('y1', String(py))
          line.setAttribute('y2', String(py))
          line.setAttribute('stroke', ref.dashed ? c.dashedColor : c.solidColor)
          line.setAttribute('stroke-width', '1')
          if (ref.dashed) line.setAttribute('stroke-dasharray', '4 4')
          else line.removeAttribute('stroke-dasharray')
          line.style.display = ''
        }
        for (let i = c.lines.length; i < nodes.length; i++) nodes[i].style.display = 'none'
      }

      chart.model.updated.on(draw)
      chart.model.resized.on(draw)
      draw()
    },
  }
}
