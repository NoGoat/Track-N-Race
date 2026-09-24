import type { TimeChartPlugin } from './tc'

// Vertical playback cursors drawn into the SVG overlay (Analysis split mode
// marks where each compared car currently sits on the map, like the scrubber in
// a video editor's timeline). The horizontal reference lines next door only
// move when the y scale does; these follow a clock that advances every display
// frame, so the plugin also hands back a redraw handle for an external rAF loop.

const SVGNS = 'http://www.w3.org/2000/svg'
const GUIDE_WIDTH = 1.5
const GUIDE_OPACITY = 0.58

function makeLine(width: number, opacity: number): SVGLineElement {
  const line = document.createElementNS(SVGNS, 'line')
  line.setAttribute('stroke-width', String(width))
  line.setAttribute('stroke-opacity', String(opacity))
  line.setAttribute('vector-effect', 'non-scaling-stroke')
  line.setAttribute('pointer-events', 'none')
  return line
}

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
      g.setAttribute('aria-hidden', 'true')
      g.setAttribute('pointer-events', 'none')
      chart.svgLayer.svgNode.appendChild(g)
      const nodes: SVGLineElement[] = []
      const pixels: number[] = []
      const guidePixels: number[] = []

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

        guidePixels.splice(0, guidePixels.length, ...pixels)
        // The compared laps normally share an x on a time axis. Keep the two
        // guides centred on the real sample, but split them by a line width so
        // both map colours remain visible.
        if (pixels.length === 2 && Number.isFinite(pixels[0]) && Number.isFinite(pixels[1]) &&
            Math.abs(pixels[1] - pixels[0]) < GUIDE_WIDTH * 2) {
          guidePixels[0] -= GUIDE_WIDTH / 2
          guidePixels[1] += GUIDE_WIDTH / 2
        }

        for (let i = 0; i < c.lines.length; i++) {
          let guide = nodes[i]
          if (!guide) {
            guide = makeLine(GUIDE_WIDTH, GUIDE_OPACITY)
            guide.setAttribute('stroke-dasharray', '2 4')
            guide.setAttribute('stroke-linecap', 'round')
            g.appendChild(guide)
            nodes[i] = guide
          }
          const x = guidePixels[i]
          if (Number.isNaN(x)) {
            guide.style.display = 'none'
            continue
          }
          guide.setAttribute('x1', String(x))
          guide.setAttribute('x2', String(x))
          guide.setAttribute('y1', String(plotTop))
          guide.setAttribute('y2', String(plotBottom))
          guide.setAttribute('stroke', c.lines[i].color)
          guide.style.display = ''
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
