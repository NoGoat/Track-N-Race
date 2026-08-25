import { memo } from 'react'
import type { StatusRow } from '../types'
import { useLabels } from '../lib/labels'
import { POWER_RESOLVERS, useColorFn, type CardCtx, type CardDesc } from '../lib/cards'
import type { DensityMode } from '../lib/graphSections'

const Card = memo(function Card({
  label, value, unit, color, sub, compact,
}: {
  label: string; value: string; unit?: string; color?: string; sub?: string; compact?: DensityMode | boolean
}) {
  const isCompact = compact === true || compact === 'compact'
  const isSpacious = compact === 'spacious'

  if (isCompact) {
    // Single-line: label · value+unit · sub.
    return (
      <div className="flex-1 min-w-0 px-3 py-1.5 flex items-center justify-between gap-2">
        <span className="text-[9px] text-[var(--text-secondary)] uppercase tracking-widest truncate">{label}</span>
        <span className="flex items-baseline gap-0.5 shrink-0">
          <span className="text-sm font-bold tabular-nums" style={{ color: color ?? 'var(--text-primary)' }}>{value}</span>
          {unit && <span className="text-[9px] text-[var(--text-secondary)]">{unit}</span>}
          {sub && <span className="text-[9px] ml-1 text-[var(--text-secondary)]">{sub}</span>}
        </span>
      </div>
    )
  }

  if (isSpacious) {
    return (
      <div className="flex-1 min-w-0 px-4 py-3 min-h-[96px]">
        <div className="text-[11px] font-bold text-[var(--text-secondary)] uppercase tracking-wider mb-1">{label}</div>
        <div className="flex items-baseline gap-1 overflow-hidden">
          <span className="text-3xl font-black tabular-nums truncate" style={{ color: color ?? 'var(--text-primary)' }}>
            {value}
          </span>
          {unit && <span className="text-xs font-semibold text-[var(--text-secondary)] shrink-0">{unit}</span>}
        </div>
        {sub && <div className="text-xs mt-1 truncate font-semibold text-[var(--text-secondary)]">{sub}</div>}
      </div>
    )
  }

  return (
    <div className="flex-1 min-w-0 px-3 py-2 min-h-[75px]">
      <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-widest mb-0.5">{label}</div>
      <div className="flex items-baseline gap-0.5 overflow-hidden">
        <span className="text-xl font-bold tabular-nums truncate" style={{ color: color ?? 'var(--text-primary)' }}>
          {value}
        </span>
        {unit && <span className="text-[10px] text-[var(--text-secondary)] shrink-0">{unit}</span>}
      </div>
      {sub && <div className="text-[9px] mt-0.5 truncate text-[var(--text-secondary)]">{sub}</div>}
    </div>
  )
})

interface VisibleCards {
  totalPower: boolean; ice: boolean; mguk: boolean; split: boolean
  ersStore: boolean; ersPct: boolean; fuel: boolean
}

interface Props {
  status: StatusRow | null
  visibleCards: VisibleCards
  isDark: boolean
  compact?: DensityMode | boolean
}

const PowerStatsBar = memo(function PowerStatsBar({ status, visibleCards, isDark, compact }: Props) {
  const { t, tn } = useLabels()
  const color = useColorFn(null, status, isDark)

  const descs: (CardDesc & { vis: keyof VisibleCards })[] = [
    { key: 'totalPower', label: 'Total Power', vis: 'totalPower' },
    { key: 'ice',        label: 'ICE',         vis: 'ice' },
    { key: 'mguk',       label: 'MGU-K',       vis: 'mguk' },
    { key: 'split',      label: 'Split',       vis: 'split' },
    { key: 'ersStore',   label: 'ERS Store',   vis: 'ersStore' },
    { key: 'ersPct',     label: 'ERS %',       vis: 'ersPct' },
    { key: 'fuel',       label: 'Fuel',        vis: 'fuel' },
  ]
  const ctx: CardCtx = { latest: null, status, lap: null, damage: null, session: null, isDark, t, tn, color }

  return (
    <div className="flex divide-x divide-[var(--border)]">
      {descs.filter(d => visibleCards[d.vis]).map(d => {
        const v = POWER_RESOLVERS[d.key]?.(ctx) ?? { value: '—' }
        return <Card key={d.vis} label={d.label} value={v.value} unit={v.unit} color={v.color} sub={v.sub} compact={compact} />
      })}
    </div>
  )
})
export default PowerStatsBar
