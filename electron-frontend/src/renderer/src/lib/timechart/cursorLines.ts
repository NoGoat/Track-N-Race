import type { TimeChartPlugin } from './tc'

// Vertical playback cursors drawn into the SVG overlay (Analysis split mode
// marks where each compared car currently sits on the map, like the scrubber in
// a video editor's timeline). The horizontal reference lines next door only
// move when the y scale does; these follow a clock that advances every display
// frame, so the plugin also hands back a redraw handle for an external rAF loop.

const SVGNS = 'http://www.w3.org/2000/svg'

export interface CursorLine {
  /** Position in x-scale units (seconds or metres); NaN hides the cursor. */
  x: number
  color: string
}

export interface CursorLinesConfig {
  lines: CursorLine[]
}

export interface CursorLinesHandle {
  redraw: (() => void) | null
}

export function createCursorLinesPlugin(
  cfg: { current: CursorLinesConfig },
  handle: CursorLinesHandle,
): TimeChartPlugin {
  return {
    apply(chart) {
      const g = document.createElementNS(SVGNS, 'g')
      chart.svgLayer.svgNode.appendChild(g)
      const nodes: SVGLineElement[] = []
      const pixels: number[] = []

      const draw = () => {
        const c = cfg.current
        const xScale = chart.model.xScale
        const yScale = chart.model.yScale
        const [plotBottom, plotTop] = yScale.range().map(Number)
        const [xMin, xMax] = xScale.domain().map(Number)

        pixels.length = 0
        for (const cursor of c.lines) {
          // Hide rather than clamp: a cursor pinned to the edge of a zoomed
          // window would read as a car parked on a corner it has long passed.
          pixels.push(Number.isFinite(cursor.x) && cursor.x >= xMin && cursor.x <= xMax
            ? Number(xScale(cursor.x))
            : NaN)
        }
        // On a time axis both laps share one x, so the second cursor would
        // paint over the first and hide a colour. Split coincident cursors by a
        // pixel instead, which reads as one thick two-tone line.
        for (let i = 1; i < pixels.length; i++) {
          for (let j = 0; j < i; j++) {
            if (Number.isNaN(pixels[i]) || Number.isNaN(pixels[j])) continue
            if (Math.abs(pixels[i] - pixels[j]) >= 2) continue
            pixels[j] -= 1
            pixels[i] += 1
          }
        }

        for (let i = 0; i < c.lines.length; i++) {
          let line = nodes[i]
          if (!line) {
            line = document.createElementNS(SVGNS, 'line')
            line.setAttribute('stroke-width', '1.5')
            line.setAttribute('pointer-events', 'none')
            g.appendChild(line)
            nodes[i] = line
          }
          const x = pixels[i]
          if (Number.isNaN(x)) {
            line.style.display = 'none'
            continue
          }
          line.setAttribute('x1', String(x))
          line.setAttribute('x2', String(x))
          line.setAttribute('y1', String(plotTop))
          line.setAttribute('y2', String(plotBottom))
          line.setAttribute('stroke', c.lines[i].color)
          line.style.display = ''
        }
        for (let i = c.lines.length; i < nodes.length; i++) nodes[i].style.display = 'none'
      }

      handle.redraw = draw
      chart.model.updated.on(draw)
      chart.model.resized.on(draw)
      chart.model.disposing.on(() => {
        if (handle.redraw === draw) handle.redraw = null
        g.remove()
      })
      draw()
    },
  }
}
