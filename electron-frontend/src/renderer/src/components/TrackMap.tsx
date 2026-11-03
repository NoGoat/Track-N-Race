import { useRef, useEffect, useLayoutEffect, useState, useMemo, useCallback, memo, type ReactNode } from 'react'
import { type SingleValue } from 'react-select'
import Select from '../lib/AnimatedSelect'
import { buildSelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import { Maximize2, Minimize2, Pencil } from 'lucide-react'
import { useSize } from '../hooks/useSize'
import { TRACK_MAPS, type TrackMapData } from '../lib/trackMaps'
import { decodeBinaryBatch } from '../lib/decodeBinaryBatch'
import { useLabels } from '../lib/labels'
import DriverSelect from './DriverSelect'
import { buildDriverOptions, type DriverOption } from '../lib/driverOptions'
import type { TrackPaint } from '../lib/analyzeMapPaint'
import type { CarPosition, ParticipantsMsg, PositionsMsg, TrackMapResolution } from '../types'
import { useResolvedTrackMap, useTrackMapStore } from '../stores/trackMapStore'
import EditMapModal from './EditMapModal'

type ZoomOption = { value: number; label: string }
type MapOption = { value: number; label: string }
type AeroOverlay = 'drs' | 'slm-dry' | 'slm-wet'
type AeroOption = { value: AeroOverlay; label: string }

/** A stretch of the lap line, between two fractions of the lap from the line,
 *  highlighted in a colour, with what to show on hover: a braking zone, for
 *  example. */
export interface TrackMark {
  fromFraction: number
  toFraction: number
  color: string
  /** 0-1; 1 when left out. Dims a stretch against its neighbours. */
  opacity?: number
  tooltip?: ReactNode
}

export interface TrackMapMarker {
  x: number
  z: number
  label: string
  color: string
}

const ZOOM_OPTIONS: ZoomOption[] = [
  { value: 2, label: '2x' },
  { value: 4, label: '4x' },
  { value: 8, label: '8x' },
  { value: 16, label: '16x' },
]

const BASE_MAP_OPTIONS: MapOption[] = Object.values(TRACK_MAPS)
  .sort((a, b) => a.track_name.localeCompare(b.track_name))
  .map(map => ({ value: map.track_id, label: map.track_name }))

const AERO_OPTIONS: AeroOption[] = [
  { value: 'slm-dry', label: 'SLM-Dry' },
  { value: 'slm-wet', label: 'SLM-Wet' },
  { value: 'drs', label: 'DRS' },
]

function aeroOverlayFromTelemetry(aeroMode: 'drs' | 'slm', slmTrackStatus: number): AeroOverlay {
  if (aeroMode === 'drs') return 'drs'
  return slmTrackStatus === 1 ? 'slm-wet' : 'slm-dry'
}


const SECTOR_COLORS_DARK  = ['#E8002D', '#0090D0', '#FFD700']
const SECTOR_COLORS_LIGHT = ['#B3132B', '#0D47A1', '#765900']
const SECTOR_COLORS       = SECTOR_COLORS_DARK
const DRS_COLOR_DARK      = '#39B54A'
const DRS_COLOR_LIGHT     = '#237A32'
const SLM_DRY_COLOR_DARK  = '#46E396' // SLM Normal grip  (slm_dry / Full status)   — bright green
const SLM_DRY_COLOR_LIGHT = '#16804A'
const SLM_WET_COLOR_DARK  = '#22D3EE' // SLM Reduced grip (slm_wet / Partial status) — cyan
const SLM_WET_COLOR_LIGHT = '#087F9C'
const TRAP_COLOR_DARK     = '#8881DE'
const TRAP_COLOR_LIGHT    = '#5B54B4'
const SF_COLOR      = '#E8002D'
const OVERTAKE_COLOR_DARK  = '#95E7F0'
const OVERTAKE_COLOR_LIGHT = '#397F88'

const TRACK_PX   = 5
const DRS_PX     = 6
const DRS_OFFSET = 18
const TRAP_R     = 9
const SF_HALF    = 14
const JUNC_HALF  = 10
const DOT_R      = 7
const MAP_PAD    = 24  // CSS-pixel padding outside every rendered map element
// The fitted centerline needs enough additional room for an outward DRS/SLM
// overlay and half its stroke, leaving MAP_PAD visible beyond the dashes.
const MAP_FIT_PAD = MAP_PAD + DRS_OFFSET + DRS_PX / 2
const LABEL_MIN_W = 38
const LABEL_H    = 16
const LABEL_GAP  = 5
const LABEL_R    = 3   // corner radius
const ACCENT_W   = 3   // left livery-color bar width
const LABEL_TEXT_PAD_X = 6
const LABEL_FONT = 'bold 9px "Cascadia Code", ui-monospace, monospace'
const LABEL_SPRITE_PAD = 8
const LABEL_SPRITE_CACHE_MAX = 256
interface LabelSprite {
  canvas: HTMLCanvasElement
  labelWidth: number
}
const labelSpriteCache = new Map<string, LabelSprite>()

function abbrev(name: string): string {
  const parts = name.trim().split(/\s+/)
  return parts[parts.length - 1].slice(0, 3).toUpperCase()
}

// ── Geometry ──────────────────────────────────────────────────────────────────

function rotatePoint(
  px: number, py: number,
  cos: number, sin: number,
  cx: number, cy: number,
): [number, number] {
  const dx = px - cx
  const dy = py - cy
  return [cos * dx - sin * dy + cx, sin * dx + cos * dy + cy]
}

function toCanvas(p: [number, number], scale: number, ox: number, oy: number): [number, number] {
  return [p[0] * scale + ox, p[1] * scale + oy]
}

function perp(pts: [number, number][], i: number): [number, number] {
  const lo = Math.max(0, i - 1)
  const hi = Math.min(pts.length - 1, i + 1)
  const dx = pts[hi][0] - pts[lo][0]
  const dy = pts[hi][1] - pts[lo][1]
  const len = Math.hypot(dx, dy) || 1
  return [-dy / len, dx / len]
}

function rawToViewBox(x: number, z: number, t: TrackMapData['transform']): [number, number] {
  return [
    (x - t.min_x) * t.scale + t.off_x,
    (z - t.min_z) * t.scale + t.off_z,
  ]
}

// ── DRS zone reconstruction ─────────────────────────────────────────────────────
// DRS zones store only their start/end points; the polyline is re-derived as the
// slice of the track centerline between them (following driving direction, wrapping
// past the start/finish line when needed).

/** Concatenate the sector polylines into one closed loop, dropping consecutive
 *  duplicate vertices (including the shared start/finish closing vertex). */
function buildCenterline(map: TrackMapData): [number, number][] {
  const ordered = [...map.sectors].sort((a, b) => a.index - b.index)
  const pts: [number, number][] = []
  for (const s of ordered) {
    for (const p of s.points as [number, number][]) {
      const last = pts[pts.length - 1]
      if (!last || last[0] !== p[0] || last[1] !== p[1]) pts.push([p[0], p[1]])
    }
  }
  if (pts.length > 1) {
    const first = pts[0]
    const last  = pts[pts.length - 1]
    if (first[0] === last[0] && first[1] === last[1]) pts.pop()
  }
  return pts
}

function nearestIdx(pts: [number, number][], p: [number, number]): number {
  let best = 0, bestD = Infinity
  for (let i = 0; i < pts.length; i++) {
    const dx = pts[i][0] - p[0]
    const dy = pts[i][1] - p[1]
    const d = dx * dx + dy * dy
    if (d < bestD) { bestD = d; best = i }
  }
  return best
}

/** Centerline indices from nearest(`start`) to nearest(`end`), forward with wraparound. */
function sliceZoneIdx(
  centerline: [number, number][],
  start: [number, number],
  end: [number, number],
): number[] {
  const N = centerline.length
  if (N === 0) return []
  const si = nearestIdx(centerline, start)
  const ei = nearestIdx(centerline, end)
  const out: number[] = []
  let i = si
  for (let n = 0; n < N; n++) {
    out.push(i)
    if (i === ei) break
    i = (i + 1) % N
  }
  return out
}

/** The DRS polyline: centerline slice from `start` to `end`, forward with wraparound. */
function sliceZone(
  centerline: [number, number][],
  start: [number, number],
  end: [number, number],
): [number, number][] {
  return sliceZoneIdx(centerline, start, end).map(i => centerline[i])
}

/** Even-odd containment works for ordinary circuits and for separate lobes of
 *  a self-intersecting circuit such as Suzuka. */
function pointInPolygon(point: [number, number], polygon: [number, number][]): boolean {
  const [x, y] = point
  let inside = false
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
    const [xi, yi] = polygon[i]
    const [xj, yj] = polygon[j]
    if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) {
      inside = !inside
    }
  }
  return inside
}

/** Pick which local normal points outside the closed circuit. The vote is made
 *  per zone so a figure-eight's two lobes can use opposite sides, while a single
 *  zone keeps a stable offset and cannot zig-zag due to boundary precision. */
function findOutwardSign(zone: [number, number][], centerline: [number, number][]): 1 | -1 {
  const PROBE_DISTANCE = 2
  const MAX_PROBES = 128
  const step = Math.max(1, Math.floor(zone.length / MAX_PROBES))
  let positiveVotes = 0
  let negativeVotes = 0

  for (let i = 0; i < zone.length; i += step) {
    const [nx, ny] = perp(zone, i)
    const [x, y] = zone[i]
    const positiveInside = pointInPolygon([x + nx * PROBE_DISTANCE, y + ny * PROBE_DISTANCE], centerline)
    const negativeInside = pointInPolygon([x - nx * PROBE_DISTANCE, y - ny * PROBE_DISTANCE], centerline)
    if (positiveInside === negativeInside) continue
    if (positiveInside) negativeVotes++
    else                positiveVotes++
  }

  if (positiveVotes !== negativeVotes) return positiveVotes > negativeVotes ? 1 : -1

  // Degenerate/ambiguous fallback: use the loop winding. Positive signed area
  // is counter-clockwise in these coordinates, whose outside is the right side.
  let twiceArea = 0
  for (let i = 0, j = centerline.length - 1; i < centerline.length; j = i++) {
    twiceArea += centerline[j][0] * centerline[i][1] - centerline[i][0] * centerline[j][1]
  }
  return twiceArea >= 0 ? -1 : 1
}

// Where a DRS zone wraps the start/finish line, the last-sector→first-sector
// seam introduces a tiny lateral drift that kinks the local tangent by tens of
// degrees. Drawn as a perpendicular offset, that kink becomes a visible chevron.
// Real corners never turn more than ~2° per (densely-sampled) vertex, so an
// isolated high-angle vertex is unambiguously the seam and is safe to relax.
const SEAM_ANGLE_DEG     = 6    // turn angle that flags the seam kink
const SEAM_SMOOTH_SPAN   = 6    // neighbours each side of a flagged vertex to relax
const SEAM_SMOOTH_PASSES = 20

function turnAngleDeg(a: [number, number], b: [number, number], c: [number, number]): number {
  const d1x = b[0] - a[0], d1y = b[1] - a[1]
  const d2x = c[0] - b[0], d2y = c[1] - b[1]
  const l1 = Math.hypot(d1x, d1y) || 1
  const l2 = Math.hypot(d2x, d2y) || 1
  const dot = (d1x * d2x + d1y * d2y) / (l1 * l2)
  return (Math.acos(Math.max(-1, Math.min(1, dot))) * 180) / Math.PI
}

/** Relax the isolated tangent kink at the start/finish seam. Each flagged spike
 *  vertex and its neighbours are pulled toward their local midpoint, but with a
 *  cosine taper — full strength at the spike, fading to zero at the window edge.
 *  That relaxes the kink while blending smoothly into the untouched track (a
 *  uniform window would just push the kink to its boundary). Endpoints, and any
 *  vertex outside a flagged window, never move — so the zone keeps its exact
 *  start/end and real corners are untouched. */
function smoothSeam(pts: [number, number][]): [number, number][] {
  const n = pts.length
  if (n < 5) return pts
  const weight = new Array<number>(n).fill(0)
  let any = false
  for (let i = 1; i < n - 1; i++) {
    if (turnAngleDeg(pts[i - 1], pts[i], pts[i + 1]) > SEAM_ANGLE_DEG) {
      for (let j = i - SEAM_SMOOTH_SPAN; j <= i + SEAM_SMOOTH_SPAN; j++) {
        if (j > 0 && j < n - 1) {
          const taper = 0.5 * (1 + Math.cos((Math.PI * Math.abs(j - i)) / (SEAM_SMOOTH_SPAN + 1)))
          if (taper > weight[j]) { weight[j] = taper; any = true }
        }
      }
    }
  }
  if (!any) return pts
  let out: [number, number][] = pts.map(p => [p[0], p[1]])
  for (let pass = 0; pass < SEAM_SMOOTH_PASSES; pass++) {
    const next: [number, number][] = out.map(p => [p[0], p[1]])
    for (let i = 1; i < n - 1; i++) {
      const w = weight[i]
      if (w > 0) {
        const mx = (out[i - 1][0] + out[i + 1][0]) / 2
        const my = (out[i - 1][1] + out[i + 1][1]) / 2
        next[i][0] = out[i][0] + w * (mx - out[i][0])
        next[i][1] = out[i][1] + w * (my - out[i][1])
      }
    }
    out = next
  }
  return out
}

// ── PreparedMap — precomputed once per map+rotation ───────────────────────────

interface SectorJunction {
  pt:    [number, number]
  nx:    number
  ny:    number
  color: string
}

interface OffsetZone {
  track_points: [number, number][]
  outwardSign: 1 | -1
}

interface PreparedMap {
  sectors:     Array<{ index: number; points: [number, number][] }>
  drsZones:    OffsetZone[]
  slmDry:      OffsetZone[]   // 2026 SLM — Full-status zones
  slmWet:      OffsetZone[]   // 2026 SLM — Partial-status zones
  speedTraps:  [number, number][]
  startFinish: [number, number] | null
  overtakeDetection: [number, number] | null
  overtakeActivation: SectorJunction | null
  s1pts:       [number, number][]
  sfIdx:       number
  junctions:   SectorJunction[]
  /** The rotated centerline from the start/finish line in driving order, and
   *  each vertex's fraction of the lap by arc length. Painted modes draw it. */
  lapLine:     [number, number][]
  lapFraction: Float64Array
  bounds:      { minX: number; minY: number; w: number; h: number }
  rotCos:      number
  rotSin:      number
  rotCx:       number
  rotCy:       number
}

function prepareMap(map: TrackMapData): PreparedMap {
  const rotRad = ((map.rotation_deg ?? 0) * Math.PI) / 180
  const cos = Math.cos(rotRad)
  const sin = Math.sin(rotRad)
  const cx = map.view_box.width  / 2
  const cy = map.view_box.height / 2

  const rot = (p: [number, number]): [number, number] =>
    rotatePoint(p[0], p[1], cos, sin, cx, cy)

  let minX = Infinity, maxX = -Infinity
  let minY = Infinity, maxY = -Infinity

  const sectors = map.sectors.map(sector => {
    const pts = (sector.points as [number, number][]).map(rot)
    for (const [x, y] of pts) {
      if (x < minX) minX = x;  if (x > maxX) maxX = x
      if (y < minY) minY = y;  if (y > maxY) maxY = y
    }
    return { index: sector.index, points: pts }
  })

  const centerline = buildCenterline(map)
  const rotatedCenterline = centerline.map(rot)
  const reconstruct = (zones: Array<{ start: [number, number]; end: [number, number] }>) =>
    zones.map(z => {
      const track_points = smoothSeam(sliceZone(centerline, z.start, z.end)).map(rot)
      return { track_points, outwardSign: findOutwardSign(track_points, rotatedCenterline) }
    })

  const drsZones = reconstruct(map.drs_zones as Array<{ start: [number, number]; end: [number, number] }>)
  const slmDry   = reconstruct(map.slm_dry ?? [])
  const slmWet   = reconstruct(map.slm_wet ?? [])
  const speedTraps = (map.speed_traps as [number, number][]).map(rot)
  const startFinish = map.start_finish ? rot(map.start_finish as [number, number]) : null
  const overtakeDetection = map.overtake_detection_point
    ? rot(map.overtake_detection_point)
    : null
  const overtakeActivationPt = map.overtake_activation_point
    ? rot(map.overtake_activation_point)
    : null
  let overtakeActivation: SectorJunction | null = null
  if (overtakeActivationPt && rotatedCenterline.length > 1) {
    const activationIdx = nearestIdx(rotatedCenterline, overtakeActivationPt)
    const [nx, ny] = perp(rotatedCenterline, activationIdx)
    overtakeActivation = { pt: overtakeActivationPt, nx, ny, color: OVERTAKE_COLOR_DARK }
  }

  const s1pts = sectors[0]?.points ?? []
  let sfIdx = 0
  if (startFinish && s1pts.length > 0) {
    let bestD = Infinity
    for (let i = 0; i < s1pts.length; i++) {
      const d = Math.hypot(s1pts[i][0] - startFinish[0], s1pts[i][1] - startFinish[1])
      if (d < bestD) { bestD = d; sfIdx = i }
    }
  }

  // Colored tick marks at S1/S2 and S2/S3 boundaries
  const junctions: SectorJunction[] = []
  for (let si = 0; si < sectors.length - 1; si++) {
    const spts = sectors[si].points
    if (spts.length < 2) continue
    const lastIdx = spts.length - 1
    const [nx, ny] = perp(spts, lastIdx)
    junctions.push({ pt: spts[lastIdx], nx, ny, color: SECTOR_COLORS[si + 1] })
  }

  const lapStart = startFinish && rotatedCenterline.length > 0 ? nearestIdx(rotatedCenterline, startFinish) : 0
  const lapLine = [...rotatedCenterline.slice(lapStart), ...rotatedCenterline.slice(0, lapStart)]
  const lapFraction = new Float64Array(lapLine.length)
  let lapArc = 0
  for (let i = 1; i < lapLine.length; i++) {
    lapArc += Math.hypot(lapLine[i][0] - lapLine[i - 1][0], lapLine[i][1] - lapLine[i - 1][1])
    lapFraction[i] = lapArc
  }
  const lapTotal = lapLine.length > 1
    ? lapArc + Math.hypot(lapLine[0][0] - lapLine[lapLine.length - 1][0], lapLine[0][1] - lapLine[lapLine.length - 1][1])
    : 0
  if (lapTotal > 0) for (let i = 0; i < lapFraction.length; i++) lapFraction[i] /= lapTotal

  return {
    sectors, drsZones, slmDry, slmWet, speedTraps, startFinish,
    overtakeDetection, overtakeActivation, s1pts, sfIdx, junctions, lapLine, lapFraction,
    bounds: { minX, minY, w: maxX - minX, h: maxY - minY },
    rotCos: cos, rotSin: sin, rotCx: cx, rotCy: cy,
  }
}

/** Compute scale + offset so the tight track bounds fill the canvas with padding. */
function buildLayout(prep: PreparedMap, cw: number, ch: number) {
  const { minX, minY, w, h } = prep.bounds
  const scale = Math.min((cw - 2 * MAP_FIT_PAD) / w, (ch - 2 * MAP_FIT_PAD) / h)
  const ox = (cw - w * scale) / 2 - minX * scale
  const oy = (ch - h * scale) / 2 - minY * scale
  return { scale, ox, oy }
}

// ── Drawing primitives ────────────────────────────────────────────────────────

function drawPolyline(
  ctx: CanvasRenderingContext2D,
  pts: [number, number][],
  scale: number, ox: number, oy: number,
  color: string, lineWidth: number,
) {
  if (pts.length < 2) return
  ctx.beginPath()
  ctx.strokeStyle = color
  ctx.lineWidth   = lineWidth
  ctx.lineCap     = 'round'
  ctx.lineJoin    = 'round'
  const [fx, fy] = toCanvas(pts[0], scale, ox, oy)
  ctx.moveTo(fx, fy)
  for (let i = 1; i < pts.length; i++) {
    const [x, y] = toCanvas(pts[i], scale, ox, oy)
    ctx.lineTo(x, y)
  }
  ctx.stroke()
}

/** A line running alongside the track, offset perpendicular toward the outside
 *  selected during map preparation. */
function drawOffsetLine(
  ctx: CanvasRenderingContext2D,
  pts: [number, number][],
  outwardSign: 1 | -1,
  scale: number, ox: number, oy: number,
  color: string, dashed: boolean,
  zoomFactor: number = 1,
  trackZoomFactor: number = 1,
) {
  if (pts.length < 2) return
  ctx.beginPath()
  ctx.strokeStyle = color
  ctx.lineWidth   = DRS_PX * zoomFactor
  ctx.lineCap     = dashed ? 'butt' : 'round'
  ctx.lineJoin    = dashed ? 'miter' : 'round'
  ctx.setLineDash(dashed ? [3 * zoomFactor, 3 * zoomFactor] : [])
  for (let i = 0; i < pts.length; i++) {
    const [nx, ny] = perp(pts, i)
    const [cx2, cy2] = toCanvas(pts[i], scale, ox, oy)
    const offset = Math.max(DRS_OFFSET * zoomFactor, (TRACK_PX * trackZoomFactor) / 2 + 8 * zoomFactor)
    const px = cx2 + nx * offset * outwardSign
    const py = cy2 + ny * offset * outwardSign
    if (i === 0) ctx.moveTo(px, py)
    else         ctx.lineTo(px, py)
  }
  ctx.stroke()
  ctx.setLineDash([])
}

function drawSpeedTrap(
  ctx: CanvasRenderingContext2D,
  pt: [number, number],
  scale: number, ox: number, oy: number,
  color: string,
  zoomFactor: number = 1,
  trackZoomFactor: number = 1,
) {
  const [cx2, cy2] = toCanvas(pt, scale, ox, oy)
  const trapR = Math.max(TRAP_R * zoomFactor, (TRACK_PX * trackZoomFactor) / 2 + 4 * zoomFactor)
  ctx.beginPath()
  ctx.arc(cx2, cy2, trapR, 0, Math.PI * 2)
  ctx.fillStyle = color
  ctx.fill()
}

function drawStartFinish(
  ctx: CanvasRenderingContext2D,
  pt: [number, number],
  normalPts: [number, number][],
  idx: number,
  scale: number, ox: number, oy: number,
  zoomFactor: number = 1,
  trackZoomFactor: number = 1,
  colorOverride?: string,
) {
  const [nx, ny] = perp(normalPts, idx)
  const [cx2, cy2] = toCanvas(pt, scale, ox, oy)
  ctx.beginPath()
  ctx.strokeStyle = colorOverride ?? SF_COLOR
  ctx.lineWidth   = 2.5 * zoomFactor
  ctx.lineCap     = 'round'
  const sfHalf = Math.max(SF_HALF * zoomFactor, (TRACK_PX * trackZoomFactor) / 2 + 4 * zoomFactor)
  ctx.moveTo(cx2 - nx * sfHalf, cy2 - ny * sfHalf)
  ctx.lineTo(cx2 + nx * sfHalf, cy2 + ny * sfHalf)
  ctx.stroke()
}

function drawSectorJunction(
  ctx: CanvasRenderingContext2D,
  j: SectorJunction,
  scale: number, ox: number, oy: number,
  colorOverride: string | undefined,
  zoomFactor: number = 1,
  trackZoomFactor: number = 1,
) {
  const [cx2, cy2] = toCanvas(j.pt, scale, ox, oy)
  ctx.beginPath()
  ctx.strokeStyle = colorOverride ?? j.color
  ctx.lineWidth   = 3 * zoomFactor
  ctx.lineCap     = 'round'
  const juncHalf = Math.max(JUNC_HALF * zoomFactor, (TRACK_PX * trackZoomFactor) / 2 + 4 * zoomFactor)
  ctx.moveTo(cx2 - j.nx * juncHalf, cy2 - j.ny * juncHalf)
  ctx.lineTo(cx2 + j.nx * juncHalf, cy2 + j.ny * juncHalf)
  ctx.stroke()
}

function drawOvertakeDetection(
  ctx: CanvasRenderingContext2D,
  pt: [number, number],
  scale: number, ox: number, oy: number,
  color: string,
  zoomFactor: number = 1,
  trackZoomFactor: number = 1,
) {
  const [cx2, cy2] = toCanvas(pt, scale, ox, oy)
  const markerR = Math.max(TRAP_R * zoomFactor, (TRACK_PX * trackZoomFactor) / 2 + 4 * zoomFactor)
  ctx.beginPath()
  ctx.arc(cx2, cy2, markerR, 0, Math.PI * 2)
  ctx.fillStyle = color
  ctx.fill()
}

function drawLabel(
  ctx: CanvasRenderingContext2D,
  cx: number, cy: number,
  text: string,
  color: string,
  centerOnDot?: boolean,
  isDark: boolean = true,
): void {
  const dpr = Math.max(1, ctx.getTransform().a)
  const key = `${text}|${color}|${isDark ? 1 : 0}|${dpr}`
  let sprite = labelSpriteCache.get(key)
  if (!sprite) {
    const canvas = document.createElement('canvas')
    const spriteCtx = canvas.getContext('2d')
    if (!spriteCtx) return
    spriteCtx.font = LABEL_FONT
    const labelWidth = Math.max(
      LABEL_MIN_W,
      Math.ceil(spriteCtx.measureText(text).width + ACCENT_W + LABEL_TEXT_PAD_X * 2),
    )
    const cssWidth = labelWidth + LABEL_SPRITE_PAD * 2
    const cssHeight = LABEL_H + LABEL_SPRITE_PAD * 2
    canvas.width = Math.ceil(cssWidth * dpr)
    canvas.height = Math.ceil(cssHeight * dpr)
    spriteCtx.setTransform(dpr, 0, 0, dpr, 0, 0)
    const x = LABEL_SPRITE_PAD
    const y = LABEL_SPRITE_PAD

    spriteCtx.shadowColor = isDark ? 'rgba(0,0,0,0.55)' : 'rgba(0,0,0,0.15)'
    spriteCtx.shadowBlur = 6
    spriteCtx.shadowOffsetY = 2
    spriteCtx.beginPath()
    spriteCtx.roundRect(x, y, labelWidth, LABEL_H, LABEL_R)
    spriteCtx.fillStyle = isDark ? 'rgba(10,15,30,0.92)' : 'rgba(241,240,236,0.97)'
    spriteCtx.fill()
    if (!isDark) {
      spriteCtx.strokeStyle = 'rgba(0,0,0,0.15)'
      spriteCtx.lineWidth = 0.5
      spriteCtx.stroke()
    }

    spriteCtx.shadowColor = 'transparent'
    spriteCtx.shadowBlur = 0
    spriteCtx.shadowOffsetY = 0
    spriteCtx.beginPath()
    spriteCtx.roundRect(x, y, ACCENT_W, LABEL_H, [LABEL_R, 0, 0, LABEL_R])
    spriteCtx.fillStyle = color
    spriteCtx.fill()
    spriteCtx.font = LABEL_FONT
    spriteCtx.textAlign = 'center'
    spriteCtx.textBaseline = 'middle'
    spriteCtx.fillStyle = isDark ? '#ffffff' : '#111827'
    spriteCtx.fillText(text, x + labelWidth / 2 + ACCENT_W / 2, y + LABEL_H / 2)
    if (labelSpriteCache.size >= LABEL_SPRITE_CACHE_MAX) {
      const oldest = labelSpriteCache.keys().next().value
      if (oldest !== undefined) labelSpriteCache.delete(oldest)
    }
    sprite = { canvas, labelWidth }
    labelSpriteCache.set(key, sprite)
  }

  const bx = cx - sprite.labelWidth / 2
  const by = centerOnDot ? cy - LABEL_H / 2 : cy - DOT_R - LABEL_GAP - LABEL_H
  const cssWidth = sprite.canvas.width / dpr
  const cssHeight = sprite.canvas.height / dpr
  // Car positions can be subpixel, but translating a pre-rasterized text
  // sprite by a fractional backing-store pixel makes the compositor bilinearly
  // resample every glyph. Snap only the label bitmap; the Canvas 2D dot keeps
  // its full subpixel precision.
  const drawX = Math.round((bx - LABEL_SPRITE_PAD) * dpr) / dpr
  const drawY = Math.round((by - LABEL_SPRITE_PAD) * dpr) / dpr
  ctx.drawImage(sprite.canvas, drawX, drawY, cssWidth, cssHeight)
}

function drawCarDots(
  ctx: CanvasRenderingContext2D,
  cars: CarPosition[],
  participants: ParticipantsMsg | null,
  map: TrackMapData,
  prep: PreparedMap,
  scale: number, ox: number, oy: number,
  driversMode: 'dots' | 'both' | 'labels',
  isDark: boolean,
) {
  type LabelJob = { cx: number; cy: number; text: string; color: string }
  const labelJobs: LabelJob[] = []

  for (const car of cars) {
    if (car.x === 0 && car.z === 0) continue
    const driver = participants?.drivers.find(d => d.idx === car.idx)
    if (!driver) continue

    const [vx, vy] = rawToViewBox(car.x, car.z, map.transform)
    const [rx, ry] = rotatePoint(vx, vy, prep.rotCos, prep.rotSin, prep.rotCx, prep.rotCy)
    const [cx2, cy2] = toCanvas([rx, ry], scale, ox, oy)

    if (driversMode === 'dots' || driversMode === 'both') {
      ctx.beginPath()
      ctx.arc(cx2, cy2, DOT_R, 0, Math.PI * 2)
      ctx.fillStyle = driver.livery_color
      ctx.fill()
    }

    const text = driver.name.trim() ? abbrev(driver.name) : String(driver.race_number)
    labelJobs.push({ cx: cx2, cy: cy2, text, color: driver.livery_color })
  }

  if (driversMode === 'both' || driversMode === 'labels') {
    const centerOnDot = driversMode === 'labels'
    for (const job of labelJobs) {
      drawLabel(ctx, job.cx, job.cy, job.text, job.color, centerOnDot, isDark)
    }
  }
}

/** The closed lap line, each segment in the paint's colour at its midpoint.
 *  Consecutive segments of one colour share a stroke so their joins stay clean. */
function drawPaintedTrack(
  ctx: CanvasRenderingContext2D,
  prep: PreparedMap,
  paint: TrackPaint,
  scale: number, ox: number, oy: number,
  lineWidth: number,
) {
  const pts = prep.lapLine
  const fractions = prep.lapFraction
  const n = pts.length
  if (n < 2) return
  ctx.lineWidth = lineWidth
  ctx.lineCap   = paint.translucent ? 'butt' : 'round'
  ctx.lineJoin  = 'round'
  let runColor: string | null = null
  const flush = () => {
    if (runColor !== null) {
      ctx.strokeStyle = runColor
      ctx.stroke()
    }
  }
  for (let i = 0; i < n; i++) {
    const next = (i + 1) % n
    const color = paint.colorAt((fractions[i] + (next === 0 ? 1 : fractions[next])) / 2)
    if (color !== runColor) {
      flush()
      runColor = color
      if (color !== null) {
        ctx.beginPath()
        const [x, y] = toCanvas(pts[i], scale, ox, oy)
        ctx.moveTo(x, y)
      }
    }
    if (color !== null) {
      const [x, y] = toCanvas(pts[next], scale, ox, oy)
      ctx.lineTo(x, y)
    }
  }
  flush()
}

// How close the pointer must be to a highlight to show its tooltip, in CSS pixels.
const MARK_HOVER_PX = 8

/** The lap line at a fraction of the lap. */
function lapPointAt(prep: PreparedMap, fraction: number): [number, number] {
  const pts = prep.lapLine
  const fractions = prep.lapFraction
  const n = pts.length
  const f = ((fraction % 1) + 1) % 1
  let lo = 0, hi = n - 1
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1
    if (fractions[mid] <= f) lo = mid
    else hi = mid - 1
  }
  const next = (lo + 1) % n
  const end = next === 0 ? 1 : fractions[next]
  const span = end - fractions[lo]
  const t = span > 0 ? (f - fractions[lo]) / span : 0
  return [pts[lo][0] + (pts[next][0] - pts[lo][0]) * t, pts[lo][1] + (pts[next][1] - pts[lo][1]) * t]
}

/** Each mark's stretch of the lap line drawn over the track, a little wider
 *  than it, in the mark's colour. Returns the canvas points along each
 *  stretch, for hover. */
function drawTrackMarks(
  ctx: CanvasRenderingContext2D,
  prep: PreparedMap,
  marks: readonly TrackMark[],
  scale: number, ox: number, oy: number,
  trackZoomFactor: number,
  isDark: boolean,
): Array<Array<[number, number]>> {
  const paths: Array<Array<[number, number]>> = []
  if (prep.lapLine.length < 2) return paths
  const width = TRACK_PX * trackZoomFactor + 3
  const fractions = prep.lapFraction
  for (const mark of marks) {
    // The lap line from one end of the stretch to the other, one step per vertex.
    const from = mark.fromFraction
    const to = mark.toFraction >= from ? mark.toFraction : mark.toFraction + 1
    const steps: number[] = [from]
    for (let lap = 0; lap <= 1; lap++)
      for (let k = 0; k < fractions.length; k++) {
        const f = fractions[k] + lap
        if (f > from && f < to) steps.push(f)
      }
    steps.push(to)
    const path = steps.map(f => toCanvas(lapPointAt(prep, f), scale, ox, oy))
    const trace = () => {
      ctx.beginPath()
      path.forEach(([x, y], k) => (k === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y)))
    }
    // Square ends, cut exactly where the stretch starts and stops.
    ctx.lineCap = 'butt'
    ctx.lineJoin = 'round'
    // A background-coloured edge keeps the stretch distinct from the track under it.
    trace()
    ctx.strokeStyle = isDark ? '#0c0d12' : '#ebeae6'
    ctx.lineWidth = width + 3
    ctx.stroke()
    trace()
    ctx.save()
    ctx.globalAlpha = mark.opacity ?? 1
    ctx.strokeStyle = mark.color
    ctx.lineWidth = width
    ctx.stroke()
    ctx.restore()
    paths.push(path)
  }
  return paths
}

// ── Main render ───────────────────────────────────────────────────────────────

function renderBackgroundFrame(
  ctx: CanvasRenderingContext2D,
  cw: number, ch: number,
  prep: PreparedMap,
  isDark: boolean,
  sectorColors: boolean,
  layout: { scale: number; ox: number; oy: number },
  effectiveZoom: number = 1,
  mapDimmed: boolean = false,
  aeroMode: 'drs' | 'slm' = 'drs',
  slmTrackStatus: number = -1,   // 0 = Full, 1 = Partial, -1 = unknown
  paint: TrackPaint | null = null,
  marks: readonly TrackMark[] | null = null,
): Array<Array<[number, number]>> {
  ctx.clearRect(0, 0, cw, ch)
  const { scale, ox, oy } = layout
  const zoomFactor = Math.sqrt(effectiveZoom)
  // Stronger scaling factor specifically for track width so cars don't float off-track
  const trackZoomFactor = Math.pow(effectiveZoom, 0.8)

  const trackColor = isDark ? '#ffffff' : '#000000'
  const colors = isDark ? SECTOR_COLORS_DARK : SECTOR_COLORS_LIGHT
  const drsColor = isDark ? DRS_COLOR_DARK : DRS_COLOR_LIGHT
  const slmDryColor = isDark ? SLM_DRY_COLOR_DARK : SLM_DRY_COLOR_LIGHT
  const slmWetColor = isDark ? SLM_WET_COLOR_DARK : SLM_WET_COLOR_LIGHT
  const trapColor = isDark ? TRAP_COLOR_DARK : TRAP_COLOR_LIGHT
  const overtakeColor = isDark ? OVERTAKE_COLOR_DARK : OVERTAKE_COLOR_LIGHT

  if (paint) {
    drawPaintedTrack(ctx, prep, paint, scale, ox, oy, TRACK_PX * trackZoomFactor)
  } else {
    if (mapDimmed) ctx.globalAlpha = 0.4

    for (const sector of prep.sectors) {
      const color = sectorColors ? (colors[sector.index - 1] ?? trackColor) : trackColor
      drawPolyline(ctx, sector.points, scale, ox, oy, color, TRACK_PX * trackZoomFactor)
    }

    if (mapDimmed) ctx.globalAlpha = 1.0
  }

  // With the track in sector colours the boundary ticks are plain; otherwise,
  // including a painted track that hides the sector colours, the ticks carry them.
  const sectorTrack = sectorColors && !paint
  for (let idx = 0; idx < prep.junctions.length; idx++) {
    const j = prep.junctions[idx]
    const color = sectorTrack ? trackColor : (colors[idx + 1] ?? trackColor)
    drawSectorJunction(ctx, j, scale, ox, oy, color, zoomFactor, trackZoomFactor)
  }

  // Overtaking-aid overlay: DRS zones (F1 24/25) or the SLM overlay (F1 26).
  // In SLM, a Partial track status (1) draws the wet zone set, otherwise (Full
  // or unknown) the dry set. Both are dashed, like DRS.
  if (aeroMode === 'slm') {
    const partial = slmTrackStatus === 1
    const zones = partial ? prep.slmWet : prep.slmDry
    const color = partial ? slmWetColor : slmDryColor
    for (const zone of zones) {
      drawOffsetLine(ctx, zone.track_points, zone.outwardSign, scale, ox, oy, color, true, zoomFactor, trackZoomFactor)
    }

    // The overtake detection/activation markers are part of the 2026 SLM rules.
    // Keep them coupled to the active SLM overlay so they never appear with DRS.
    // A painted track leaves them out, as it does speed traps.
    if (prep.overtakeDetection && !paint) {
      drawOvertakeDetection(ctx, prep.overtakeDetection, scale, ox, oy, overtakeColor, zoomFactor, trackZoomFactor)
    }

    if (prep.overtakeActivation && !paint) {
      drawSectorJunction(
        ctx,
        prep.overtakeActivation,
        scale,
        ox,
        oy,
        overtakeColor,
        zoomFactor,
        trackZoomFactor,
      )
    }
  } else {
    for (const zone of prep.drsZones) {
      drawOffsetLine(ctx, zone.track_points, zone.outwardSign, scale, ox, oy, drsColor, true, zoomFactor, trackZoomFactor)
    }
  }

  if (!paint) for (const trap of prep.speedTraps) {
    drawSpeedTrap(ctx, trap, scale, ox, oy, trapColor, zoomFactor, trackZoomFactor)
  }

  if (prep.startFinish) {
    drawStartFinish(ctx, prep.startFinish, prep.s1pts, prep.sfIdx, scale, ox, oy, zoomFactor, trackZoomFactor, sectorTrack ? trackColor : undefined)
  }

  return marks && marks.length > 0
    ? drawTrackMarks(ctx, prep, marks, scale, ox, oy, trackZoomFactor, isDark)
    : []
}

function renderForegroundFrame(
  ctx: CanvasRenderingContext2D,
  cw: number, ch: number,
  prep: PreparedMap,
  map: TrackMapData,
  cars: CarPosition[] | null,
  participants: ParticipantsMsg | null,
  isDark: boolean,
  driversMode: 'dots' | 'both' | 'labels',
  layout: { scale: number; ox: number; oy: number },
): void {
  ctx.clearRect(0, 0, cw, ch)
  if (!cars) return
  const { scale, ox, oy } = layout
  drawCarDots(ctx, cars, participants, map, prep, scale, ox, oy, driversMode, isDark)
}

function renderControlledMarkers(
  ctx: CanvasRenderingContext2D,
  cw: number, ch: number,
  prep: PreparedMap,
  map: TrackMapData,
  markers: readonly TrackMapMarker[],
  isDark: boolean,
  layout: { scale: number; ox: number; oy: number },
): void {
  ctx.clearRect(0, 0, cw, ch)
  const { scale, ox, oy } = layout
  for (const marker of markers) {
    if (!Number.isFinite(marker.x) || !Number.isFinite(marker.z)) continue
    const [vx, vy] = rawToViewBox(marker.x, marker.z, map.transform)
    const [rx, ry] = rotatePoint(vx, vy, prep.rotCos, prep.rotSin, prep.rotCx, prep.rotCy)
    const [x, y] = toCanvas([rx, ry], scale, ox, oy)
    ctx.beginPath()
    ctx.arc(x, y, DOT_R, 0, Math.PI * 2)
    ctx.fillStyle = marker.color
    ctx.fill()
  }
  for (const marker of markers) {
    if (!Number.isFinite(marker.x) || !Number.isFinite(marker.z)) continue
    const [vx, vy] = rawToViewBox(marker.x, marker.z, map.transform)
    const [rx, ry] = rotatePoint(vx, vy, prep.rotCos, prep.rotSin, prep.rotCx, prep.rotCy)
    const [x, y] = toCanvas([rx, ry], scale, ox, oy)
    drawLabel(ctx, x, y, marker.label, marker.color, false, isDark)
  }
}

// ── Module-level position cache (survives component remounts) ─────────────────

let _cachedCars:      CarPosition[] | null = null
let _cachedPlayerIdx: number               = 0


interface FollowDriverSelectorProps {
  selectedDriverIdx: number | null
  onChange: (driverIndex: number | null) => void
  options: DriverOption[]
  isDark: boolean
}

const FollowDriverSelector = memo(({ selectedDriverIdx, onChange, options, isDark }: FollowDriverSelectorProps) => (
  <div className="w-40 shrink-0">
    <DriverSelect
      options={options}
      selectedDriverIdx={selectedDriverIdx}
      onChange={onChange}
      clearable
      placeholder="Follow driver…"
      isDark={isDark}
      solidBg
      menuAlign="right"
    />
  </div>
))
FollowDriverSelector.displayName = 'FollowDriverSelector'

interface ZoomSelectorProps {
  zoomLevel: number
  onChange: (opt: SingleValue<ZoomOption>) => void
  isDark: boolean
}

const ZoomSelector = memo(({ zoomLevel, onChange, isDark }: ZoomSelectorProps) => {
  const styles = useMemo(() => buildSelectStyles(isDark, { solidBg: true }), [isDark])
  const val = useMemo(() => ZOOM_OPTIONS.find(o => o.value === zoomLevel) ?? ZOOM_OPTIONS[1], [zoomLevel])
  
  return (
    <div className="w-20 shrink-0">
      <Select<ZoomOption>
        value={val}
        options={ZOOM_OPTIONS}
        onChange={onChange}
        isSearchable={false}
        styles={styles}
        components={selectComponents}
      />
    </div>
  )
})
ZoomSelector.displayName = 'ZoomSelector'

type ModdedMapOption = { value: string; label: string }

const SOURCE_LABELS: Record<string, string> = { generated: 'Generated', imported: 'Imported' }

interface ModdedMapPickerProps {
  resolution: TrackMapResolution
  onPinned?: () => void
  isDark: boolean
}

// With Modded Maps on: any saved or built-in map, kept for this track.
const ModdedMapPicker = memo(({ resolution, onPinned, isDark }: ModdedMapPickerProps) => {
  const catalog = useTrackMapStore(state => state.catalog)
  const styles = useMemo(() => buildSelectStyles(isDark, { solidBg: true }), [isDark])
  const options = useMemo<ModdedMapOption[]>(() => [
    { value: '', label: 'Auto' },
    ...catalog.maps.map(entry => ({
      value: entry.id,
      label: SOURCE_LABELS[entry.source] ? `${entry.name} · ${SOURCE_LABELS[entry.source]}` : entry.name,
    })),
  ], [catalog.maps])
  const pinned = catalog.pins[resolution.track_key] ?? ''
  const value = options.find(option => option.value === pinned) ?? options[0]
  const handleChange = useCallback((option: SingleValue<ModdedMapOption>) => {
    void window.trackMapsBridge.pin(resolution.track_key, option?.value ?? '').then(() => onPinned?.())
  }, [onPinned, resolution.track_key])
  return (
    <div className="w-44 shrink-0">
      <Select<ModdedMapOption>
        aria-label="Map"
        value={value}
        options={options}
        onChange={handleChange}
        isSearchable
        styles={styles}
        components={selectComponents}
      />
    </div>
  )
})
ModdedMapPicker.displayName = 'ModdedMapPicker'

interface TestMapControlsProps {
  alignRight: boolean
  trackId: number | null
  aeroOverlay: AeroOverlay
  mapOptions: MapOption[]
  onTrackChange: (opt: SingleValue<MapOption>) => void
  onAeroChange: (opt: SingleValue<AeroOption>) => void
  isDark: boolean
}

const TestMapControls = memo(({ alignRight, trackId, aeroOverlay, mapOptions, onTrackChange, onAeroChange, isDark }: TestMapControlsProps) => {
  const styles = useMemo(() => buildSelectStyles(isDark, { solidBg: true }), [isDark])
  const mapValue = useMemo(() => mapOptions.find(o => o.value === trackId) ?? null, [mapOptions, trackId])
  const aeroValue = useMemo(() => AERO_OPTIONS.find(o => o.value === aeroOverlay) ?? AERO_OPTIONS[2], [aeroOverlay])

  return (
    <div className={`absolute top-2 z-10 flex flex-col gap-1.5 ${alignRight ? 'right-2 items-end' : 'left-2 items-start'}`} style={{ WebkitAppRegion: 'no-drag' } as React.CSSProperties}>
      <div className="w-56 shrink-0">
        <Select<MapOption>
          aria-label="Test map"
          value={mapValue}
          options={mapOptions}
          onChange={onTrackChange}
          isSearchable
          placeholder="Select map…"
          styles={styles}
          components={selectComponents}
        />
      </div>
      <div className="w-32 shrink-0">
        <Select<AeroOption>
          aria-label="Test overtaking aid"
          value={aeroValue}
          options={AERO_OPTIONS}
          onChange={onAeroChange}
          isSearchable={false}
          styles={styles}
          components={selectComponents}
        />
      </div>
    </div>
  )
})
TestMapControls.displayName = 'TestMapControls'

// ── Component ─────────────────────────────────────────────────────────────────

interface Props {
  trackId:             number | null
  participants:        ParticipantsMsg | null
  isDark:              boolean
  sectorColors?:       boolean
  driversMode?:        'dots' | 'both' | 'labels'
  mapTimeout?:         number
  isFullscreen?:       boolean
  onToggleFullscreen?: () => void
  reduceAnimations?:   boolean
  mapDimmed?:          boolean
  aeroMode?:           'drs' | 'slm'
  slmTrackStatus?:     number   // 2026 SLM track status: 0 = Full, 1 = Partial
  /** Supplying a marker source puts the map in controlled mode and disables
   *  its live telemetry subscription. The source is read inside the map rAF. */
  markerSource?:       () => readonly TrackMapMarker[]
  /** Colours the track by lap position in place of the plain/sector colours. */
  trackPaint?:         TrackPaint | null
  /** Ticks drawn across the track, such as braking points. */
  trackMarks?:         readonly TrackMark[] | null
  /** The engine's map choice for this track (Modded Maps). */
  mapResolution?:      TrackMapResolution | null
  /** Called after the user picks a map, for a choice the engine does not
   *  resend by itself (a recording open in Analysis). */
  onMapPinned?:        () => void
  /** Draws this map instead of the track's (Edit Map's preview). */
  mapOverride?:        TrackMapData | null
}

export default function TrackMap({ trackId, participants, isDark, sectorColors = false, driversMode = 'both', mapTimeout = 10, isFullscreen = false, onToggleFullscreen, reduceAnimations = false, mapDimmed = false, aeroMode = 'drs', slmTrackStatus = -1, markerSource, trackPaint = null, trackMarks = null, mapResolution = null, onMapPinned, mapOverride = null }: Props) {
  const { ref: wrapRef, width, height } = useSize()
  const { raw: labels } = useLabels()
  const backgroundCanvasRef = useRef<HTMLCanvasElement>(null)
  const foregroundCanvasRef = useRef<HTMLCanvasElement>(null)
  const backgroundDirtyRef = useRef(true)
  const backgroundLayoutRef = useRef<{ scale: number; ox: number; oy: number } | null>(null)
  const prepRef         = useRef<PreparedMap | null>(null)
  const carsRef         = useRef<CarPosition[] | null>(_cachedCars)
  const playerIdxRef    = useRef<number>(_cachedPlayerIdx)
  const participantsRef = useRef<ParticipantsMsg | null>(null)
  const rafRef          = useRef<number>(0)
  const isDarkRef         = useRef<boolean>(isDark)
  const sectorColorsRef   = useRef<boolean>(sectorColors)
  const driversModeRef    = useRef<'dots' | 'both' | 'labels'>(driversMode)
  const mapDimmedRef      = useRef<boolean>(mapDimmed)
  const aeroModeRef       = useRef<'drs' | 'slm'>(aeroMode)
  const slmTrackStatusRef = useRef<number>(slmTrackStatus)
  const markerSourceRef = useRef(markerSource)
  const trackPaintRef = useRef<TrackPaint | null>(trackPaint)
  const trackMarksRef = useRef<readonly TrackMark[] | null>(trackMarks)
  // Where the background last drew each mark, and the one under the pointer.
  const markPositionsRef = useRef<Array<Array<[number, number]>>>([])
  const [hoveredMark, setHoveredMark] = useState<{ index: number; x: number; y: number } | null>(null)
  const handleMarkHover = useCallback((event: React.MouseEvent<HTMLDivElement>) => {
    const marks = trackMarksRef.current
    if (!marks?.some(mark => mark.tooltip)) return
    const rect = event.currentTarget.getBoundingClientRect()
    const px = event.clientX - rect.left
    const py = event.clientY - rect.top
    // The nearest highlighted stretch, by its closest point to the pointer.
    let best = -1
    let bestDistance = MARK_HOVER_PX
    let bestPoint: [number, number] = [px, py]
    markPositionsRef.current.forEach((path, index) => {
      if (!marks[index]?.tooltip) return
      for (const [x, y] of path) {
        const distance = Math.hypot(x - px, y - py)
        if (distance <= bestDistance) { best = index; bestDistance = distance; bestPoint = [x, y] }
      }
    })
    setHoveredMark(previous => {
      if (best < 0) return previous === null ? previous : null
      return previous?.index === best ? previous : { index: best, x: bestPoint[0], y: bestPoint[1] }
    })
  }, [])
  // A new set of marks invalidates what was hovered.
  const [hoverMarks, setHoverMarks] = useState(trackMarks)
  if (hoverMarks !== trackMarks) {
    setHoverMarks(trackMarks)
    setHoveredMark(null)
  }
  const reduceAnimationsRef = useRef(reduceAnimations)
  const controlledMarkers = markerSource !== undefined

  const [selectedDriverIdx, setSelectedDriverIdx] = useState<number | null>(null)
  const selectedDriverIdxRef = useRef<number | null>(null)
  const camRef = useRef<{ scale: number; ox: number; oy: number } | null>(null)

  const [zoomLevel, setZoomLevel] = useState<number>(4)
  const zoomLevelRef = useRef<number>(4)

  const [previewTrackId, setPreviewTrackId] = useState<number | null>(trackId)
  const [previewAeroOverlay, setPreviewAeroOverlay] = useState<AeroOverlay>(() => aeroOverlayFromTelemetry(aeroMode, slmTrackStatus))
  const mapOptions = useMemo(() => BASE_MAP_OPTIONS.map(option => ({
    ...option,
    label: labels[`track.${option.value}.track_name`] ?? option.label,
  })).sort((a, b) => a.label.localeCompare(b.label)), [labels])
  const resolvedMap = useResolvedTrackMap(trackId, mapResolution)
  const moddedMaps = useTrackMapStore(state => state.settings.modded)
  const pickerResolution = moddedMaps && mapResolution && mapResolution.track_id === trackId ? mapResolution : null
  // The debug test controls preview another track's bundled map.
  const map = mapOverride ?? (previewTrackId !== trackId
    ? (previewTrackId != null ? TRACK_MAPS[previewTrackId] ?? null : null)
    : resolvedMap)
  // With Modded Maps on, the map in use can be renamed and rotated.
  const editableMapId = pickerResolution
    ? pickerResolution.map_id || (trackId != null && TRACK_MAPS[trackId] ? `builtin:${trackId}` : '')
    : ''
  const [editingMapId, setEditingMapId] = useState<string | null>(null)
  const closeMapEditor = useCallback(() => setEditingMapId(null), [])
  const effectiveAeroMode = previewAeroOverlay === 'drs' ? 'drs' : 'slm'
  const effectiveSlmTrackStatus = previewAeroOverlay === 'slm-wet' ? 1 : 0

  // A new session track or aero state replaces whatever the test controls previewed.
  const [previewSource, setPreviewSource] = useState({ trackId, aeroMode, slmTrackStatus })
  if (previewSource.trackId !== trackId || previewSource.aeroMode !== aeroMode ||
      previewSource.slmTrackStatus !== slmTrackStatus) {
    if (previewSource.trackId !== trackId) setPreviewTrackId(trackId)
    if (previewSource.aeroMode !== aeroMode || previewSource.slmTrackStatus !== slmTrackStatus)
      setPreviewAeroOverlay(aeroOverlayFromTelemetry(aeroMode, slmTrackStatus))
    setPreviewSource({ trackId, aeroMode, slmTrackStatus })
  }

  const mapTimeoutRef = useRef<number>(mapTimeout)
  // The draw loop and position subscription read these each frame. Declared
  // ahead of every other effect so they see this render's values.
  useLayoutEffect(() => {
    markerSourceRef.current     = markerSource
    selectedDriverIdxRef.current = selectedDriverIdx
    zoomLevelRef.current        = zoomLevel
    participantsRef.current     = participants
    isDarkRef.current           = isDark
    sectorColorsRef.current     = sectorColors
    driversModeRef.current      = driversMode
    mapDimmedRef.current        = mapDimmed
    aeroModeRef.current         = effectiveAeroMode
    slmTrackStatusRef.current   = effectiveSlmTrackStatus
    mapTimeoutRef.current       = mapTimeout
    reduceAnimationsRef.current = reduceAnimations
    trackPaintRef.current       = trackPaint
    trackMarksRef.current       = trackMarks
  })

  // Precompute rotated geometry whenever the map changes
  useEffect(() => {
    prepRef.current = map ? prepareMap(map) : null
    backgroundDirtyRef.current = true
  }, [map])

  const lastPosRef = useRef<Record<number, { x: number; z: number; lastMovedTime: number }>>({})

  // Subscribe to position updates directly — bypasses React state to avoid 60 Hz re-renders
  useEffect(() => {
    if (controlledMarkers) return
    const handleMsg = (raw: unknown) => {
      const msg = raw as Partial<PositionsMsg> | null
      if (msg?.type === 'positions' && msg.cars) {
        const now = Date.now()
        const timeoutMs = mapTimeoutRef.current * 1000
        const playerIdx = msg.player_idx ?? 0

        const patchedCars = new Map((_cachedCars ?? []).map(car => [car.idx, car]))
        for (const car of msg.cars) {
          if (car.idx === playerIdx) {
            patchedCars.set(car.idx, car)
            continue
          }

          const prev = lastPosRef.current[car.idx]
          if (!prev) {
            lastPosRef.current[car.idx] = { x: car.x, z: car.z, lastMovedTime: now }
            patchedCars.set(car.idx, car)
            continue
          }

          const hasMoved = car.x !== prev.x || car.z !== prev.z
          if (hasMoved) {
            lastPosRef.current[car.idx] = { x: car.x, z: car.z, lastMovedTime: now }
            patchedCars.set(car.idx, car)
          } else {
            const duration = now - prev.lastMovedTime
            if (mapTimeoutRef.current > 0 && duration > timeoutMs) {
              patchedCars.set(car.idx, { ...car, x: 0, z: 0 })
            } else {
              patchedCars.set(car.idx, car)
            }
          }
        }

        _cachedCars      = [...patchedCars.values()]
        _cachedPlayerIdx = playerIdx
        carsRef.current      = _cachedCars
        playerIdxRef.current = _cachedPlayerIdx
      }
    }

    const unsubBatch = window.telemetryBridge.onBatch((batchStr: string) => {
      let start = 0
      while (start < batchStr.length) {
        let end = batchStr.indexOf('\n', start)
        if (end === -1) end = batchStr.length
        if (end > start) {
          const raw = JSON.parse(batchStr.slice(start, end))
          if (raw.type === 'positions') handleMsg(raw)
        }
        start = end + 1
      }
    })

    const unsubOn = window.telemetryBridge.on((raw) => {
      handleMsg(raw)
    })

    // Live positions arrive packed in the binary hot-row batch (playback still
    // delivers them as JSON via onBatch above).
    const unsubBinary = window.telemetryBridge.onBinary((batch) => {
      try {
        const rows = decodeBinaryBatch(batch)
        let lastPositions: PositionsMsg | null = null
        for (const row of rows) {
          if (row.type === 'positions') lastPositions = row
        }
        if (lastPositions) handleMsg(lastPositions)
      } catch (e) {
        console.error('TrackMap: failed to decode binary batch:', e)
      }
    })

    return () => {
      unsubBatch()
      unsubOn()
      unsubBinary()
    }
  }, [controlledMarkers])


  // Track geometry lives on a separate Canvas 2D layer. In overview mode it is
  // redrawn only when its inputs change; the foreground cars and labels remain
  // on the display-rate rAF path. Following a driver invalidates the background
  // as the camera moves.
  useEffect(() => {
    backgroundDirtyRef.current = true
  }, [isDark, sectorColors, mapDimmed, effectiveAeroMode, effectiveSlmTrackStatus, trackPaint, trackMarks])

  useEffect(() => {
    if (map) return
    for (const canvas of [backgroundCanvasRef.current, foregroundCanvasRef.current]) {
      const ctx = canvas?.getContext('2d')
      if (canvas && ctx) {
        ctx.setTransform(1, 0, 0, 1, 0, 0)
        ctx.clearRect(0, 0, canvas.width, canvas.height)
      }
    }
    backgroundLayoutRef.current = null
  }, [map, width, height])

  // Resize both Canvas 2D layers when the container changes. Assigning the
  // backing dimensions clears their contexts, so the background must redraw.
  useEffect(() => {
    const backgroundCanvas = backgroundCanvasRef.current
    const foregroundCanvas = foregroundCanvasRef.current
    if (!backgroundCanvas || !foregroundCanvas || width === 0 || height === 0) return
    const dpr = window.devicePixelRatio || 1
    const pixelWidth = Math.round(width * dpr)
    const pixelHeight = Math.round(height * dpr)
    for (const canvas of [backgroundCanvas, foregroundCanvas]) {
      canvas.width = pixelWidth
      canvas.height = pixelHeight
      canvas.style.width = `${width}px`
      canvas.style.height = `${height}px`
    }
    backgroundDirtyRef.current = true
    backgroundLayoutRef.current = null
  }, [width, height])

  // rAF render loop
  useEffect(() => {
    if (!map || width === 0 || height === 0) return
    let active = true

    const loop = () => {
      if (!active) return
      const backgroundCanvas = backgroundCanvasRef.current
      const foregroundCanvas = foregroundCanvasRef.current
      const prep   = prepRef.current
      if (backgroundCanvas && foregroundCanvas && prep) {
        const dpr = window.devicePixelRatio || 1
        const backgroundCtx = backgroundCanvas.getContext('2d')
        const foregroundCtx = foregroundCanvas.getContext('2d')
        if (backgroundCtx && foregroundCtx) {
          backgroundCtx.setTransform(dpr, 0, 0, dpr, 0, 0)
          foregroundCtx.setTransform(dpr, 0, 0, dpr, 0, 0)
          foregroundCtx.imageSmoothingEnabled = false

          const baseLayout = buildLayout(prep, width, height)
          let layout = baseLayout

          const followIdx = selectedDriverIdxRef.current
          const cars = carsRef.current
          if (followIdx !== null && cars) {
            const car = cars.find(c => c.idx === followIdx)
            if (car && (car.x !== 0 || car.z !== 0)) {
              const [vx, vy] = rawToViewBox(car.x, car.z, map.transform)
              const [rx, ry] = rotatePoint(vx, vy, prep.rotCos, prep.rotSin, prep.rotCx, prep.rotCy)
              const followScale = baseLayout.scale * zoomLevelRef.current
              if (!camRef.current) {
                camRef.current = { scale: baseLayout.scale, ox: 0, oy: 0 }
              }
              if (reduceAnimationsRef.current) {
                camRef.current.scale = followScale
              } else {
                camRef.current.scale += (followScale - camRef.current.scale) * 0.12
              }
              // Snap pan so the driver is always exactly centered — no lag on the dot
              camRef.current.ox = width  / 2 - rx * camRef.current.scale
              camRef.current.oy = height / 2 - ry * camRef.current.scale
              layout = camRef.current
            }
          } else if (camRef.current) {
            if (reduceAnimationsRef.current) {
              camRef.current = null
            } else {
              camRef.current = {
                scale: camRef.current.scale + (baseLayout.scale - camRef.current.scale) * 0.12,
                ox:    camRef.current.ox    + (baseLayout.ox    - camRef.current.ox)    * 0.12,
                oy:    camRef.current.oy    + (baseLayout.oy    - camRef.current.oy)    * 0.12,
              }
              const diff = Math.abs(camRef.current.scale - baseLayout.scale)
                         + Math.abs(camRef.current.ox    - baseLayout.ox)
                         + Math.abs(camRef.current.oy    - baseLayout.oy)
              if (diff < 0.5) camRef.current = null
            }
            layout = camRef.current ?? baseLayout
          }

          const effectiveZoom = layout.scale / baseLayout.scale
          const previousLayout = backgroundLayoutRef.current
          const layoutChanged = !previousLayout
            || previousLayout.scale !== layout.scale
            || previousLayout.ox !== layout.ox
            || previousLayout.oy !== layout.oy
          if (backgroundDirtyRef.current || layoutChanged) {
            markPositionsRef.current = renderBackgroundFrame(backgroundCtx, width, height, prep, isDarkRef.current, sectorColorsRef.current, layout, effectiveZoom, mapDimmedRef.current, aeroModeRef.current, slmTrackStatusRef.current, trackPaintRef.current, trackMarksRef.current)
            backgroundLayoutRef.current = { ...layout }
            backgroundDirtyRef.current = false
          }
          const source = markerSourceRef.current
          if (source) {
            renderControlledMarkers(foregroundCtx, width, height, prep, map, source(), isDarkRef.current, layout)
          } else {
            renderForegroundFrame(foregroundCtx, width, height, prep, map, cars, participantsRef.current, isDarkRef.current, driversModeRef.current, layout)
          }
        }
      }
      rafRef.current = requestAnimationFrame(loop)
    }

    rafRef.current = requestAnimationFrame(loop)
    return () => {
      active = false
      cancelAnimationFrame(rafRef.current)
    }
  }, [map, width, height])

  const driverOptions = useMemo(() => buildDriverOptions(participants?.drivers ?? []), [participants])

  const handleDriverChange = useCallback((driverIndex: number | null) => {
    setSelectedDriverIdx(driverIndex)
  }, [])

  const handleZoomChange = useCallback((opt: SingleValue<ZoomOption>) => {
    setZoomLevel(opt?.value ?? 4)
  }, [])

  const handleTrackChange = useCallback((opt: SingleValue<MapOption>) => {
    setPreviewTrackId(opt?.value ?? null)
  }, [])

  const handleAeroChange = useCallback((opt: SingleValue<AeroOption>) => {
    setPreviewAeroOverlay(opt?.value ?? 'drs')
  }, [])

  return (
    <div ref={wrapRef} className="relative w-full h-full"
      onMouseMove={handleMarkHover} onMouseLeave={() => setHoveredMark(null)}>
      {hoveredMark && trackMarks?.[hoveredMark.index]?.tooltip && <div
        className="pointer-events-none absolute z-20 w-max rounded-lg border border-[var(--border)] bg-[var(--bg-panel)] px-3 py-2.5 text-[10px] font-mono text-[var(--text-primary)] shadow-[0_6px_20px_rgba(0,0,0,0.4)]"
        style={{
          left: hoveredMark.x > width - 310 ? undefined : hoveredMark.x + 12,
          right: hoveredMark.x > width - 310 ? width - hoveredMark.x + 12 : undefined,
          top: Math.min(hoveredMark.y + 12, Math.max(0, height - 400)),
        }}
      >{trackMarks[hoveredMark.index].tooltip}</div>}
      <canvas ref={backgroundCanvasRef} className="absolute inset-0" />
      <canvas ref={foregroundCanvasRef} className="absolute inset-0 pointer-events-none" />
      {import.meta.env.MODE === 'debug' && (
        <TestMapControls
          alignRight={controlledMarkers}
          trackId={previewTrackId}
          aeroOverlay={previewAeroOverlay}
          mapOptions={mapOptions}
          onTrackChange={handleTrackChange}
          onAeroChange={handleAeroChange}
          isDark={isDark}
        />
      )}
      {!map && (
        <div className="absolute inset-0 flex items-center justify-center">
          <span className="text-[11px] uppercase tracking-widest text-[var(--text-secondary)]">
            No map data
          </span>
        </div>
      )}
      {pickerResolution && (pickerResolution.generating || pickerResolution.mismatch) && (
        <div className="pointer-events-none absolute bottom-2 left-2 z-10 rounded-md bg-[var(--bg-panel)] px-2 py-1 text-[10px] font-mono uppercase tracking-wider text-[var(--text-secondary)]">
          {pickerResolution.generating ? 'Building map…' : 'Map doesn\'t fit'}
        </div>
      )}
      {pickerResolution && (
        <div className="absolute top-2 left-2 z-10 flex items-center gap-1.5" style={{ WebkitAppRegion: 'no-drag' } as React.CSSProperties}>
          <ModdedMapPicker resolution={pickerResolution} onPinned={onMapPinned} isDark={isDark} />
          {editableMapId && map && (
            <button
              onClick={() => setEditingMapId(editableMapId)}
              className="h-7 w-7 rounded-[6px] bg-[var(--bg-panel)] text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-all flex items-center justify-center shrink-0"
              title="Edit Map"
            >
              <Pencil size={13} />
            </button>
          )}
        </div>
      )}
      {map && (driverOptions.length > 0 || onToggleFullscreen) && (
        <div className="absolute top-2 right-2 z-10 flex items-center gap-1.5" style={{ WebkitAppRegion: 'no-drag' } as React.CSSProperties}>
          {selectedDriverIdx !== null && (
            <ZoomSelector
              zoomLevel={zoomLevel}
              onChange={handleZoomChange}
              isDark={isDark}
            />
          )}
          {driverOptions.length > 0 && (
            <FollowDriverSelector
              selectedDriverIdx={selectedDriverIdx}
              onChange={handleDriverChange}
              options={driverOptions}
              isDark={isDark}
            />
          )}
          {onToggleFullscreen && (
            <button
              onClick={onToggleFullscreen}
              className="h-7 w-7 rounded-[6px] bg-[var(--bg-panel)] text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-all flex items-center justify-center shrink-0"
              title={isFullscreen ? 'Exit Fullscreen' : 'Fullscreen'}
            >
              {isFullscreen ? <Minimize2 size={13} /> : <Maximize2 size={13} />}
            </button>
          )}
        </div>
      )}
      {editableMapId && (
        <EditMapModal mapId={editingMapId} isDark={isDark} onClose={closeMapEditor} onSaved={onMapPinned} />
      )}
    </div>
  )
}
