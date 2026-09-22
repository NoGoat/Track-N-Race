import type { TimeChartPlugin } from './tc'

// Vertical playback cursors drawn into the SVG overlay (Analysis split mode
// marks where each compared car currently sits on the map, like the scrubber in
// a video editor's timeline). The horizontal reference lines next door only
// move when the y scale does; these follow a clock that advances every display
// frame, so the plugin also hands back a redraw handle for an external rAF loop.

const SVGNS = 'http://www.w3.org/2000/svg'
const GUIDE_WIDTH = 1
const GUIDE_OPACITY = 0.58
const HALO_WIDTH = 7
const HALO_OPACITY = 0.07
const PLAYHEAD_HALF_WIDTH = 5
const PLAYHEAD_HEIGHT = 9
const PLAYHEAD_GAP = PLAYHEAD_HALF_WIDTH * 2 + 1

interface CursorNodes {
  group: SVGGElement
  halo: SVGLineElement
  guide: SVGLineElement
  playhead: SVGPathElement
}

function makeLine(width: number, opacity: number): SVGLineElement {
  const line = document.createElementNS(SVGNS, 'line')
  line.setAttribute('stroke-width', String(width))
  line.setAttribute('stroke-opacity', String(opacity))
  line.setAttribute('vector-effect', 'non-scaling-stroke')
  line.setAttribute('pointer-events', 'none')
  return line
}

function playheadPath(centerX: number, pointX: number, top: number): string {
  const left = centerX - PLAYHEAD_HALF_WIDTH
  const right = centerX + PLAYHEAD_HALF_WIDTH
  return `M ${left} ${top} H ${right} V ${top + 4} L ${pointX} ${top + PLAYHEAD_HEIGHT} L ${left} ${top + 4} Z`
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
      const nodes: CursorNodes[] = []
      const pixels: number[] = []
      const guidePixels: number[] = []
      const playheadPixels: number[] = []

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
        playheadPixels.splice(0, playheadPixels.length, ...pixels)
        // The compared laps normally share an x on a time axis. Keep the two
        // guides centred on the real sample, but split them by a subpixel so
        // both map colours remain visible. Their playheads sit side by side and
        // point back to that same sample instead of becoming one heavy bar.
        if (pixels.length === 2 && Number.isFinite(pixels[0]) && Number.isFinite(pixels[1])) {
          const gap = Math.abs(pixels[1] - pixels[0])
          if (gap < 2) {
            guidePixels[0] -= 0.75
            guidePixels[1] += 0.75
          }
          if (gap < PLAYHEAD_GAP) {
            const midpoint = (pixels[0] + pixels[1]) / 2
            playheadPixels[0] = midpoint - PLAYHEAD_GAP / 2
            playheadPixels[1] = midpoint + PLAYHEAD_GAP / 2
          }
        }

        for (let i = 0; i < c.lines.length; i++) {
          let node = nodes[i]
          if (!node) {
            const group = document.createElementNS(SVGNS, 'g')
            const halo = makeLine(HALO_WIDTH, HALO_OPACITY)
            halo.setAttribute('stroke-linecap', 'round')
            const guide = makeLine(GUIDE_WIDTH, GUIDE_OPACITY)
            guide.setAttribute('stroke-dasharray', '2 4')
            guide.setAttribute('stroke-linecap', 'round')
            const playhead = document.createElementNS(SVGNS, 'path')
            playhead.setAttribute('stroke', 'var(--bg-panel)')
            playhead.setAttribute('stroke-width', '1.5')
            playhead.setAttribute('stroke-linejoin', 'round')
            playhead.setAttribute('paint-order', 'stroke fill')
            playhead.setAttribute('vector-effect', 'non-scaling-stroke')
            group.append(halo, guide, playhead)
            g.appendChild(group)
            node = { group, halo, guide, playhead }
            nodes[i] = node
          }
          const x = pixels[i]
          if (Number.isNaN(x)) {
            node.group.style.display = 'none'
            continue
          }
          const guideX = guidePixels[i]
          const playheadTop = plotTop + 1
          for (const line of [node.halo, node.guide]) {
            line.setAttribute('x1', String(guideX))
            line.setAttribute('x2', String(guideX))
            line.setAttribute('y1', String(playheadTop + PLAYHEAD_HEIGHT))
            line.setAttribute('y2', String(plotBottom))
            line.setAttribute('stroke', c.lines[i].color)
          }
          node.playhead.setAttribute('d', playheadPath(playheadPixels[i], guideX, playheadTop))
          node.playhead.setAttribute('fill', c.lines[i].color)
          node.group.style.display = ''
        }
        for (let i = c.lines.length; i < nodes.length; i++) nodes[i].group.style.display = 'none'
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
