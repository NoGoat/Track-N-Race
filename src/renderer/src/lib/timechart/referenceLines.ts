import type { TimeChartPlugin } from 'timechart/plugins'

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

      const draw = () => {
        const c = cfg.current
        const xScale = chart.model.xScale
        const yScale = chart.model.yScale
        const [plotLeft, plotRight] = xScale.range()

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
