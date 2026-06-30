import { memo } from 'react'
import type { StatusRow } from '../types'
import { useLabels } from '../lib/labels'
import { POWER_RESOLVERS, useColorFn, type CardCtx, type CardDesc } from '../lib/cards'

const Card = memo(function Card({
  label, value, unit, color, sub,
}: {
  label: string; value: string; unit?: string; color?: string; sub?: string
}) {
  return (
    <div className="flex-1 min-w-0 px-3 py-2">
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
}

const PowerStatsBar = memo(function PowerStatsBar({ status, visibleCards, isDark }: Props) {
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
        return <Card key={d.vis} label={d.label} value={v.value} unit={v.unit} color={v.color} sub={v.sub} />
      })}
    </div>
  )
})
export default PowerStatsBar
