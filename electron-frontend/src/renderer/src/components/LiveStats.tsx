import { memo } from 'react'
import type { TelemetryRow, StatusRow, LapRow, DamageRow } from '../types'
import { useLabels } from '../lib/labels'
import { OVERVIEW_RESOLVERS, useColorFn, type CardCtx, type CardDesc } from '../lib/cards'

import type { DensityMode } from '../lib/graphSections'

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
  compact?: DensityMode | boolean
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
  compact,
}: {
  label: string
  value: string
  unit?: string
  color?: string
  textColor?: string
  sub?: string
  subColor?: string
  subTextColor?: string
  compact?: DensityMode | boolean
}) {
  const isCompact = compact === true || compact === 'compact'
  const isSpacious = compact === 'spacious'

  if (isCompact) {
    // Single-line: label · value+unit · sub — trades vertical space for a short row.
    return (
      <div className="flex-1 min-w-0 px-3 py-1.5 flex items-center justify-between gap-2">
        <span className="text-[9px] text-[var(--text-secondary)] uppercase tracking-widest truncate">{label}</span>
        <span className="flex items-baseline gap-0.5 shrink-0">
          <span
            className={`text-sm font-bold tabular-nums ${color ?? ''}`}
            style={textColor ? { color: textColor } : undefined}
          >
            {value}
          </span>
          {unit && <span className="text-[9px] text-[var(--text-secondary)]">{unit}</span>}
          {sub && (
            <span
              className={`text-[9px] ml-1 font-semibold ${subColor ?? ''}`}
              style={subTextColor ? { color: subTextColor } : undefined}
            >
              {sub}
            </span>
          )}
        </span>
      </div>
    )
  }

  if (isSpacious) {
    return (
      <div className="flex-1 min-w-0 px-4 py-3 min-h-[96px]">
        <div className="text-[11px] font-bold text-[var(--text-secondary)] uppercase tracking-wider mb-1">{label}</div>
        <div className="flex items-baseline gap-1 overflow-hidden">
          <span
            className={`text-3xl font-black tabular-nums truncate ${color ?? ''}`}
            style={textColor ? { color: textColor } : undefined}
          >
            {value}
          </span>
          {unit && <span className="text-xs font-semibold text-[var(--text-secondary)] shrink-0">{unit}</span>}
        </div>
        {sub && (
          <div
            className={`text-xs mt-1 truncate font-semibold ${subColor ?? ''}`}
            style={subTextColor ? { color: subTextColor } : undefined}
          >
            {sub}
          </div>
        )}
      </div>
    )
  }

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

const LiveStats = memo(function LiveStats({ latest, status, lap, damage, isConnected, visibleCards, isDark, compact }: Props) {
  const { t, tn } = useLabels()
  const color = useColorFn(latest, status, isDark)

  // Card definitions: { key (resolver/data), label (catalog), vis (visibility flag) }.
  // The wing card's data key is format-aware (drs ↔ slm) while it stays under the
  // 'drs' visibility toggle.
  const descs: (CardDesc & { vis: keyof VisibleCards })[] = [
    { key: 'speed',    label: t('ui.overview.speed'),    vis: 'speed' },
    { key: 'rpm',      label: t('ui.overview.rpm'),      vis: 'rpm' },
    { key: 'gear',     label: t('ui.overview.gear'),     vis: 'gear' },
    { key: 'throttle', label: t('ui.overview.throttle'), vis: 'throttle' },
    { key: 'brake',    label: t('ui.overview.brake'),    vis: 'brake' },
    { key: t('card.wing.key'), label: t('ui.overview.drs'), vis: 'drs' },
    { key: 'engine',   label: t('ui.overview.engine'),   vis: 'engine' },
    { key: 'ers',      label: t('ui.overview.ers'),      vis: 'ers' },
    { key: 'fuel',     label: t('ui.overview.fuel'),     vis: 'fuel' },
    { key: 'pos',      label: t('ui.overview.pos'),      vis: 'pos' },
    { key: 'tyre',     label: t('ui.overview.tyre'),     vis: 'tyre' },
  ]
  const shown = descs.filter(d => visibleCards[d.vis])

  if (!isConnected || !latest) {
    return (
      <div className="flex divide-x divide-[var(--border)]">
        {shown.map(d => <Card key={d.vis} label={d.label} value="—" compact={compact} />)}
      </div>
    )
  }

  const ctx: CardCtx = { latest, status, lap, damage, session: null, isDark, t, tn, color }
  return (
    <div className="flex divide-x divide-[var(--border)]">
      {shown.map(d => {
        const v = OVERVIEW_RESOLVERS[d.key]?.(ctx) ?? { value: '—' }
        let sub = v.sub
        const isSpacious = compact === 'spacious'
        const isCompact = compact === true || compact === 'compact'
        if (isCompact && sub) {
          if (d.key === 'ers') sub = sub.replace('Overtake', 'OT')
          else if (d.key === 'fuel') sub = sub.replace(' vs fin', '')
        } else if (isSpacious) {
          if (d.key === 'ers' && status) {
            sub = damage?.ers_fault === 1 ? 'FAULT' : `${tn('ers.mode', status.ers_mode)} · ${(status.ers_j / 1_000_000).toFixed(2)} MJ`
          } else if (d.key === 'fuel' && status) {
            sub = `${status.fuel_laps >= 0 ? '+' : ''}${status.fuel_laps.toFixed(1)} laps vs finish`
          } else if (d.key === 'tyre' && status) {
            const mix = ['Lean', 'Std', 'Rich', 'Max'][status.fuel_mix] ?? ''
            sub = `${status.tyre_age_laps}L age${mix ? ` · ${mix} mix` : ''}`
          } else if (d.key === 'pos' && lap) {
            sub = `Lap ${lap.lap_num}`
          } else if ((d.key === 'drs' || d.key === 'slm') && latest) {
            sub = latest.drs ? 'Active (Open)' : (status?.drs_allowed ? 'Available' : 'Closed')
          }
        }
        return (
          <Card key={d.vis} label={d.label} value={v.value} unit={v.unit}
                textColor={v.color} sub={sub} subTextColor={v.subColor} compact={compact} />
        )
      })}
    </div>
  )
})
export default LiveStats
