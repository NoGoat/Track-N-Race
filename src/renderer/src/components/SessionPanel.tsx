import { useRef, useState, memo } from 'react'
import { Sun, CloudSun, Cloud, CloudDrizzle, CloudRain, CloudLightning, type LucideIcon } from 'lucide-react'
import type { SessionMsg, RaceEventMsg, TimingMsg, ParticipantsMsg, TimingCar } from '../types'
import TrackMap from './TrackMap'
import { useColorFn } from '../lib/cards'

// ─── Lookup tables ───────────────────────────────────────────────────────────

const TRACK_INFO: Record<number, { gp: string; circuit: string }> = {
  0:  { gp: 'Australian Grand Prix',      circuit: 'Albert Park Circuit' },
  2:  { gp: 'Chinese Grand Prix',         circuit: 'Shanghai International Circuit' },
  3:  { gp: 'Bahrain Grand Prix',         circuit: 'Bahrain International Circuit' },
  4:  { gp: 'Spanish Grand Prix',         circuit: 'Circuit de Barcelona-Catalunya' },
  5:  { gp: 'Monaco Grand Prix',          circuit: 'Circuit de Monaco' },
  6:  { gp: 'Canadian Grand Prix',        circuit: 'Circuit Gilles Villeneuve' },
  7:  { gp: 'British Grand Prix',         circuit: 'Silverstone Circuit' },
  9:  { gp: 'Hungarian Grand Prix',       circuit: 'Hungaroring' },
  10: { gp: 'Belgian Grand Prix',         circuit: 'Circuit de Spa-Francorchamps' },
  11: { gp: 'Italian Grand Prix',         circuit: 'Autodromo Nazionale Monza' },
  12: { gp: 'Singapore Grand Prix',       circuit: 'Marina Bay Street Circuit' },
  13: { gp: 'Japanese Grand Prix',        circuit: 'Suzuka International Racing Course' },
  14: { gp: 'Abu Dhabi Grand Prix',       circuit: 'Yas Marina Circuit' },
  15: { gp: 'United States Grand Prix',   circuit: 'Circuit of the Americas' },
  16: { gp: 'São Paulo Grand Prix',       circuit: 'Autódromo José Carlos Pace' },
  17: { gp: 'Austrian Grand Prix',        circuit: 'Red Bull Ring' },
  19: { gp: 'Mexico City Grand Prix',     circuit: 'Autódromo Hermanos Rodríguez' },
  20: { gp: 'Azerbaijan Grand Prix',      circuit: 'Baku City Circuit' },
  26: { gp: 'Dutch Grand Prix',           circuit: 'Circuit Zandvoort' },
  27: { gp: 'Emilia Romagna Grand Prix',  circuit: 'Autodromo Enzo e Dino Ferrari' },
  29: { gp: 'Saudi Arabian Grand Prix',   circuit: 'Jeddah Corniche Circuit' },
  30: { gp: 'Miami Grand Prix',           circuit: 'Miami International Autodrome' },
  31: { gp: 'Las Vegas Grand Prix',       circuit: 'Las Vegas Street Circuit' },
  32: { gp: 'Qatar Grand Prix',           circuit: 'Losail International Circuit' },
  39: { gp: 'British Grand Prix',         circuit: 'Silverstone Circuit (Reverse)' },
  40: { gp: 'Austrian Grand Prix',        circuit: 'Red Bull Ring (Reverse)' },
  41: { gp: 'Dutch Grand Prix',           circuit: 'Circuit Zandvoort (Reverse)' },
}

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
  if (t >= 1  && t <= 4)  return isDark ? '#FB923C' : '#C2660A'
  if (t >= 5  && t <= 14) return isDark ? '#ffd700' : '#B7950B'
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

const INFRINGEMENT_LABELS: Record<number, string> = {
  0: 'Blocking by slowing', 1: 'Blocking wrong way', 2: 'Reversing off start',
  3: 'Big collision', 4: 'Small collision', 7: 'SC delta exceeded',
  8: 'SC illegal overtake', 9: 'SC exceeding pace', 13: 'Pit lane too fast',
  17: 'Pit lane speeding', 25: 'Corner cutting', 30: 'Lap invalidated',
}

function formatEvent(ev: RaceEventMsg, p: ParticipantsMsg | null, isDark: boolean): { label: string; color: string } | null {
  const name = (idx?: number) => driverName(p, idx ?? 0)
  const fmtLap = (s: number) => `${Math.floor(s / 60)}:${(s % 60).toFixed(3).padStart(6, '0')}`
  switch (ev.code) {
    case 'FTLP': return { label: `Fastest Lap — ${name(ev.car_idx)}  ${fmtLap(ev.lap_time_s ?? 0)}`, color: isDark ? '#BF5FFF' : '#9B5DE5' }
    case 'DRSE': return { label: 'DRS Enabled', color: isDark ? '#37872D' : '#137333' }
    case 'DRSD': return { label: 'DRS Disabled', color: isDark ? '#6e7177' : '#565B70' }
    case 'RDFL': return { label: 'Red Flag', color: '#e10600' }
    case 'CHQF': return { label: 'Chequered Flag', color: isDark ? '#a0a8b8' : '#565B70' }
    case 'LGOT': return { label: 'Lights Out', color: isDark ? '#37872D' : '#137333' }
    case 'SSTA': return { label: 'Session Start', color: isDark ? '#5794F2' : '#0B57D0' }
    case 'SEND': return { label: 'Session End', color: isDark ? '#5794F2' : '#0B57D0' }
    case 'RTMT': return { label: `Retired — ${name(ev.car_idx)}`, color: isDark ? '#a0a8b8' : '#565B70' }
    case 'RCWN': return { label: `Race Winner — ${name(ev.car_idx)}`, color: isDark ? '#FFD700' : '#B7950B' }
    case 'DTSV': return { label: `DT Served — ${name(ev.car_idx)}`, color: isDark ? '#a0a8b8' : '#565B70' }
    case 'SGSV': return { label: `SG Served — ${name(ev.car_idx)}`, color: isDark ? '#a0a8b8' : '#565B70' }
    case 'SCAR': {
      const T: Record<number, string> = { 1: 'Safety Car', 2: 'Virtual SC', 3: 'Formation Lap' }
      const A: Record<number, string> = { 0: 'Deployed', 1: 'Returning', 2: 'Returned', 3: 'Resume Race' }
      return { label: `${T[ev.safety_car_type ?? 0] ?? 'SC'} — ${A[ev.event_type ?? 0] ?? ''}`, color: isDark ? '#ffd700' : '#B7950B' }
    }
    case 'PENA': {
      const pt = ev.penalty_type ?? 0
      const L: Record<number, string> = { 0: 'Drive Through', 1: 'Stop-Go', 2: 'Grid Penalty', 4: 'Time Penalty', 5: 'Warning', 6: 'DSQ' }
      const C: Record<number, string> = isDark
        ? { 0: '#e10600', 1: '#e10600', 2: '#c47d0e', 4: '#c47d0e', 5: '#ffd700', 6: '#e10600' }
        : { 0: '#C4162A', 1: '#C4162A', 2: '#C26400', 4: '#C26400', 5: '#B7950B', 6: '#C4162A' }
      if (!(pt in L)) return null
      const ts = (pt === 1 || pt === 4) && ev.penalty_time_s ? ` ${ev.penalty_time_s}s` : ''
      const inf = ev.infringement_type != null ? INFRINGEMENT_LABELS[ev.infringement_type] : undefined
      return { label: `${L[pt]}${ts} — ${name(ev.car_idx)}${inf ? ` — ${inf}` : ''}`, color: C[pt] }
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

const StatCard = memo(function StatCard({ label, value, unit, accent, compact }: { label: string; value: string; unit?: string; accent?: string; compact?: boolean }) {
  if (compact) {
    // Single-line: label · value+unit — the row collapses to one line.
    return (
      <div className="flex-1 flex items-center justify-between gap-2 px-4 py-2 border-r border-[var(--border)] last:border-r-0">
        <span className="text-[10px] font-medium tracking-widest uppercase text-[var(--text-secondary)] truncate">{label}</span>
        <span className="flex items-baseline gap-1 shrink-0">
          <span className="text-lg font-black tabular-nums leading-none" style={{ color: accent ?? 'var(--text-primary)' }}>{value}</span>
          {unit && <span className="text-[10px] text-[var(--text-secondary)]">{unit}</span>}
        </span>
      </div>
    )
  }
  return (
    <div className="flex-1 flex flex-col justify-between px-5 py-4 border-r border-[var(--border)] last:border-r-0">
      <span className="text-[10px] font-medium tracking-widest uppercase text-[var(--text-secondary)]">{label}</span>
      <div>
        <div className="text-3xl font-black tabular-nums leading-none mt-2" style={{ color: accent ?? 'var(--text-primary)' }}>
          {value}
        </div>
        {unit && <div className="text-xs text-[var(--text-secondary)] mt-1">{unit}</div>}
      </div>
    </div>
  )
})

function areProximityPropsEqual(prev: any, next: any) {
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

const ProximityWidget = memo(function ProximityWidget({ timing, participants }: {
  timing: TimingMsg; participants: ParticipantsMsg | null
}) {
  const active = timing.cars
    .filter(c => c.result_status === 2 && c.position > 0)
    .sort((a, b) => a.position - b.position)

  const player = active.find(c => c.idx === timing.player_idx)
  if (!player) return <p className="text-xs text-[var(--text-secondary)] py-2">No position data</p>

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
        const livery = participants?.drivers.find(d => d.idx === car.idx)?.livery_color ?? '#8e8e8e'
        return (
          <div
            key={car.idx}
            className="flex items-center gap-3 px-3 py-3"
          >
            <span
              className="text-[10px] font-bold tabular-nums w-7 text-center py-0.5 rounded shrink-0"
              style={{ backgroundColor: livery + '28', color: livery }}
            >
              P{car.position}
            </span>
            <span className={`text-sm font-bold flex-1 truncate ${isPlayer ? 'text-[var(--text-primary)]' : 'text-[var(--text-secondary)]'}`}>
              {driverName(participants, car.idx)}
            </span>
            {deltaMs != null && deltaMs > 0 && (
              <span className="text-xs font-semibold tabular-nums" style={{ color: sign === 'ahead' ? '#73BF69' : '#6e7177' }}>
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
  compactHeader?: boolean
  compactCards?: boolean
  compactWeather?: boolean
}

const SessionPanel = memo(function SessionPanel({ session, raceEvents, timing, participants, isDark, sectorColors, driversMode, mapTimeout, reduceAnimations, mapDimmed = false, aeroMode, compactHeader, compactCards, compactWeather }: Props) {
  const logRef = useRef<HTMLDivElement>(null)
  const [mapFullscreen, setMapFullscreen] = useState(false)

  const noData  = !session
  const colorFn = useColorFn(null, null, isDark)
  const accent  = session ? sessionAccent(session.session_type, isDark) : (isDark ? '#a0a8b8' : '#565B70')
  const info    = session ? TRACK_INFO[session.track_id] : null
  const gpName  = info?.gp ?? (session ? `Track ${session.track_id}` : '—')
  const circuit = info?.circuit ?? ''
  const sType   = session ? SESSION_TYPES[session.session_type] ?? 'Unknown' : null

  const isSpecialMode = session
    ? ((session.session_type >= 1 && session.session_type <= 14) || session.session_type === 18)
    : false

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
    .map(e => ({ event: e, fmt: formatEvent(e, participants, isDark) }))
    .filter(({ fmt }) => fmt !== null)
    .reverse() as { event: RaceEventMsg; fmt: { label: string; color: string } }[]

  const conditionCards = [
    ...(isSpecialMode ? [
      {
        label: 'Total Laps',
        value: session && session.total_laps > 0 ? String(session.total_laps) : '—',
        color: undefined as string | undefined,
        sub: undefined as string | undefined,
      },
      {
        label: 'Pit Speed',
        value: session ? `${session.pit_speed_limit} km/h` : '—',
        color: noData ? undefined : colorFn('session.pitSpeed'),
        sub: 'Limit',
      }
    ] : []),
    {
      label: 'Track Temp',
      value: session ? `${session.track_temp}°C` : '—',
      color: session ? colorFn('session.trackTemp', session.track_temp) : undefined as string | undefined,
      sub: 'Road surface',
    },
    {
      label: 'Air Temp',
      value: session ? `${session.air_temp}°C` : '—',
      color: session ? colorFn('session.airTemp', session.air_temp) : undefined as string | undefined,
      sub: 'Ambient',
    },
    {
      label: 'Track Length',
      value: session ? `${(session.track_length_m / 1000).toFixed(3)} km` : '—',
      color: undefined as string | undefined,
      sub: undefined as string | undefined,
    },
    {
      label: 'Time of Day',
      value: session ? fmtTimeOfDay(session.time_of_day) : '—',
      color: undefined as string | undefined,
      sub: undefined as string | undefined,
    },
  ]

  return (
    <div className="flex flex-col h-full overflow-hidden">

      {/* ── Map fullscreen ── */}
      {mapFullscreen && (
        <div className="flex-1 min-h-0">
          <TrackMap
            trackId={session?.track_id ?? null}
            participants={participants}
            isDark={isDark}
            sectorColors={sectorColors}
            driversMode={driversMode}
            mapTimeout={mapTimeout}
            reduceAnimations={reduceAnimations}
            mapDimmed={mapDimmed}
            aeroMode={aeroMode}
            slmTrackStatus={session?.active_aero_track_status ?? -1}
            isFullscreen
            onToggleFullscreen={() => setMapFullscreen(false)}
          />
        </div>
      )}

      {/* ── Normal layout ── */}
      {!mapFullscreen && <>

      {/* ── Header ── */}
      <div
        className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]"
      >
        {/* GP name */}
        <div className={`flex flex-col justify-center shrink-0 px-6 ${compactHeader ? 'py-2' : 'py-4'}`}>
          <div className={`flex items-center gap-3 ${compactHeader ? '' : 'mb-0.5'}`}>
            <h1 className={`${compactHeader ? 'text-base' : 'text-xl'} font-black tracking-tight ${noData ? 'text-[var(--text-secondary)]' : 'text-[var(--text-primary)]'}`}>
              {gpName}
            </h1>
          </div>
          {circuit && !compactHeader && <p className="text-xs text-[var(--text-secondary)]">{circuit}</p>}
        </div>

        {/* Zones strip. Compact: ZONES · marshal strip · legend all on one row. */}
        <div className={`flex flex-col justify-center flex-1 min-w-0 px-6 ${compactHeader ? 'py-2' : 'py-4'}`}>
          {(() => {
            const legend = (
              <div className="flex items-center gap-4 shrink-0">
                {[{ c: '#fdd835', l: 'Yellow' }, { c: '#00c853', l: 'Green' }, { c: '#2196f3', l: 'Blue' }, { c: isDark ? 'rgba(255,255,255,0.07)' : 'rgba(0,0,0,0.08)', l: 'Clear' }].map(({ c, l }) => (
                  <div key={l} className="flex items-center gap-1.5">
                    <div
                      className="w-3 h-3 rounded-sm"
                      style={{
                        backgroundColor: c,
                        border: `1px solid ${isDark ? 'rgba(255,255,255,0.1)' : 'rgba(0,0,0,0.15)'}`
                      }}
                    />
                    <span className="text-[10px] text-[var(--text-secondary)]">{l}</span>
                  </div>
                ))}
              </div>
            )
            const zonesLbl = <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)] shrink-0">Zones</span>
            return compactHeader ? (
              <div className="flex items-center gap-3">
                {zonesLbl}
                <div className="flex-1 min-w-0"><MarshalStrip zones={session?.marshal_zones ?? []} isDark={isDark} /></div>
                {legend}
              </div>
            ) : (
              <>
                <div className="flex items-center justify-between mb-2">
                  {zonesLbl}
                  {legend}
                </div>
                <MarshalStrip zones={session?.marshal_zones ?? []} isDark={isDark} />
              </>
            )
          })()}
        </div>

        {/* Time left */}
        <div className={`flex flex-col justify-center items-end shrink-0 px-6 ${compactHeader ? 'py-2' : 'py-4'}`}>
          <div className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Time Left</div>
          <div
            className={`${compactHeader ? 'text-xl' : 'text-3xl'} font-black tabular-nums leading-tight`}
            style={{ color: noData ? 'var(--text-secondary)' : 'var(--text-primary)' }}
          >
            {session ? fmtTimeLeft(session.session_time_left) : '--:--'}
          </div>
        </div>
      </div>

      {/* ── Stat cards ── */}
      {!isSpecialMode && (
        <div className="shrink-0 flex border-b border-[var(--border)]">
          <StatCard label="Total Laps" value={session && session.total_laps > 0 ? String(session.total_laps) : '—'} compact={compactCards} />
          {remainingLaps !== null
            ? <StatCard label="Remaining" value={String(remainingLaps)} compact={compactCards} />
            : <StatCard label="Remaining" value="—" compact={compactCards} />
          }
          <StatCard label="Pit Speed"  value={session ? String(session.pit_speed_limit) : '—'} unit={session ? 'km/h' : undefined} accent={noData ? undefined : colorFn('session.pitSpeed')} compact={compactCards} />
          <StatCard label="Pit Window" value={pitWindow} accent={noData ? undefined : colorFn('session.pitWindow')} compact={compactCards} />
          <StatCard label="Rejoin"     value={rejoinPos} accent={noData ? undefined : colorFn('session.rejoin')} compact={compactCards} />
        </div>
      )}

      {/* ── Main body — three columns that all fill the available height ── */}
      <div className="flex flex-1 min-h-0 divide-x divide-[var(--border)]">

        {/* Col 1 — Weather: empty top (reserved), Now+forecast strip at bottom */}
        <div className="flex flex-col flex-1 min-w-0 divide-y divide-[var(--border)]">

          {/* Track map */}
          <div className="flex-1 min-h-0">
            <TrackMap trackId={session?.track_id ?? null} participants={participants} isDark={isDark} sectorColors={sectorColors}
            driversMode={driversMode} mapTimeout={mapTimeout} reduceAnimations={reduceAnimations} mapDimmed={mapDimmed} aeroMode={aeroMode} slmTrackStatus={session?.active_aero_track_status ?? -1} isFullscreen={false} onToggleFullscreen={() => setMapFullscreen(true)} />
          </div>

          {/* Now + forecast strip — pinned to bottom. Compact: each cell is a single
              horizontal row (time · weather · rain %), matching native's compact strip. */}
          <div className="flex shrink-0 divide-x divide-[var(--border)]" style={{ height: compactWeather ? 32 : 110 }}>

            {/* Now card */}
            {compactWeather ? (
              <div className="flex items-center w-44 shrink-0 px-4 gap-2">
                <span className="text-[9px] font-medium uppercase tracking-widest text-[var(--text-secondary)] shrink-0">Now</span>
                <span className="flex-1 text-center text-xs font-semibold truncate" style={{ color: session ? weatherColor(session.weather, isDark) : 'var(--text-secondary)' }}>
                  {session ? WEATHER_LABELS[session.weather] ?? '—' : '—'}
                </span>
              </div>
            ) : (
              <div className="flex flex-col items-center justify-evenly w-44 shrink-0 px-4">
                <span className="text-[9px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Now</span>
                <div style={{ color: noData ? 'var(--text-secondary)' : 'var(--text-primary)' }}>
                  <WeatherIcon id={session?.weather ?? 2} size={28} isDark={isDark} />
                </div>
                <span className={`text-sm font-semibold text-center leading-tight ${noData ? 'text-[var(--text-secondary)]' : 'text-[var(--text-primary)]'}`}>
                  {session ? WEATHER_LABELS[session.weather] ?? '—' : '—'}
                </span>
                <span className="text-[9px] text-[var(--text-secondary)]">
                  {session ? (session.forecast_accuracy === 0 ? 'Exact' : 'Approx') : '—'}
                </span>
              </div>
            )}

            {/* Forecast cards — skeleton when no data */}
            {noData
              ? [1, 2, 3, 4, 5].map(i => compactWeather ? (
                <div key={i} className="flex-1 flex items-center px-2 gap-2 min-w-0">
                  <span className="text-[9px] tabular-nums text-[var(--text-secondary)] shrink-0">—</span>
                  <span className="flex-1 text-center text-xs text-[var(--text-secondary)]">—</span>
                  <span className="text-[10px] font-bold tabular-nums text-[var(--text-secondary)] shrink-0">—</span>
                </div>
              ) : (
                <div key={i} className="flex-1 flex flex-col items-center justify-evenly px-2 py-2">
                  <span className="text-[9px] tabular-nums text-[var(--text-secondary)]">—</span>
                  <div className="text-[var(--text-secondary)]"><WeatherIcon id={2} size={20} isDark={isDark} /></div>
                  <span className="text-[10px] text-[var(--text-secondary)] text-center">—</span>
                  <span className="text-[11px] font-bold tabular-nums text-[var(--text-secondary)]">—</span>
                </div>
              ))
              : forecast.map((s, i) => {
                const rainColor = s.rain_percentage > 50 ? '#5794F2'
                  : s.rain_percentage > 20 ? '#73BF69' : 'var(--text-secondary)'
                return compactWeather ? (
                  <div key={i} className="flex-1 flex items-center px-2 gap-2 min-w-0">
                    <span className="text-[9px] tabular-nums text-[var(--text-secondary)] shrink-0">+{s.time_offset}m</span>
                    <span className="flex-1 text-center text-xs font-semibold truncate" style={{ color: weatherColor(s.weather, isDark) }}>
                      {WEATHER_LABELS[s.weather] ?? '—'}
                    </span>
                    <span className="text-[10px] font-bold tabular-nums shrink-0" style={{ color: rainColor }}>
                      {s.rain_percentage}%
                    </span>
                  </div>
                ) : (
                  <div key={i} className="flex-1 flex flex-col items-center justify-evenly px-2 py-2">
                    <span className="text-[9px] tabular-nums text-[var(--text-secondary)]">+{s.time_offset}m</span>
                    <div className="text-[var(--text-primary)]"><WeatherIcon id={s.weather} size={20} isDark={isDark} /></div>
                    <span className="text-[10px] text-[var(--text-primary)] text-center leading-tight">
                      {WEATHER_LABELS[s.weather] ?? '—'}
                    </span>
                    <span className="text-[11px] font-bold tabular-nums" style={{ color: rainColor }}>
                      {s.rain_percentage}%
                    </span>
                  </div>
                )
              })
            }
          </div>
        </div>

        {/* Col 2 — Track conditions: 4 stacked cards, each fills equal height */}
        <div className="flex flex-col w-52 shrink-0 divide-y divide-[var(--border)]">
          {conditionCards.map(({ label, value, color, sub }) => (
            <div key={label} className="flex flex-col flex-1 justify-center px-5 gap-1">
              <span className="text-[9px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">
                {label}
              </span>
              <span
                className="text-2xl font-black tabular-nums leading-tight"
                style={{ color: noData ? 'var(--text-secondary)' : color ?? 'var(--text-primary)' }}
              >
                {value}
              </span>
              {sub && !noData && <span className="text-[10px] text-[var(--text-secondary)]">{sub}</span>}
            </div>
          ))}
        </div>

        {/* Col 3 — Proximity (content-height) + Events (fills remainder) */}
        <div className="flex flex-col w-72 shrink-0">

          <div className="shrink-0 border-b border-[var(--border)]">
            <div className="px-4 py-2 border-b border-[var(--border)]">
              <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">
                Proximity
              </span>
            </div>
            {timing
              ? <ProximityWidget timing={timing} participants={participants} />
              : <p className="text-xs text-[var(--text-secondary)] px-4 py-3">No timing data</p>
            }
          </div>

          <div className="flex flex-col flex-1 min-h-0">
            <div className="shrink-0 px-4 py-2 border-b border-[var(--border)]">
              <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">
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
                      className="flex flex-col gap-0.5 px-3 py-1.5 border-b border-[var(--border)]"
                    >
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
                    </div>
                  ))}
                </div>
              )}
            </div>
          </div>

        </div>
      </div>

      </>}
    </div>
  )
})
export default SessionPanel
