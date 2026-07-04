import { useRef, useEffect, useState, useMemo, useCallback, memo } from 'react'
import Select, { type SingleValue } from 'react-select'
import { buildSelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import { Maximize2, Minimize2 } from 'lucide-react'
import { useSize } from '../hooks/useSize'
import { TRACK_MAPS, type TrackMapData } from '../lib/trackMaps'
import { decodeBinaryBatch } from '../lib/decodeBinaryBatch'
import type { CarPosition, ParticipantsMsg } from '../types'

type DriverOption = { value: number; label: string }
type ZoomOption = { value: number; label: string }

const ZOOM_OPTIONS: ZoomOption[] = [
  { value: 2, label: '2x' },
  { value: 4, label: '4x' },
  { value: 8, label: '8x' },
  { value: 16, label: '16x' },
]


const SECTOR_COLORS_DARK  = ['#E8002D', '#0090D0', '#FFD700']
const SECTOR_COLORS_LIGHT = ['#D32F2F', '#0D47A1', '#B7950B']
const SECTOR_COLORS       = SECTOR_COLORS_DARK
const DRS_COLOR     = '#39B54A'
const SLM_DRY_COLOR = '#FF9500'   // SLM Normal grip  (slm_dry / Full status)   — orange
const SLM_WET_COLOR = '#22D3EE'   // SLM Reduced grip (slm_wet / Partial status) — cyan
const TRAP_COLOR    = '#C77DFF'
const SF_COLOR      = '#E8002D'

const TRACK_PX   = 5
const DRS_PX     = 6
const DRS_OFFSET = 18
const TRAP_R     = 9
const SF_HALF    = 14
const JUNC_HALF  = 10
const DOT_R      = 7
const MAP_PAD    = 24  // CSS-pixel padding around the tight track bounds
const LABEL_W    = 38
const LABEL_H    = 16
const LABEL_GAP  = 5
const LABEL_R    = 3   // corner radius
const ACCENT_W   = 3   // left livery-color bar width

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

interface PreparedMap {
  sectors:     Array<{ index: number; points: [number, number][] }>
  drsZones:    Array<{ track_points: [number, number][] }>
  slmDry:      Array<{ track_points: [number, number][] }>   // 2026 SLM — Full-status zones
  slmWet:      Array<{ track_points: [number, number][] }>   // 2026 SLM — Partial-status zones
  speedTraps:  [number, number][]
  startFinish: [number, number] | null
  s1pts:       [number, number][]
  sfIdx:       number
  junctions:   SectorJunction[]
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
  const reconstruct = (zones: Array<{ start: [number, number]; end: [number, number] }>) =>
    zones.map(z => ({ track_points: smoothSeam(sliceZone(centerline, z.start, z.end)).map(rot) }))

  const drsZones = reconstruct(map.drs_zones as Array<{ start: [number, number]; end: [number, number] }>)
  const slmDry   = reconstruct(map.slm_dry ?? [])
  const slmWet   = reconstruct(map.slm_wet ?? [])
  const speedTraps = (map.speed_traps as [number, number][]).map(rot)
  const startFinish = map.start_finish ? rot(map.start_finish as [number, number]) : null

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

  return {
    sectors, drsZones, slmDry, slmWet, speedTraps, startFinish, s1pts, sfIdx, junctions,
    bounds: { minX, minY, w: maxX - minX, h: maxY - minY },
    rotCos: cos, rotSin: sin, rotCx: cx, rotCy: cy,
  }
}

/** Compute scale + offset so the tight track bounds fill the canvas with padding. */
function buildLayout(prep: PreparedMap, cw: number, ch: number) {
  const { minX, minY, w, h } = prep.bounds
  const scale = Math.min((cw - 2 * MAP_PAD) / w, (ch - 2 * MAP_PAD) / h)
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

/** A line running alongside the track, offset perpendicular toward the *outside*
 *  of the circuit from each point (the inward normal points at the enclosed
 *  interior for a consistently-wound loop, so we negate it). Used for both the
 *  DRS zone (green, dashed) and the SLM overlay (purple; solid where wet applies,
 *  dashed where dry-only). */
function drawOffsetLine(
  ctx: CanvasRenderingContext2D,
  pts: [number, number][],
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
    const px = cx2 - nx * offset
    const py = cy2 - ny * offset
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
  zoomFactor: number = 1,
  trackZoomFactor: number = 1,
) {
  const [cx2, cy2] = toCanvas(pt, scale, ox, oy)
  ctx.strokeStyle = TRAP_COLOR
  ctx.lineWidth   = 2 * zoomFactor
  const trapR = Math.max(TRAP_R * zoomFactor, (TRACK_PX * trackZoomFactor) / 2 + 4 * zoomFactor)
  for (const r of [trapR, trapR + 5 * zoomFactor, trapR + 10 * zoomFactor]) {
    ctx.beginPath()
    ctx.arc(cx2, cy2, r, 0, Math.PI * 2)
    ctx.stroke()
  }
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

function drawLabel(
  ctx: CanvasRenderingContext2D,
  cx: number, cy: number,
  text: string,
  color: string,
  centerOnDot?: boolean,
  isDark: boolean = true,
): void {
  ctx.save()
  const bx = cx - LABEL_W / 2
  const by = centerOnDot ? cy - LABEL_H / 2 : cy - DOT_R - LABEL_GAP - LABEL_H

  // Drop shadow
  ctx.shadowColor   = isDark ? 'rgba(0,0,0,0.55)' : 'rgba(0,0,0,0.15)'
  ctx.shadowBlur    = 6
  ctx.shadowOffsetY = 2

  // Background
  ctx.beginPath()
  ctx.roundRect(bx, by, LABEL_W, LABEL_H, LABEL_R)
  ctx.fillStyle = isDark ? 'rgba(10,15,30,0.92)' : 'rgba(255,255,255,0.96)'
  ctx.fill()

  if (!isDark) {
    ctx.strokeStyle = 'rgba(0,0,0,0.15)'
    ctx.lineWidth   = 0.5
    ctx.stroke()
  }

  ctx.shadowColor = 'transparent'
  ctx.shadowBlur  = 0
  ctx.shadowOffsetY = 0

  // Left accent bar
  ctx.beginPath()
  ctx.roundRect(bx, by, ACCENT_W, LABEL_H, [LABEL_R, 0, 0, LABEL_R])
  ctx.fillStyle = color
  ctx.fill()

  // Text
  ctx.font         = 'bold 9px "Cascadia Code", ui-monospace, monospace'
  ctx.textAlign    = 'center'
  ctx.textBaseline = 'middle'
  ctx.fillStyle    = isDark ? '#ffffff' : '#111827'
  ctx.fillText(text, cx + ACCENT_W / 2, by + LABEL_H / 2)

  ctx.restore()
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

// ── Main render ───────────────────────────────────────────────────────────────

function renderFrame(
  ctx: CanvasRenderingContext2D,
  cw: number, ch: number,
  prep: PreparedMap,
  map: TrackMapData,
  cars: CarPosition[] | null,
  participants: ParticipantsMsg | null,
  isDark: boolean,
  sectorColors: boolean,
  driversMode: 'dots' | 'both' | 'labels',
  layout: { scale: number; ox: number; oy: number },
  effectiveZoom: number = 1,
  mapDimmed: boolean = false,
  aeroMode: 'drs' | 'slm' = 'drs',
  slmTrackStatus: number = -1,   // 0 = Full, 1 = Partial, -1 = unknown
) {
  ctx.clearRect(0, 0, cw, ch)
  const { scale, ox, oy } = layout
  const zoomFactor = Math.sqrt(effectiveZoom)
  // Stronger scaling factor specifically for track width so cars don't float off-track
  const trackZoomFactor = Math.pow(effectiveZoom, 0.8)

  const trackColor = isDark ? '#ffffff' : '#000000'
  const colors = isDark ? SECTOR_COLORS_DARK : SECTOR_COLORS_LIGHT

  if (mapDimmed) ctx.globalAlpha = 0.4

  for (const sector of prep.sectors) {
    const color = sectorColors ? (colors[sector.index - 1] ?? trackColor) : trackColor
    drawPolyline(ctx, sector.points, scale, ox, oy, color, TRACK_PX * trackZoomFactor)
  }

  if (mapDimmed) ctx.globalAlpha = 1.0

  for (let idx = 0; idx < prep.junctions.length; idx++) {
    const j = prep.junctions[idx]
    const color = sectorColors ? trackColor : (colors[idx + 1] ?? trackColor)
    drawSectorJunction(ctx, j, scale, ox, oy, color, zoomFactor, trackZoomFactor)
  }

  // Overtaking-aid overlay: DRS zones (F1 24/25) or the SLM overlay (F1 26).
  // In SLM, a Partial track status (1) draws the wet zone set, otherwise (Full
  // or unknown) the dry set. Both are dashed, like DRS.
  if (aeroMode === 'slm') {
    const partial = slmTrackStatus === 1
    const zones = partial ? prep.slmWet : prep.slmDry
    const color = partial ? SLM_WET_COLOR : SLM_DRY_COLOR
    for (const zone of zones) {
      drawOffsetLine(ctx, zone.track_points, scale, ox, oy, color, true, zoomFactor, trackZoomFactor)
    }
  } else {
    for (const zone of prep.drsZones) {
      drawOffsetLine(ctx, zone.track_points, scale, ox, oy, DRS_COLOR, true, zoomFactor, trackZoomFactor)
    }
  }

  for (const trap of prep.speedTraps) {
    drawSpeedTrap(ctx, trap, scale, ox, oy, zoomFactor, trackZoomFactor)
  }

  if (prep.startFinish) {
    drawStartFinish(ctx, prep.startFinish, prep.s1pts, prep.sfIdx, scale, ox, oy, zoomFactor, trackZoomFactor, sectorColors ? trackColor : undefined)
  }

  if (cars) {
    drawCarDots(ctx, cars, participants, map, prep, scale, ox, oy, driversMode, isDark)
  }
}

// ── Module-level position cache (survives component remounts) ─────────────────

let _cachedCars:      CarPosition[] | null = null
let _cachedPlayerIdx: number               = 0


interface FollowDriverSelectorProps {
  selectedDriverIdx: number | null
  onChange: (opt: SingleValue<DriverOption>) => void
  options: DriverOption[]
  isDark: boolean
}

const FollowDriverSelector = memo(({ selectedDriverIdx, onChange, options, isDark }: FollowDriverSelectorProps) => {
  const styles = useMemo(() => buildSelectStyles(isDark, { solidBg: true }), [isDark])
  const val = useMemo(() => options.find(o => o.value === selectedDriverIdx) ?? null, [options, selectedDriverIdx])
  
  return (
    <div className="w-40 shrink-0">
      <Select<DriverOption>
        value={val}
        options={options}
        onChange={onChange}
        isClearable
        isSearchable
        placeholder="Follow driver…"
        styles={styles}
        components={selectComponents}
      />
    </div>
  )
})
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
}

export default function TrackMap({ trackId, participants, isDark, sectorColors = false, driversMode = 'both', mapTimeout = 10, isFullscreen = false, onToggleFullscreen, reduceAnimations = false, mapDimmed = false, aeroMode = 'drs', slmTrackStatus = -1 }: Props) {
  const { ref: wrapRef, width, height } = useSize()
  const canvasRef       = useRef<HTMLCanvasElement>(null)
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

  const [selectedDriverIdx, setSelectedDriverIdx] = useState<number | null>(null)
  const selectedDriverIdxRef = useRef<number | null>(null)
  const camRef = useRef<{ scale: number; ox: number; oy: number } | null>(null)
  selectedDriverIdxRef.current = selectedDriverIdx

  const [zoomLevel, setZoomLevel] = useState<number>(4)
  const zoomLevelRef = useRef<number>(4)
  zoomLevelRef.current = zoomLevel

  const map = trackId != null ? TRACK_MAPS[trackId] ?? null : null

  participantsRef.current = participants

  // Precompute rotated geometry whenever the map changes
  useEffect(() => {
    prepRef.current = map ? prepareMap(map) : null
  }, [map])

  const lastPosRef = useRef<Record<number, { x: number; z: number; lastMovedTime: number }>>({})
  const mapTimeoutRef = useRef<number>(mapTimeout)
  mapTimeoutRef.current = mapTimeout

  // Subscribe to position updates directly — bypasses React state to avoid 60 Hz re-renders
  useEffect(() => {
    const handleMsg = (msg: any) => {
      if (msg.type === 'positions' && msg.cars) {
        const now = Date.now()
        const timeoutMs = mapTimeoutRef.current * 1000
        const playerIdx = msg.player_idx ?? 0

        const filteredCars = msg.cars.map((car: any) => {
          if (car.idx === playerIdx) {
            return car
          }

          const prev = lastPosRef.current[car.idx]
          if (!prev) {
            lastPosRef.current[car.idx] = { x: car.x, z: car.z, lastMovedTime: now }
            return car
          }

          const hasMoved = car.x !== prev.x || car.z !== prev.z
          if (hasMoved) {
            lastPosRef.current[car.idx] = { x: car.x, z: car.z, lastMovedTime: now }
            return car
          } else {
            const duration = now - prev.lastMovedTime
            if (mapTimeoutRef.current > 0 && duration > timeoutMs) {
              return { ...car, x: 0, z: 0 }
            }
            return car
          }
        })

        _cachedCars      = filteredCars
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
          handleMsg(raw)
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
        for (const row of rows) handleMsg(row)
      } catch (e) {
        console.error('TrackMap: failed to decode binary batch:', e)
      }
    })

    return () => {
      unsubBatch()
      unsubOn()
      unsubBinary()
    }
  }, [])

  isDarkRef.current            = isDark
  sectorColorsRef.current      = sectorColors
  driversModeRef.current       = driversMode
  mapDimmedRef.current         = mapDimmed
  aeroModeRef.current          = aeroMode
  slmTrackStatusRef.current    = slmTrackStatus
  mapTimeoutRef.current        = mapTimeout
  const reduceAnimationsRef    = useRef(reduceAnimations)
  reduceAnimationsRef.current  = reduceAnimations

  // Resize canvas physical pixels when the container changes
  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas || width === 0 || height === 0) return
    const dpr = window.devicePixelRatio || 1
    canvas.width  = Math.round(width  * dpr)
    canvas.height = Math.round(height * dpr)
    canvas.style.width  = `${width}px`
    canvas.style.height = `${height}px`
  }, [width, height])

  // rAF render loop
  useEffect(() => {
    if (!map || width === 0 || height === 0) return
    let active = true

    const loop = () => {
      if (!active) return
      const canvas = canvasRef.current
      const prep   = prepRef.current
      if (canvas && prep) {
        const dpr = window.devicePixelRatio || 1
        const ctx = canvas.getContext('2d')
        if (ctx) {
          ctx.setTransform(dpr, 0, 0, dpr, 0, 0)

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
          renderFrame(ctx, width, height, prep, map, carsRef.current, participantsRef.current, isDarkRef.current, sectorColorsRef.current, driversModeRef.current, layout, effectiveZoom, mapDimmedRef.current, aeroModeRef.current, slmTrackStatusRef.current)
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

  const driverOptions = useMemo((): (DriverOption & { raceNumber: number })[] => {
    return (participants?.drivers ?? [])
      .filter(d => d.name.trim() !== '' || d.race_number > 0)
      .map(d => {
        const name = d.name.trim()
        const lastName = name ? (name.split(/\s+/).pop() ?? name).toUpperCase() : `C${d.idx}`
        return {
          value: d.idx,
          label: `${d.race_number} ${lastName}`,
          raceNumber: d.race_number
        }
      })
      .sort((a, b) => a.raceNumber - b.raceNumber)
  }, [participants])

  const handleDriverChange = useCallback((opt: SingleValue<DriverOption>) => {
    setSelectedDriverIdx(opt?.value ?? null)
  }, [])

  const handleZoomChange = useCallback((opt: SingleValue<ZoomOption>) => {
    setZoomLevel(opt?.value ?? 4)
  }, [])

  return (
    <div ref={wrapRef} className="relative w-full h-full">
      {map
        ? <canvas ref={canvasRef} className="absolute inset-0" />
        : (
          <div className="absolute inset-0 flex items-center justify-center">
            <span className="text-[11px] uppercase tracking-widest text-[var(--text-secondary)]">
              No map data
            </span>
          </div>
        )
      }
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
    </div>
  )
}
