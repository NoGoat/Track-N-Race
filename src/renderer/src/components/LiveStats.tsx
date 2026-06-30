import { memo } from 'react'
import type { TelemetryRow, StatusRow, LapRow, DamageRow } from '../types'
import { useLabels } from '../lib/labels'

// Visual compound codes: always Soft/Med/Hard regardless of which C-number this weekend
const VISUAL_COLORS: Record<number, string> = {
  16: 'text-[var(--compound-soft)]',
  17: 'text-[var(--compound-medium)]',
  18: 'text-[var(--compound-hard)]',
   7: 'text-[var(--compound-inter)]',
   8: 'text-[var(--compound-wet)]',
}

const FUEL_MIX  = ['Lean', 'Std', 'Rich', 'Max']

interface VisibleCards {
  speed: boolean; rpm: boolean; gear: boolean; throttle: boolean; brake: boolean
  drs: boolean; engine: boolean; ers: boolean; fuel: boolean; pos: boolean; tyre: boolean
}

interface Props {
  latest: TelemetryRow | null
  status: StatusRow | null
  lap: LapRow | null
  damage: DamageRow | null
  isConnected: boolean
  visibleCards: VisibleCards
  isDark: boolean
}

const Card = memo(function Card({
  label,
  value,
  unit,
  color,
  textColor,
  sub,
  subColor,
  subTextColor,
}: {
  label: string
  value: string
  unit?: string
  color?: string
  textColor?: string
  sub?: string
  subColor?: string
  subTextColor?: string
}) {
  return (
    <div className="flex-1 min-w-0 px-3 py-2 min-h-[75px]">
      <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-widest mb-0.5">{label}</div>
      <div className="flex items-baseline gap-0.5 overflow-hidden">
        <span
          className={`text-xl font-bold tabular-nums truncate ${color ?? ''}`}
          style={textColor ? { color: textColor } : undefined}
        >
          {value}
        </span>
        {unit && <span className="text-[10px] text-[var(--text-secondary)] shrink-0">{unit}</span>}
      </div>
      {sub && (
        <div
          className={`text-[9px] mt-0.5 truncate font-semibold ${subColor ?? ''}`}
          style={subTextColor ? { color: subTextColor } : undefined}
        >
          {sub}
        </div>
      )}
    </div>
  )
})

const LiveStats = memo(function LiveStats({ latest, status, lap, damage, isConnected, visibleCards, isDark }: Props) {
  const { t, tn } = useLabels()
  const green  = isDark ? '#37872D' : '#137333'
  const red    = '#C4162A'
  const blue   = isDark ? '#5794F2' : '#0B57D0'
  const yellow = isDark ? '#d4ad04' : '#B06000'
  const gray   = isDark ? 'var(--text-secondary)' : '#565B70'

  if (!isConnected || !latest) {
    const placeholders = [
      { key: 'speed', label: 'Speed' }, { key: 'rpm', label: 'RPM' }, { key: 'gear', label: 'Gear' },
      { key: 'throttle', label: 'Throttle' }, { key: 'brake', label: 'Brake' }, { key: 'drs', label: t('ui.overview.drs') },
      { key: 'engine', label: 'Engine' }, { key: 'ers', label: 'ERS' }, { key: 'fuel', label: 'Fuel' },
      { key: 'pos', label: 'P' }, { key: 'tyre', label: 'Tyre' },
    ] as { key: keyof VisibleCards; label: string }[]
    return (
      <div className="flex divide-x divide-[var(--border)]">
        {placeholders.filter(p => visibleCards[p.key]).map(p => <Card key={p.key} label={p.label} value="—" />)}
      </div>
    )
  }

  const { speed_kph, rpm, gear, throttle, brake, drs, engine_temp } = latest

  const gearLabel = gear === 0 ? 'N' : gear < 0 ? 'R' : String(gear)
  const gearColor =
    gear <= 2 ? blue :
    gear <= 4 ? yellow :
    gear <= 6 ? (isDark ? '#c47d0e' : '#C26400') :
    red

  const tyreName  = status ? tn('tyre.actual', status.tyre_compound) : null
  const tyreColor = status ? (VISUAL_COLORS[status.visual_compound] ?? 'text-[var(--text-primary)]') : undefined

  return (
    <div className="flex divide-x divide-[var(--border)]">
      {visibleCards.speed    && <Card label="Speed"    value={String(speed_kph)} unit="kph" textColor={green} />}
      {visibleCards.rpm      && <Card label="RPM"      value={rpm.toLocaleString()} textColor={red} />}
      {visibleCards.gear     && <Card label="Gear"     value={gearLabel} textColor={gearColor} />}
      {visibleCards.throttle && <Card label="Throttle" value={String(Math.round(throttle * 100))} unit="%" textColor={green} />}
      {visibleCards.brake    && <Card label="Brake"    value={String(Math.round(brake * 100))} unit="%" textColor={brake > 0.05 ? red : undefined} />}
      {visibleCards.drs      && (
        <Card label={t('ui.overview.drs')}
          value={drs ? 'ON' : 'OFF'}
          textColor={drs ? green : gray}
          sub={damage?.drs_fault === 1 ? 'FAULT' : undefined}
          subTextColor={red}
        />
      )}
      {visibleCards.engine   && <Card label="Engine"   value={String(engine_temp)} unit="°C" textColor={engine_temp > 112 ? red : undefined} />}
      {visibleCards.ers      && (
        <Card label="ERS"
          value={status ? `${status.ers_pct.toFixed(0)}` : '—'}
          unit={status ? '%' : undefined}
          textColor={!status ? undefined : status.ers_mode === 3 ? red : status.ers_pct < 20 ? yellow : blue}
          sub={damage?.ers_fault === 1 ? 'FAULT' : status ? tn('ers.mode', status.ers_mode) : undefined}
          subTextColor={damage?.ers_fault === 1 ? red : undefined}
        />
      )}
      {visibleCards.fuel && (
        <Card label="Fuel"
          value={status ? status.fuel_kg.toFixed(1) : '—'}
          unit={status ? 'kg' : undefined}
          textColor={!status ? undefined : status.fuel_laps > 1 ? green : status.fuel_laps >= 0 ? yellow : red}
          sub={status ? `${status.fuel_laps >= 0 ? '+' : ''}${status.fuel_laps.toFixed(1)} vs fin` : undefined}
        />
      )}
      {visibleCards.pos  && <Card label="Pos"  value={lap ? `P${lap.position}` : '—'} sub={lap ? `Lap ${lap.lap_num}` : undefined} />}
      {visibleCards.tyre && <Card label="Tyre" value={tyreName ?? '—'} color={tyreColor} sub={status ? `${status.tyre_age_laps}L · ${FUEL_MIX[status.fuel_mix] ?? ''}` : undefined} />}
    </div>
  )
})
export default LiveStats
