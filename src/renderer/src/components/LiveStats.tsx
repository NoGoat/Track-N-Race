import { memo } from 'react'
import type { TelemetryRow, StatusRow, LapRow, DamageRow } from '../types'
import { useLabels } from '../lib/labels'
import { OVERVIEW_RESOLVERS, useColorFn, type CardCtx, type CardDesc } from '../lib/cards'

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
        {shown.map(d => <Card key={d.vis} label={d.label} value="—" />)}
      </div>
    )
  }

  const ctx: CardCtx = { latest, status, lap, damage, session: null, isDark, t, tn, color }
  return (
    <div className="flex divide-x divide-[var(--border)]">
      {shown.map(d => {
        const v = OVERVIEW_RESOLVERS[d.key]?.(ctx) ?? { value: '—' }
        return (
          <Card key={d.vis} label={d.label} value={v.value} unit={v.unit}
                textColor={v.color} sub={v.sub} subTextColor={v.subColor} />
        )
      })}
    </div>
  )
})
export default LiveStats
