import { useRef, useEffect, useState } from 'react'
import Select, { type StylesConfig, type SingleValue } from 'react-select'
import { Maximize2, Minimize2 } from 'lucide-react'
import { useSize } from '../hooks/useSize'
import { TRACK_MAPS, type TrackMapData } from '../lib/trackMaps'
import type { CarPosition, ParticipantsMsg } from '../types'

const FOLLOW_ZOOM = 4

type DriverOption = { value: number; label: string }

function buildSelectStyles(isDark: boolean): StylesConfig<DriverOption> {
  const bg          = isDark ? '#1a1f2e' : '#ffffff'
  const text        = isDark ? '#e0e0e0' : '#111827'
  const muted       = isDark ? '#6b7280' : '#9ca3af'
  const border      = isDark ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.15)'
  const hoverBg     = isDark ? 'rgba(255,255,255,0.08)' : '#f3f4f6'
  const selectedBg  = '#0090D0'
  return {
    control: (base, state) => ({
      ...base,
      background: bg,
      borderColor: state.isFocused ? selectedBg : border,
      boxShadow: state.isFocused ? `0 0 0 1px ${selectedBg}` : 'none',
      minHeight: 28,
      fontSize: 11,
      cursor: 'pointer',
      '&:hover': { borderColor: selectedBg },
    }),
    menu: (base) => ({
      ...base,
      background: bg,
      border: `1px solid ${border}`,
      boxShadow: '0 4px 12px rgba(0,0,0,0.35)',
      zIndex: 9999,
    }),
    option: (base, state) => ({
      ...base,
      background: state.isSelected ? selectedBg : state.isFocused ? hoverBg : 'transparent',
      color: state.isSelected ? '#fff' : text,
      fontSize: 11,
      padding: '4px 8px',
      cursor: 'pointer',
    }),
    singleValue:       (base) => ({ ...base, color: text,  fontSize: 11 }),
    placeholder:       (base) => ({ ...base, color: muted, fontSize: 11 }),
    input:             (base) => ({ ...base, color: text,  fontSize: 11 }),
    indicatorSeparator: ()   => ({ display: 'none' }),
    dropdownIndicator: (base) => ({ ...base, padding: '0 4px', color: muted }),
    clearIndicator:    (base) => ({ ...base, padding: '0 4px', color: muted }),
    valueContainer:    (base) => ({ ...base, padding: '0 6px' }),
  }
}

const SECTOR_COLORS_DARK  = ['#E8002D', '#0090D0', '#FFD700']
const SECTOR_COLORS_LIGHT = ['#D32F2F', '#0D47A1', '#B7950B']
const SECTOR_COLORS       = SECTOR_COLORS_DARK
const DRS_COLOR     = '#39B54A'
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

  const drsZones   = map.drs_zones.map(z => ({ track_points: (z.track_points as [number, number][]).map(rot) }))
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
    sectors, drsZones, speedTraps, startFinish, s1pts, sfIdx, junctions,
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

function drawDrsZone(
  ctx: CanvasRenderingContext2D,
  pts: [number, number][],
  scale: number, ox: number, oy: number,
) {
  if (pts.length < 2) return
  ctx.beginPath()
  ctx.strokeStyle = DRS_COLOR
  ctx.lineWidth   = DRS_PX
  ctx.lineCap     = 'butt'
  ctx.lineJoin    = 'miter'
  ctx.setLineDash([3, 3])
  for (let i = 0; i < pts.length; i++) {
    const [nx, ny] = perp(pts, i)
    const [cx2, cy2] = toCanvas(pts[i], scale, ox, oy)
    const px = cx2 + nx * DRS_OFFSET
    const py = cy2 + ny * DRS_OFFSET
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
) {
  const [cx2, cy2] = toCanvas(pt, scale, ox, oy)
  ctx.strokeStyle = TRAP_COLOR
  ctx.lineWidth   = 2
  for (const r of [TRAP_R, TRAP_R + 5, TRAP_R + 10]) {
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
  colorOverride?: string,
) {
  const [nx, ny] = perp(normalPts, idx)
  const [cx2, cy2] = toCanvas(pt, scale, ox, oy)
  ctx.beginPath()
  ctx.strokeStyle = colorOverride ?? SF_COLOR
  ctx.lineWidth   = 2.5
  ctx.lineCap     = 'round'
  ctx.moveTo(cx2 - nx * SF_HALF, cy2 - ny * SF_HALF)
  ctx.lineTo(cx2 + nx * SF_HALF, cy2 + ny * SF_HALF)
  ctx.stroke()
}

function drawSectorJunction(
  ctx: CanvasRenderingContext2D,
  j: SectorJunction,
  scale: number, ox: number, oy: number,
  colorOverride?: string,
) {
  const [cx2, cy2] = toCanvas(j.pt, scale, ox, oy)
  ctx.beginPath()
  ctx.strokeStyle = colorOverride ?? j.color
  ctx.lineWidth   = 3
  ctx.lineCap     = 'round'
  ctx.moveTo(cx2 - j.nx * JUNC_HALF, cy2 - j.ny * JUNC_HALF)
  ctx.lineTo(cx2 + j.nx * JUNC_HALF, cy2 + j.ny * JUNC_HALF)
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
  ctx.font         = 'bold 9px ui-monospace, monospace'
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
) {
  ctx.clearRect(0, 0, cw, ch)
  const { scale, ox, oy } = layout

  const trackColor = isDark ? '#ffffff' : '#000000'
  const colors = isDark ? SECTOR_COLORS_DARK : SECTOR_COLORS_LIGHT

  for (const sector of prep.sectors) {
    const color = sectorColors ? (colors[sector.index - 1] ?? trackColor) : trackColor
    drawPolyline(ctx, sector.points, scale, ox, oy, color, TRACK_PX)
  }

  for (let idx = 0; idx < prep.junctions.length; idx++) {
    const j = prep.junctions[idx]
    const color = sectorColors ? trackColor : (colors[idx + 1] ?? trackColor)
    drawSectorJunction(ctx, j, scale, ox, oy, color)
  }

  for (const zone of prep.drsZones) {
    drawDrsZone(ctx, zone.track_points, scale, ox, oy)
  }

  for (const trap of prep.speedTraps) {
    drawSpeedTrap(ctx, trap, scale, ox, oy)
  }

  if (prep.startFinish) {
    drawStartFinish(ctx, prep.startFinish, prep.s1pts, prep.sfIdx, scale, ox, oy, sectorColors ? trackColor : undefined)
  }

  if (cars) {
    drawCarDots(ctx, cars, participants, map, prep, scale, ox, oy, driversMode, isDark)
  }
}

// ── Module-level position cache (survives component remounts) ─────────────────

let _cachedCars:      CarPosition[] | null = null
let _cachedPlayerIdx: number               = 0


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
}

export default function TrackMap({ trackId, participants, isDark, sectorColors = false, driversMode = 'both', mapTimeout = 10, isFullscreen = false, onToggleFullscreen }: Props) {
  const { ref: wrapRef, width, height } = useSize()
  const canvasRef       = useRef<HTMLCanvasElement>(null)
  const prepRef         = useRef<PreparedMap | null>(null)
  const carsRef         = useRef<CarPosition[] | null>(_cachedCars)
  const playerIdxRef    = useRef<number>(_cachedPlayerIdx)
  const participantsRef = useRef<ParticipantsMsg | null>(null)
  const rafRef          = useRef<number>(0)
  const isDarkRef         = useRef<boolean>(isDark)
  const sectorColorsRef   = useRef<boolean>(sectorColors)
  const driversModeRef     = useRef<'dots' | 'both' | 'labels'>(driversMode)

  const [selectedDriverIdx, setSelectedDriverIdx] = useState<number | null>(null)
  const selectedDriverIdxRef = useRef<number | null>(null)
  const camRef = useRef<{ scale: number; ox: number; oy: number } | null>(null)
  selectedDriverIdxRef.current = selectedDriverIdx

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
    return window.telemetryBridge.on((raw) => {
      const msg = raw as { type: string; player_idx?: number; cars?: CarPosition[] }
      if (msg.type === 'positions' && msg.cars) {
        const now = Date.now()
        const timeoutMs = mapTimeoutRef.current * 1000
        const playerIdx = msg.player_idx ?? 0

        const filteredCars = msg.cars.map(car => {
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
    })
  }, [])

  isDarkRef.current       = isDark
  sectorColorsRef.current = sectorColors
  driversModeRef.current   = driversMode
  mapTimeoutRef.current    = mapTimeout

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
              const followScale = baseLayout.scale * FOLLOW_ZOOM
              const LERP = 0.12
              if (!camRef.current) {
                camRef.current = { scale: baseLayout.scale, ox: 0, oy: 0 }
              }
              camRef.current.scale += (followScale - camRef.current.scale) * LERP
              // Snap pan so the driver is always exactly centered — no lag on the dot
              camRef.current.ox = width  / 2 - rx * camRef.current.scale
              camRef.current.oy = height / 2 - ry * camRef.current.scale
              layout = camRef.current
            }
          } else if (camRef.current) {
            const LERP = 0.12
            camRef.current = {
              scale: camRef.current.scale + (baseLayout.scale - camRef.current.scale) * LERP,
              ox:    camRef.current.ox    + (baseLayout.ox    - camRef.current.ox)    * LERP,
              oy:    camRef.current.oy    + (baseLayout.oy    - camRef.current.oy)    * LERP,
            }
            const diff = Math.abs(camRef.current.scale - baseLayout.scale)
                       + Math.abs(camRef.current.ox    - baseLayout.ox)
                       + Math.abs(camRef.current.oy    - baseLayout.oy)
            if (diff < 0.5) camRef.current = null
            layout = camRef.current ?? baseLayout
          }

          renderFrame(ctx, width, height, prep, map, carsRef.current, participantsRef.current, isDarkRef.current, sectorColorsRef.current, driversModeRef.current, layout)
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

  const driverOptions: DriverOption[] = (participants?.drivers ?? [])
    .filter(d => d.name.trim() !== '' || d.race_number > 0)
    .map(d => ({ value: d.idx, label: d.name.trim() || String(d.race_number) }))

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
      {map && driverOptions.length > 0 && (
        <div className="absolute top-2 left-2 z-10 w-44">
          <Select<DriverOption>
            value={driverOptions.find(o => o.value === selectedDriverIdx) ?? null}
            options={driverOptions}
            onChange={(opt: SingleValue<DriverOption>) => setSelectedDriverIdx(opt?.value ?? null)}
            isClearable
            isSearchable
            placeholder="Follow driver…"
            styles={buildSelectStyles(isDark)}
          />
        </div>
      )}
      {onToggleFullscreen && (
        <button
          onClick={onToggleFullscreen}
          className="absolute top-2 right-2 p-1.5 rounded text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-white/10 transition-colors"
        >
          {isFullscreen ? <Minimize2 size={14} /> : <Maximize2 size={14} />}
        </button>
      )}
    </div>
  )
}
