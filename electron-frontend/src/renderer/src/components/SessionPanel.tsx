import { useRef, useState, memo } from 'react'
import { flushSync } from 'react-dom'
import { Sun, CloudSun, Cloud, CloudDrizzle, CloudRain, CloudLightning, type LucideIcon } from 'lucide-react'
import type { SessionMsg, RaceEventMsg, TimingMsg, ParticipantsMsg, TimingCar } from '../types'
import TrackMap from './TrackMap'
import { useColorFn } from '../lib/cards'
import { useLabels } from '../lib/labels'
import { TRACK_MAPS } from '../lib/trackMaps'
import { DEFAULT_SESSION_LAYOUT, type SessionLayout } from '../app/appConfig'
import type { DensityMode } from '../lib/graphSections'

// ─── Lookup tables ───────────────────────────────────────────────────────────

export const SESSION_TYPES: Record<number, string> = {
  0: 'Unknown', 1: 'Practice 1', 2: 'Practice 2', 3: 'Practice 3',
  4: 'Short Practice', 5: 'Qualifying 1', 6: 'Qualifying 2', 7: 'Qualifying 3',
  8: 'Short Qualifying', 9: 'One-Shot Qualifying',
  10: 'Sprint Shootout 1', 11: 'Sprint Shootout 2', 12: 'Sprint Shootout 3',
  13: 'Short Sprint Shootout', 14: 'One-Shot Sprint Shootout',
  15: 'Race', 16: 'Race 2', 17: 'Race 3', 18: 'Time Trial',
}

const WEATHER_LABELS = ['Clear', 'Light Cloud', 'Overcast', 'Light Rain', 'Heavy Rain', 'Storm']
const WEATHER_ICONS: { icon: LucideIcon; dark: string; light: string }[] = [
  { icon: Sun,            dark: '#fde047', light: '#ca8a04' },  // Clear
  { icon: CloudSun,       dark: '#fb923c', light: '#c2410c' },  // Light Cloud
  { icon: Cloud,          dark: '#94a3b8', light: '#475569' },  // Overcast
  { icon: CloudDrizzle,   dark: '#7dd3fc', light: '#0284c7' },  // Light Rain
  { icon: CloudRain,      dark: '#2563eb', light: '#1d4ed8' },  // Heavy Rain
  { icon: CloudLightning, dark: '#c084fc', light: '#7c3aed' },  // Storm
]

const FLAG_BG: Record<number, string> = {
  1: '#00c853',
  2: '#2196f3',
  3: '#fdd835',
}

export function sessionAccent(t: number, isDark: boolean): string {
  if (t >= 1  && t <= 4)  return isDark ? '#FB923C' : '#A04300'
  if (t >= 5  && t <= 14) return isDark ? '#ffd700' : '#765900'
  if (t >= 15 && t <= 17) return isDark ? '#5794F2' : '#0B57D0'
  return isDark ? '#a0a8b8' : '#565B70'
}

// Track/air temp colours come from the shared library spec (session.trackTemp /
// session.airTemp) via the card colour evaluator — see useColorFn below.

// ─── Formatters ──────────────────────────────────────────────────────────────

function fmtTimeLeft(s: number): string {
  if (s <= 0) return '0:00'
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`
}

function fmtSessionTime(s?: number): string {
  if (s == null || s <= 0) return '00:00'
  return `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(Math.floor(s % 60)).padStart(2, '0')}`
}

function fmtDelta(ms: number, sign: 'ahead' | 'behind'): string {
  return `${sign === 'ahead' ? '-' : '+'}${(ms / 1000).toFixed(3)}`
}

function fmtTimeOfDay(mins: number): string {
  const h = Math.floor(mins / 60) % 24
  const m = mins % 60
  return `${h % 12 || 12}:${String(m).padStart(2, '0')} ${h >= 12 ? 'PM' : 'AM'}`
}

function driverName(p: ParticipantsMsg | null, idx: number): string {
  const d = p?.drivers.find(d => d.idx === idx)
  if (!d) return `Car ${idx}`
  const parts = d.name.trim().split(/\s+/)
  return parts[parts.length - 1].toUpperCase()
}

function formatEvent(
  ev: RaceEventMsg,
  p: ParticipantsMsg | null,
  isDark: boolean,
  labels: Readonly<Record<string, string>>,
): { label: string; color: string } | null {
  const name = (idx?: number) => driverName(p, idx ?? 0)
  const fmtLap = (s: number) => `${Math.floor(s / 60)}:${(s % 60).toFixed(3).padStart(6, '0')}`
  switch (ev.code) {
    case 'FTLP': return { label: `Fastest Lap — ${name(ev.car_idx)}  ${fmtLap(ev.lap_time_s ?? 0)}`, color: isDark ? '#BF5FFF' : '#7C3BA6' }
    case 'DRSE': return { label: 'DRS Enabled', color: isDark ? '#37872D' : '#137333' }
    case 'DRSD': return { label: 'DRS Disabled', color: isDark ? '#6e7177' : '#565B70' }
    case 'RDFL': return { label: 'Red Flag', color: '#e10600' }
    case 'CHQF': return { label: 'Chequered Flag', color: isDark ? '#a0a8b8' : '#565B70' }
    case 'LGOT': return { label: 'Lights Out', color: isDark ? '#37872D' : '#137333' }
    case 'SSTA': return { label: 'Session Start', color: isDark ? '#5794F2' : '#0B57D0' }
    case 'SEND': return { label: 'Session End', color: isDark ? '#5794F2' : '#0B57D0' }
    case 'RTMT': return { label: `Retired — ${name(ev.car_idx)}`, color: isDark ? '#a0a8b8' : '#565B70' }
    case 'RCWN': return { label: `Race Winner — ${name(ev.car_idx)}`, color: isDark ? '#FFD700' : '#765900' }
    case 'DTSV': return { label: `DT Served — ${name(ev.car_idx)}`, color: isDark ? '#a0a8b8' : '#565B70' }
    case 'SGSV': return { label: `SG Served — ${name(ev.car_idx)}`, color: isDark ? '#a0a8b8' : '#565B70' }
    case 'SCAR': {
      const T: Record<number, string> = { 1: 'Safety Car', 2: 'Virtual SC', 3: 'Formation Lap' }
      const A: Record<number, string> = { 0: 'Deployed', 1: 'Returning', 2: 'Returned', 3: 'Resume Race' }
      return { label: `${T[ev.safety_car_type ?? 0] ?? 'SC'} — ${A[ev.event_type ?? 0] ?? ''}`, color: isDark ? '#ffd700' : '#765900' }
    }
    case 'PENA': {
      const pt = ev.penalty_type ?? 0
      const penaltyLabel = labels[`penalty.${pt}`]
      if (!penaltyLabel) return null
      const color = pt === 5
        ? (isDark ? '#ffd700' : '#765900')
        : pt === 2 || pt === 4
          ? (isDark ? '#c47d0e' : '#A04300')
          : (isDark ? '#e10600' : '#C4162A')
      const ts = (pt === 1 || pt === 4) && ev.penalty_time_s ? ` ${ev.penalty_time_s}s` : ''
      const inf = ev.infringement_type != null ? labels[`infringe.${ev.infringement_type}`] : undefined
      return { label: `${penaltyLabel}${ts} — ${name(ev.car_idx)}${inf ? ` — ${inf}` : ''}`, color }
    }
    case 'OVTK': return { label: `Overtake — ${name(ev.overtaking_car_idx)} passed ${name(ev.being_overtaken_car_idx)}`, color: isDark ? '#a0a8b8' : '#565B70' }
    case 'SPTP': return null
    default: return null
  }
}

// ─── Sub-components ──────────────────────────────────────────────────────────

const WeatherIcon = memo(function WeatherIcon({ id, size, isDark }: { id: number; size: number; isDark: boolean }) {
  const entry = WEATHER_ICONS[id] ?? WEATHER_ICONS[2]
  return <entry.icon size={size} strokeWidth={1.5} color={isDark ? entry.dark : entry.light} />
})

// The weather's icon colour, reused to tint the label in the compact strip (native
// colours the weather name with the icon colour when the icon itself is dropped).
function weatherColor(id: number, isDark: boolean): string {
  const e = WEATHER_ICONS[id] ?? WEATHER_ICONS[2]
  return isDark ? e.dark : e.light
}

const StatCard = memo(function StatCard({ label, value, unit, accent, sub, compact }: { label: string; value: string; unit?: string; accent?: string; sub?: string; compact?: DensityMode | boolean }) {
  const isCompact = compact === true || compact === 'compact'
  const isSpacious = compact === 'spacious'

  if (isCompact) {
    // Single-line: label · value+unit — the row collapses to one line.
    return (
      <div className="flex-1 min-w-0 overflow-hidden flex items-center justify-between gap-2 px-4 py-2 border-r border-[var(--border)] last:border-r-0">
        <span className="min-w-0 text-[10px] font-medium tracking-widest uppercase text-[var(--text-secondary)] truncate">{label}</span>
        <span className="flex items-baseline gap-1 shrink-0">
          <span className="text-lg font-black tabular-nums leading-none" style={{ color: accent ?? 'var(--text-primary)' }}>{value}</span>
          {unit && <span className="text-[10px] text-[var(--text-secondary)]">{unit}</span>}
        </span>
      </div>
    )
  }

  if (isSpacious) {
    return (
      <div className="flex-1 min-w-0 overflow-hidden flex flex-col justify-between px-6 py-5 border-r border-[var(--border)] last:border-r-0">
        <span className="text-xs font-black tracking-widest uppercase text-[var(--text-secondary)] truncate">{label}</span>
        <div>
          <div className="flex items-baseline gap-1.5 overflow-hidden">
            <span className="text-4xl font-black tabular-nums leading-none truncate" style={{ color: accent ?? 'var(--text-primary)' }}>
              {value}
            </span>
            {unit && <span className="text-sm font-semibold text-[var(--text-secondary)] shrink-0">{unit}</span>}
          </div>
          {sub && <div className="text-[11px] font-medium text-[var(--text-secondary)] mt-1.5 truncate">{sub}</div>}
        </div>
      </div>
    )
  }

  return (
    <div className="flex-1 min-w-0 overflow-hidden flex flex-col justify-between px-5 py-4 border-r border-[var(--border)] last:border-r-0">
      <span className="text-[10px] font-medium tracking-widest uppercase text-[var(--text-secondary)] truncate">{label}</span>
      <div>
        <div className="text-3xl font-black tabular-nums leading-none mt-2 truncate" style={{ color: accent ?? 'var(--text-primary)' }}>
          {value}
        </div>
        {unit && <div className="text-xs text-[var(--text-secondary)] mt-1">{unit}</div>}
      </div>
    </div>
  )
})

function areProximityPropsEqual(prev: any, next: any) {
  if (prev.compact !== next.compact) return false
  if (prev.participants !== next.participants) return false
  if (!prev.timing || !next.timing) return prev.timing === next.timing
  if (prev.timing.player_idx !== next.timing.player_idx) return false
  if (prev.timing.cars.length !== next.timing.cars.length) return false
  for (let i = 0; i < prev.timing.cars.length; i++) {
    const c1 = prev.timing.cars[i]
    const c2 = next.timing.cars[i]
    if (
      c1.idx !== c2.idx ||
      c1.position !== c2.position ||
      c1.gap_ms !== c2.gap_ms ||
      c1.result_status !== c2.result_status
    ) {
      return false
    }
  }
  return true
}

const ProximityWidget = memo(function ProximityWidget({ timing, participants, compact }: {
  timing: TimingMsg; participants: ParticipantsMsg | null; compact?: DensityMode | boolean
}) {
  const isCompact = compact === true || compact === 'compact'
  const isSpacious = compact === 'spacious'

  const active = timing.cars
    .filter(c => c.result_status === 2 && c.position > 0)
    .sort((a, b) => a.position - b.position)

  const player = active.find(c => c.idx === timing.player_idx)
  if (!player) return <p className="text-xs text-[var(--text-secondary)] px-4 py-3">No position data</p>

  const pos = player.position
  const isFirst = pos === 1
  const isLast  = pos === active.length

  type Row = { car: TimingCar; isPlayer: boolean; deltaMs: number | null; sign: 'ahead' | 'behind' | null }
  const rows: Row[] = []

  if (isFirst) {
    rows.push({ car: player, isPlayer: true, deltaMs: null, sign: null })
    const p2 = active.find(c => c.position === 2)
    const p3 = active.find(c => c.position === 3)
    if (p2) rows.push({ car: p2, isPlayer: false, deltaMs: p2.gap_ms - player.gap_ms, sign: 'behind' })
    if (p3) rows.push({ car: p3, isPlayer: false, deltaMs: p3.gap_ms - player.gap_ms, sign: 'behind' })
  } else if (isLast) {
    const p2b = active.find(c => c.position === pos - 2)
    const p1b = active.find(c => c.position === pos - 1)
    if (p2b) rows.push({ car: p2b, isPlayer: false, deltaMs: player.gap_ms - p2b.gap_ms, sign: 'ahead' })
    if (p1b) rows.push({ car: p1b, isPlayer: false, deltaMs: player.gap_ms - p1b.gap_ms, sign: 'ahead' })
    rows.push({ car: player, isPlayer: true, deltaMs: null, sign: null })
  } else {
    const ahead  = active.find(c => c.position === pos - 1)
    const behind = active.find(c => c.position === pos + 1)
    if (ahead)  rows.push({ car: ahead,  isPlayer: false, deltaMs: player.gap_ms - ahead.gap_ms,  sign: 'ahead' })
    rows.push({ car: player, isPlayer: true, deltaMs: null, sign: null })
    if (behind) rows.push({ car: behind, isPlayer: false, deltaMs: behind.gap_ms - player.gap_ms, sign: 'behind' })
  }

  return (
    <div className="flex flex-col divide-y divide-[var(--border)]">
      {rows.map(({ car, isPlayer, deltaMs, sign }) => {
        const d = participants?.drivers.find(drv => drv.idx === car.idx)
        return (
          <div
            key={car.idx}
            className={isCompact ? "flex items-center gap-3 px-3 h-[32px]" : isSpacious ? "flex items-center gap-3 px-4 py-3.5" : "flex items-center gap-3 px-3 py-3"}
          >
            <span className={`${isSpacious ? 'text-xs font-black w-8' : 'text-[10px] font-bold w-7'} tabular-nums text-center text-[var(--text-primary)] shrink-0`}>
              P{car.position}
            </span>
            {d && isSpacious && (
              <div className="w-1.5 h-4 rounded-full shrink-0" style={{ background: d.livery_color }} />
            )}
            <span className={`${isCompact ? 'text-xs' : isSpacious ? 'text-base font-black' : 'text-sm font-bold'} flex-1 truncate ${isPlayer ? 'text-[var(--text-primary)]' : 'text-[var(--text-secondary)]'}`}>
              {driverName(participants, car.idx)}
            </span>
            {d?.race_number && isSpacious && (
              <span className="text-xs font-semibold text-[var(--text-secondary)] tabular-nums shrink-0">
                #{d.race_number}
              </span>
            )}
            {deltaMs != null && deltaMs > 0 && (
              <span className={`${isCompact ? 'text-[11px]' : isSpacious ? 'text-sm font-black' : 'text-xs font-semibold'} tabular-nums shrink-0`} style={{ color: sign === 'ahead' ? '#73BF69' : '#6e7177' }}>
                {fmtDelta(deltaMs, sign!)}
              </span>
            )}
          </div>
        )
      })}
    </div>
  )
}, areProximityPropsEqual)

function areMarshalPropsEqual(prev: any, next: any) {
  if (prev.isDark !== next.isDark) return false
  if (!prev.zones || !next.zones) return prev.zones === next.zones
  if (prev.zones.length !== next.zones.length) return false
  for (let i = 0; i < prev.zones.length; i++) {
    const z1 = prev.zones[i]
    const z2 = next.zones[i]
    if (z1.flag !== z2.flag || z1.zone_start !== z2.zone_start) {
      return false
    }
  }
  return true
}

const MarshalStrip = memo(function MarshalStrip({ zones, isDark }: { zones: SessionMsg['marshal_zones']; isDark: boolean }) {
  const valid = zones.filter(z => z.flag !== -1)
  if (valid.length === 0) return <p className="text-xs text-[var(--text-secondary)]">No zone data</p>

  const segs = valid.map((z, i) => ({
    flag: z.flag,
    width: Math.max(((valid[i + 1]?.zone_start ?? 1.0) - z.zone_start) * 100, 0),
    num: i + 1,
  }))

  return (
    <div className="flex w-full h-1 gap-0.5">
      {segs.map(s => (
        <div
          key={s.num}
          style={{ width: `${s.width}%`, backgroundColor: FLAG_BG[s.flag] ?? (isDark ? 'rgba(255,255,255,0.07)' : 'rgba(0,0,0,0.08)') }}
          className="rounded-full"
          title={`Zone ${s.num}`}
        />
      ))}
    </div>
  )
}, areMarshalPropsEqual)

// ─── Main component ───────────────────────────────────────────────────────────

interface Props {
  session: SessionMsg | null
  raceEvents: RaceEventMsg[]
  timing: TimingMsg | null
  participants: ParticipantsMsg | null
  isDark: boolean
  sectorColors: boolean
  driversMode: 'dots' | 'both' | 'labels'
  mapTimeout: number
  reduceAnimations: boolean
  mapDimmed?: boolean
  aeroMode: 'drs' | 'slm'
  compactHeader?: boolean | number
  compactCards?: DensityMode | boolean
  compactWeather?: number
  compactEvents?: DensityMode | boolean
  compactProximity?: DensityMode | boolean
  layout?: SessionLayout
}

const SessionPanel = memo(function SessionPanel({ session, raceEvents, timing, participants, isDark, sectorColors, driversMode, mapTimeout, reduceAnimations, mapDimmed = false, aeroMode, compactHeader, compactCards, compactWeather, compactEvents, compactProximity, layout = DEFAULT_SESSION_LAYOUT }: Props) {
  const logRef = useRef<HTMLDivElement>(null)
  const [mapFullscreen, setMapFullscreen] = useState(false)
  const { t, raw: labels } = useLabels()
  const weatherLevel = Math.max(0, Math.min(4, compactWeather ?? 0))
  const headerLevel = typeof compactHeader === 'boolean' ? (compactHeader ? 1 : 0) : Math.max(0, Math.min(3, compactHeader ?? 0))
  const isCompactHeader = headerLevel === 1 || headerLevel === 2
  const isSpaciousHeader = headerLevel === 3

  const isCompactEvents = compactEvents === true || compactEvents === 'compact'
  const isSpaciousEvents = compactEvents === 'spacious'

  const isCompactProximity = compactProximity === true || compactProximity === 'compact'
  const isSpaciousProximity = compactProximity === 'spacious'

  const setMapFullscreenAnimated = (fullscreen: boolean) => {
    const transitionDocument = document as Document & {
      startViewTransition?: (update: () => void) => unknown
    }
    const motionReduced = reduceAnimations
      || document.documentElement.dataset.reduceAnimations === 'true'

    if (motionReduced || !transitionDocument.startViewTransition) {
      setMapFullscreen(fullscreen)
      return
    }

    try {
      transitionDocument.startViewTransition(() => {
        flushSync(() => setMapFullscreen(fullscreen))
      })
    } catch {
      setMapFullscreen(fullscreen)
    }
  }

  const noData  = !session
  const colorFn = useColorFn(null, null, isDark)
  const accent  = session ? sessionAccent(session.session_type, isDark) : (isDark ? '#a0a8b8' : '#565B70')
  const info    = session ? TRACK_MAPS[session.track_id] : null
  const gpKey   = session ? `track.${session.track_id}.track_name` : ''
  const circuitKey = session ? `track.${session.track_id}.circuit_name` : ''
  const gpOverride = gpKey ? t(gpKey) : ''
  const circuitOverride = circuitKey ? t(circuitKey) : ''
  const gpName  = gpOverride && gpOverride !== gpKey
    ? gpOverride : (info?.track_name ?? (session ? `Track ${session.track_id}` : '—'))
  const circuit = circuitOverride && circuitOverride !== circuitKey
    ? circuitOverride : (info?.circuit_name ?? '')
  const sType   = session ? SESSION_TYPES[session.session_type] ?? 'Unknown' : null

  const playerCar = timing?.cars.find(c => c.idx === timing.player_idx && c.result_status === 2)
  const remainingLaps = (session && session.total_laps > 0 && playerCar)
    ? Math.max(session.total_laps - playerCar.lap_num + 1, 0) : null

  const pitWindow = session && session.pit_stop_window_ideal_lap > 0
    ? `L${session.pit_stop_window_ideal_lap}–${session.pit_stop_window_latest_lap}` : '—'
  const rejoinPos = session && session.pit_stop_rejoin_position > 0
    ? `P${session.pit_stop_rejoin_position}` : '—'

  const forecast = session
    ? session.weather_forecast_samples.filter(s => s.time_offset > 0).slice(0, 5)
    : []

  const visibleEvents = raceEvents
    .map(e => ({ event: e, fmt: formatEvent(e, participants, isDark, labels) }))
    .filter(({ fmt }) => fmt !== null)
    .reverse() as { event: RaceEventMsg; fmt: { label: string; color: string } }[]

  return (
    <div className="relative flex flex-col h-full overflow-hidden">

      {/* ── Normal layout ── */}
      {/* ── Header ── */}
      {(layout.header.gpName || layout.header.marshalZones || layout.header.timeLeft) && (
        <div
          className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]"
        >
          {/* GP name */}
          {layout.header.gpName && (
            <div className={`flex flex-col justify-center ${!layout.header.marshalZones && !layout.header.timeLeft ? 'flex-1' : 'shrink-0'} ${isSpaciousHeader ? 'px-8 py-5' : 'px-6'} ${isCompactHeader ? 'py-2' : isSpaciousHeader ? 'py-5' : 'py-4'}`}>
              <div className={`flex items-center gap-3 ${isCompactHeader ? '' : 'mb-0.5'}`}>
                <h1 className={`${isSpaciousHeader ? 'text-2xl font-black' : isCompactHeader ? 'text-base font-black' : 'text-xl font-black'} tracking-tight ${noData ? 'text-[var(--text-secondary)]' : 'text-[var(--text-primary)]'}`}>
                  {gpName}
                </h1>
              </div>
              {circuit && !isCompactHeader && <p className={`${isSpaciousHeader ? 'text-sm font-semibold' : 'text-xs'} text-[var(--text-secondary)] mt-0.5`}>{circuit}</p>}
            </div>
          )}

          {/* Zones strip */}
          {layout.header.marshalZones && (
            <div className={`flex flex-col justify-center flex-1 min-w-0 ${isSpaciousHeader ? 'px-8 py-5' : 'px-6'} ${isCompactHeader ? 'py-2' : isSpaciousHeader ? 'py-5' : 'py-4'}`}>
              {headerLevel === 3 ? (
                <div className="flex flex-col justify-center gap-2 w-full">
                  <div className="flex items-center justify-between">
                    <span className="text-xs font-black uppercase tracking-widest text-[var(--text-secondary)]">Marshal Zones</span>
                  </div>
                  <div className="w-full">
                    <MarshalStrip zones={session?.marshal_zones ?? []} isDark={isDark} />
                  </div>
                </div>
              ) : headerLevel === 2 ? (
                <div className="w-full">
                  <MarshalStrip zones={session?.marshal_zones ?? []} isDark={isDark} />
                </div>
              ) : headerLevel === 1 ? (
                <div className="flex items-center gap-3 w-full">
                  <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)] shrink-0">Zones</span>
                  <div className="flex-1 min-w-0"><MarshalStrip zones={session?.marshal_zones ?? []} isDark={isDark} /></div>
                </div>
              ) : (
                <>
                  <div className="flex items-center mb-2">
                    <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)] shrink-0">Zones</span>
                  </div>
                  <MarshalStrip zones={session?.marshal_zones ?? []} isDark={isDark} />
                </>
              )}
            </div>
          )}

          {/* Time left */}
          {layout.header.timeLeft && (
            <div className={`flex flex-col justify-center items-end ${!layout.header.gpName && !layout.header.marshalZones ? 'flex-1' : 'shrink-0'} ${!layout.header.marshalZones && layout.header.gpName ? 'ml-auto' : ''} ${isSpaciousHeader ? 'px-8 py-5' : 'px-6'} ${isCompactHeader ? 'py-2' : isSpaciousHeader ? 'py-5' : 'py-4'}`}>
              {!isCompactHeader && (
                <div className={`${isSpaciousHeader ? 'text-xs font-bold' : 'text-[10px] font-medium'} uppercase tracking-widest text-[var(--text-secondary)]`}>Time Left</div>
              )}
              <div
                className={`${isSpaciousHeader ? 'text-4xl' : isCompactHeader ? 'text-xl' : 'text-3xl'} font-black tabular-nums leading-tight`}
                style={{ color: noData ? 'var(--text-secondary)' : 'var(--text-primary)' }}
              >
                {session ? fmtTimeLeft(session.session_time_left) : '--:--'}
              </div>
            </div>
          )}
        </div>
      )}

      {/* ── Stat cards ── */}
      {Object.values(layout.statsCards).some(Boolean) && (
        <div className="shrink-0 flex min-w-0 overflow-hidden border-b border-[var(--border)]">
          {layout.statsCards.totalLaps && <StatCard label="Total Laps" value={session && session.total_laps > 0 ? String(session.total_laps) : '—'} sub="Total distance" compact={compactCards} />}
          {layout.statsCards.lapsRemaining && (remainingLaps !== null
            ? <StatCard label="Remaining" value={String(remainingLaps)} sub="Laps to flag" compact={compactCards} />
            : <StatCard label="Remaining" value="—" sub="Laps to flag" compact={compactCards} />
          )}
          {layout.statsCards.pitSpeedLimit && <StatCard label="Pit Speed"  value={session ? String(session.pit_speed_limit) : '—'} unit={session ? 'km/h' : undefined} accent={noData ? undefined : colorFn('session.pitSpeed')} sub="Pitlane limit" compact={compactCards} />}
          {layout.statsCards.pitWindow && <StatCard label="Pit Window" value={pitWindow} accent={noData ? undefined : colorFn('session.pitWindow')} sub="Optimal window" compact={compactCards} />}
          {layout.statsCards.pitRejoin && <StatCard label="Rejoin"     value={rejoinPos} accent={noData ? undefined : colorFn('session.rejoin')} sub="Projected pos" compact={compactCards} />}
          {layout.statsCards.trackTemp && <StatCard label="Track Temp"   value={session ? `${session.track_temp}°C` : '—'} accent={session ? colorFn('session.trackTemp', session.track_temp) : undefined} sub="Tarmac surface" compact={compactCards} />}
          {layout.statsCards.airTemp && <StatCard label="Air Temp"     value={session ? `${session.air_temp}°C` : '—'} accent={session ? colorFn('session.airTemp', session.air_temp) : undefined} sub="Ambient air" compact={compactCards} />}
          {layout.statsCards.trackLength && <StatCard label="Track Length" value={session ? `${(session.track_length_m / 1000).toFixed(3)} km` : '—'} sub="Lap distance" compact={compactCards} />}
          {layout.statsCards.timeOfDay && <StatCard label="Time of Day"  value={session ? fmtTimeOfDay(session.time_of_day) : '—'} sub="Circuit local" compact={compactCards} />}
        </div>
      )}

      {/* ── Main body — two columns that both fill the available height ── */}
      {(layout.showMap || layout.showWeather || layout.showProximity || layout.showEvents) && (
        <div className="flex flex-1 min-h-0 divide-x divide-[var(--border)]">

          {/* Col 1 — Weather: empty top (reserved), Now+forecast strip at bottom */}
          {(layout.showMap || layout.showWeather) && (
            <div
              className={`flex flex-col ${layout.showProximity || layout.showEvents ? '' : 'flex-1'} min-w-0 divide-y divide-[var(--border)]`}
              style={layout.showProximity || layout.showEvents ? { width: `${100 - (layout.sidebarPct ?? 28)}%` } : undefined}
            >

              {/* Track map */}
              {layout.showMap && (
                <div className={`session-map-transition ${mapFullscreen ? 'absolute inset-0 z-40 bg-[var(--bg-base)]' : 'flex-1 min-h-0'}`}>
                  <TrackMap trackId={session?.track_id ?? null} participants={participants} isDark={isDark} sectorColors={sectorColors}
                  driversMode={driversMode} mapTimeout={mapTimeout} reduceAnimations={reduceAnimations} mapDimmed={mapDimmed} aeroMode={aeroMode} slmTrackStatus={session?.active_aero_track_status ?? -1} isFullscreen={mapFullscreen} onToggleFullscreen={() => setMapFullscreenAnimated(!mapFullscreen)} />
                </div>
              )}

              {/* Now + forecast strip — pinned to bottom. Compact 1 keeps a smaller icon
                  in the horizontal row; Compact 2 is the original icon-free strip;
                  Compact 3 is the single-line layout: [Icon] Condition ... +Time Rain%;
                  Spacious is level 4 with extra large icons and metrics. */}
              {layout.showWeather && (
                <div className="flex shrink-0 divide-x divide-[var(--border)]" style={{ height: weatherLevel === 2 ? 32 : weatherLevel === 3 ? 34 : weatherLevel === 1 ? 58 : weatherLevel === 4 ? 110 : 90 }}>

                  {/* Now card — same width as the forecast cards */}
                  {weatherLevel === 4 ? (
                    <div className="flex-1 flex items-center justify-center gap-4 px-6 min-w-0">
                      <div className="shrink-0" style={{ color: noData ? 'var(--text-secondary)' : 'var(--text-primary)' }}>
                        <WeatherIcon id={session?.weather ?? 2} size={40} isDark={isDark} />
                      </div>
                      <div className="flex flex-col gap-1 min-w-0">
                        <span className="text-[11px] font-bold uppercase tracking-widest text-[var(--text-secondary)]">Now</span>
                        <span className={`text-base font-black truncate ${noData ? 'text-[var(--text-secondary)]' : 'text-[var(--text-primary)]'}`}>
                          {session ? WEATHER_LABELS[session.weather] ?? '—' : '—'}
                        </span>
                        <span className="text-[11px] font-semibold text-[var(--text-secondary)]">
                          {session ? (session.forecast_accuracy === 0 ? 'Exact' : 'Approx') : '—'}
                        </span>
                      </div>
                    </div>
                  ) : weatherLevel === 2 ? (
                    <div className="flex-1 flex items-center px-4 gap-2 min-w-0">
                <span className="text-[9px] font-medium uppercase tracking-widest text-[var(--text-secondary)] shrink-0">Now</span>
                <span className="flex-1 text-center text-xs font-semibold truncate" style={{ color: session ? weatherColor(session.weather, isDark) : 'var(--text-secondary)' }}>
                  {session ? WEATHER_LABELS[session.weather] ?? '—' : '—'}
                </span>
              </div>
            ) : weatherLevel === 3 ? (
              <div className="flex-1 flex items-center justify-between px-3 gap-2 min-w-0">
                <div className="flex items-center gap-2 min-w-0">
                  <span className="shrink-0 flex items-center justify-center">
                    <WeatherIcon id={session?.weather ?? 2} size={18} isDark={isDark} />
                  </span>
                  <span className={`text-xs font-semibold truncate ${noData ? 'text-[var(--text-secondary)]' : 'text-[var(--text-primary)]'}`}>
                    {session ? WEATHER_LABELS[session.weather] ?? '—' : '—'}
                  </span>
                </div>
                <span className="text-[10px] font-bold uppercase tracking-wider text-[var(--text-secondary)] shrink-0">
                  NOW
                </span>
              </div>
            ) : weatherLevel === 1 ? (
              <div className="flex-1 flex items-center px-3 gap-2 min-w-0">
                <span className="text-[9px] font-medium uppercase tracking-widest text-[var(--text-secondary)] shrink-0">Now</span>
                <span className="flex flex-1 items-center justify-center gap-2 min-w-0">
                  <span className="shrink-0"><WeatherIcon id={session?.weather ?? 2} size={26} isDark={isDark} /></span>
                  <span className={`text-sm font-semibold truncate ${noData ? 'text-[var(--text-secondary)]' : 'text-[var(--text-primary)]'}`}>
                    {session ? WEATHER_LABELS[session.weather] ?? '—' : '—'}
                  </span>
                </span>
              </div>
            ) : (
              <div className="flex-1 flex items-center justify-center gap-3 px-4 min-w-0">
                <div className="shrink-0" style={{ color: noData ? 'var(--text-secondary)' : 'var(--text-primary)' }}>
                  <WeatherIcon id={session?.weather ?? 2} size={32} isDark={isDark} />
                </div>
                <div className="flex flex-col gap-0.5 min-w-0">
                  <span className="text-[9px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Now</span>
                  <span className={`text-sm font-semibold truncate ${noData ? 'text-[var(--text-secondary)]' : 'text-[var(--text-primary)]'}`}>
                    {session ? WEATHER_LABELS[session.weather] ?? '—' : '—'}
                  </span>
                  <span className="text-[9px] text-[var(--text-secondary)]">
                    {session ? (session.forecast_accuracy === 0 ? 'Exact' : 'Approx') : '—'}
                  </span>
                </div>
              </div>
            )}

            {/* Forecast cards — skeleton when no data */}
            {noData
              ? [1, 2, 3, 4, 5].map(i => weatherLevel === 4 ? (
                <div key={i} className="flex-1 flex items-center justify-center gap-4 px-4 py-3 min-w-0">
                  <div className="shrink-0 text-[var(--text-secondary)]"><WeatherIcon id={2} size={40} isDark={isDark} /></div>
                  <div className="flex flex-col gap-1 min-w-0">
                    <span className="text-[11px] font-bold tabular-nums text-[var(--text-secondary)]">—</span>
                    <span className="text-xs font-bold text-[var(--text-secondary)] truncate">—</span>
                    <span className="text-sm font-black tabular-nums text-[var(--text-secondary)]">—</span>
                  </div>
                </div>
              ) : weatherLevel === 2 ? (
                <div key={i} className="flex-1 flex items-center px-2 gap-2 min-w-0">
                  <span className="text-[9px] tabular-nums text-[var(--text-secondary)] shrink-0">—</span>
                  <span className="flex-1 text-center text-xs text-[var(--text-secondary)]">—</span>
                  <span className="text-[10px] font-bold tabular-nums text-[var(--text-secondary)] shrink-0">—</span>
                </div>
              ) : weatherLevel === 3 ? (
                <div key={i} className="flex-1 flex items-center justify-between px-3 gap-2 min-w-0">
                  <div className="flex items-center gap-2 min-w-0">
                    <span className="shrink-0 flex items-center justify-center text-[var(--text-secondary)]">
                      <WeatherIcon id={2} size={18} isDark={isDark} />
                    </span>
                    <span className="text-xs text-[var(--text-secondary)] truncate">—</span>
                  </div>
                  <div className="flex items-baseline gap-1.5 shrink-0">
                    <span className="text-[10px] tabular-nums text-[var(--text-secondary)]">—</span>
                    <span className="text-xs font-bold tabular-nums text-[var(--text-secondary)]">—</span>
                  </div>
                </div>
              ) : weatherLevel === 1 ? (
                <div key={i} className="flex-1 flex items-center px-2 gap-1.5 min-w-0">
                  <span className="text-xs tabular-nums text-[var(--text-secondary)] shrink-0">—</span>
                  <span className="flex flex-1 items-center justify-center gap-1.5 min-w-0 text-[var(--text-secondary)]">
                    <span className="shrink-0"><WeatherIcon id={2} size={26} isDark={isDark} /></span>
                    <span className="text-sm font-semibold truncate">—</span>
                  </span>
                  <span className="text-xs font-bold tabular-nums text-[var(--text-secondary)] shrink-0">—</span>
                </div>
              ) : (
                <div key={i} className="flex-1 flex items-center justify-center gap-3 px-2 py-2 min-w-0">
                  <div className="shrink-0 text-[var(--text-secondary)]"><WeatherIcon id={2} size={32} isDark={isDark} /></div>
                  <div className="flex flex-col gap-0.5 min-w-0">
                    <span className="text-[9px] tabular-nums text-[var(--text-secondary)]">—</span>
                    <span className="text-[10px] text-[var(--text-secondary)] truncate">—</span>
                    <span className="text-[11px] font-bold tabular-nums text-[var(--text-secondary)]">—</span>
                  </div>
                </div>
              ))
              : forecast.map((s, i) => {
                const rainColor = s.rain_percentage > 50 ? '#5794F2'
                  : s.rain_percentage > 20 ? '#73BF69' : 'var(--text-secondary)'
                return weatherLevel === 4 ? (
                  <div key={i} className="flex-1 flex items-center justify-center gap-4 px-4 py-3 min-w-0">
                    <div className="shrink-0 text-[var(--text-secondary)]"><WeatherIcon id={s.weather} size={40} isDark={isDark} /></div>
                    <div className="flex flex-col gap-1 min-w-0">
                      <span className="text-[11px] font-bold tabular-nums text-[var(--text-secondary)]">+{s.time_offset}m</span>
                      <span className="text-xs font-bold text-[var(--text-secondary)] truncate">{WEATHER_LABELS[s.weather] ?? '—'}</span>
                      <span className="text-sm font-black tabular-nums" style={{ color: rainColor }}>{s.rain_percentage}%</span>
                    </div>
                  </div>
                ) : weatherLevel === 2 ? (
                  <div key={i} className="flex-1 flex items-center px-2 gap-2 min-w-0">
                    <span className="text-[9px] tabular-nums text-[var(--text-secondary)] shrink-0">+{s.time_offset}m</span>
                    <span className="flex-1 text-center text-xs font-semibold truncate" style={{ color: weatherColor(s.weather, isDark) }}>
                      {WEATHER_LABELS[s.weather] ?? '—'}
                    </span>
                    <span className="text-[10px] font-bold tabular-nums shrink-0" style={{ color: rainColor }}>
                      {s.rain_percentage}%
                    </span>
                  </div>
                ) : weatherLevel === 3 ? (
                  <div key={i} className="flex-1 flex items-center justify-between px-3 gap-2 min-w-0">
                    <div className="flex items-center gap-2 min-w-0">
                      <span className="shrink-0 flex items-center justify-center">
                        <WeatherIcon id={s.weather} size={18} isDark={isDark} />
                      </span>
                      <span className="text-xs font-semibold truncate text-[var(--text-primary)]">
                        {WEATHER_LABELS[s.weather] ?? '—'}
                      </span>
                    </div>
                    <div className="flex items-baseline gap-1.5 shrink-0">
                      <span className="text-[10px] font-medium tabular-nums text-[var(--text-secondary)]">+{s.time_offset}m</span>
                      <span className="text-xs font-bold tabular-nums" style={{ color: rainColor }}>
                        {s.rain_percentage}%
                      </span>
                    </div>
                  </div>
                ) : weatherLevel === 1 ? (
                  <div key={i} className="flex-1 flex items-center px-2 gap-1.5 min-w-0">
                    <span className="text-xs tabular-nums text-[var(--text-secondary)] shrink-0">+{s.time_offset}m</span>
                    <span className="flex flex-1 items-center justify-center gap-1.5 min-w-0 text-[var(--text-primary)]">
                      <span className="shrink-0"><WeatherIcon id={s.weather} size={26} isDark={isDark} /></span>
                      <span className="text-sm font-semibold truncate">{WEATHER_LABELS[s.weather] ?? '—'}</span>
                    </span>
                    <span className="text-xs font-bold tabular-nums shrink-0" style={{ color: rainColor }}>
                      {s.rain_percentage}%
                    </span>
                  </div>
                ) : (
                  <div key={i} className="flex-1 flex items-center justify-center gap-3 px-2 py-2 min-w-0">
                    <div className="shrink-0 text-[var(--text-secondary)]"><WeatherIcon id={s.weather} size={32} isDark={isDark} /></div>
                    <div className="flex flex-col gap-0.5 min-w-0">
                      <span className="text-[9px] tabular-nums text-[var(--text-secondary)]">+{s.time_offset}m</span>
                      <span className="text-[10px] text-[var(--text-secondary)] truncate">{WEATHER_LABELS[s.weather] ?? '—'}</span>
                      <span className="text-[11px] font-bold tabular-nums text-[var(--text-secondary)]">{s.rain_percentage}%</span>
                    </div>
                  </div>
                )
              })}
            </div>
          )}
        </div>
      )}

      {/* Col 2 — Proximity (content-height) + Events (fills remainder) */}
      {(layout.showProximity || layout.showEvents) && (
        <div
          className={`flex flex-col ${layout.showMap || layout.showWeather ? 'shrink-0' : 'flex-1'} min-w-0`}
          style={layout.showMap || layout.showWeather ? { width: `${layout.sidebarPct ?? 28}%` } : undefined}
        >

          {layout.showProximity && (
            <div className="shrink-0 border-b border-[var(--border)]">
              <div className={isCompactProximity
                ? "shrink-0 flex items-center border-b border-[var(--border)] h-[32px] px-3"
                : isSpaciousProximity
                ? "px-4 py-3 border-b border-[var(--border)] flex items-center"
                : "px-4 py-2 border-b border-[var(--border)]"
              }>
                <span className={isCompactProximity
                  ? "text-[10px] font-semibold uppercase tracking-wider text-[var(--text-secondary)] leading-none"
                  : isSpaciousProximity
                  ? "text-xs font-black uppercase tracking-wider text-[var(--text-secondary)]"
                  : "text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]"
                }>
                  Proximity
                </span>
              </div>
              {timing
                ? <ProximityWidget timing={timing} participants={participants} compact={compactProximity} />
                : <p className="text-xs text-[var(--text-secondary)] px-4 py-3">No timing data</p>
              }
            </div>
          )}

          {layout.showEvents && (
            <div className="flex flex-col flex-1 min-h-0">
              <div className={isCompactEvents
                ? "shrink-0 flex items-center border-b border-[var(--border)] h-[32px] px-3"
                : isSpaciousEvents
                ? "px-4 py-3 border-b border-[var(--border)] flex items-center"
                : "px-4 py-2 border-b border-[var(--border)]"
              }>
                <span className={isCompactEvents
                  ? "text-[10px] font-semibold uppercase tracking-wider text-[var(--text-secondary)] leading-none"
                  : isSpaciousEvents
                  ? "text-xs font-black uppercase tracking-wider text-[var(--text-secondary)]"
                  : "text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]"
                }>
                  Events
                </span>
              </div>
              <div ref={logRef} className="flex-1 overflow-y-auto">
                {visibleEvents.length === 0 ? (
                  <div className="flex items-center justify-center h-full text-[10px] text-[var(--text-secondary)]">
                    No events yet
                  </div>
                ) : (
                  <div className="flex flex-col">
                    {visibleEvents.map(({ event, fmt }, i) => (
                      <div
                        key={i}
                        className={isCompactEvents
                          ? "flex items-center justify-between gap-2 px-3 h-[32px] border-b border-[var(--border)] min-w-0"
                          : isSpaciousEvents
                          ? "flex flex-col gap-1 px-4 py-3 border-b border-[var(--border)]"
                          : "flex flex-col gap-0.5 px-3 py-1.5 border-b border-[var(--border)]"
                        }
                      >
                        {isCompactEvents ? (
                          <>
                            <span
                              className="text-[11px] font-bold leading-tight truncate min-w-0 flex-1"
                              style={{ color: fmt.color }}
                            >
                              {fmt.label}
                            </span>
                            <span
                              className="text-[9px] tabular-nums font-mono font-semibold shrink-0"
                              style={{ color: `${fmt.color}aa` }}
                            >
                              {fmtSessionTime(event.session_time)}
                            </span>
                          </>
                        ) : isSpaciousEvents ? (
                          <>
                            <span
                              className="text-xs tabular-nums font-mono font-bold"
                              style={{ color: `${fmt.color}cc` }}
                            >
                              {fmtSessionTime(event.session_time)}
                            </span>
                            <span
                              className="text-sm font-black leading-snug"
                              style={{ color: fmt.color }}
                            >
                              {fmt.label}
                            </span>
                          </>
                        ) : (
                          <>
                            <span
                              className="text-[9px] tabular-nums font-mono font-semibold"
                              style={{ color: `${fmt.color}aa` }}
                            >
                              {fmtSessionTime(event.session_time)}
                            </span>
                            <span
                              className="text-[11px] font-bold leading-snug"
                              style={{ color: fmt.color }}
                            >
                              {fmt.label}
                            </span>
                          </>
                        )}
                      </div>
                    ))}
                  </div>
                )}
              </div>
            </div>
          )}

        </div>
      )}

    </div>
  )}

</div>
  )
})
export default SessionPanel
