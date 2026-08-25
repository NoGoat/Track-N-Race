import { memo } from 'react'
import type { TelemetryRow, DamageRow } from '../types'
import type { DensityMode } from '../lib/graphSections'

interface VisibleItems {
  wingFl: boolean; wingFr: boolean; wingRear: boolean; floor: boolean
  diffuser: boolean; sidepod: boolean; gearbox: boolean; engine: boolean
  tyreDmgFl: boolean; tyreDmgFr: boolean; tyreDmgRl: boolean; tyreDmgRr: boolean
  brakeDmgFl: boolean; brakeDmgFr: boolean; brakeDmgRl: boolean; brakeDmgRr: boolean
}

interface Props {
  connected: boolean
  damage: DamageRow | null
  visibleItems: VisibleItems
  twoRow?: boolean
  isDark: boolean
  compact?: DensityMode | boolean
}

const DamageCard = memo(function DamageCard({ label, v, connected, isDark, compact }: { label: string; v: number; connected: boolean; isDark: boolean; compact?: DensityMode | boolean }) {
  const textColor = !connected ? undefined : v > 0 ? '#C4162A' : (isDark ? '#37872D' : '#137333')
  const isCompact = compact === true || compact === 'compact'
  const isSpacious = compact === 'spacious'

  if (isCompact) {
    // Single-line: label · value% — the row shrinks from two lines to one.
    return (
      <div className="flex-1 min-w-0 px-3 py-1 flex items-center justify-between gap-2">
        <span className="text-[9px] text-[var(--text-secondary)] uppercase tracking-widest truncate">{label}</span>
        <span className="flex items-baseline gap-0.5 shrink-0">
          <span className="text-sm font-bold tabular-nums" style={textColor ? { color: textColor } : undefined}>
            {!connected ? '—' : String(v)}
          </span>
          {connected && <span className="text-[9px] text-[var(--text-secondary)]">%</span>}
        </span>
      </div>
    )
  }

  if (isSpacious) {
    return (
      <div className="flex-1 min-w-0 px-4 py-3">
        <div className="text-[11px] font-bold text-[var(--text-secondary)] uppercase tracking-wider mb-1">{label}</div>
        <div className="flex items-baseline gap-0.5 overflow-hidden">
          <span
            className="text-2xl font-black tabular-nums truncate"
            style={textColor ? { color: textColor } : undefined}
          >
            {!connected ? '—' : String(v)}
          </span>
          {connected && <span className="text-xs font-semibold text-[var(--text-secondary)] shrink-0">%</span>}
        </div>
      </div>
    )
  }

  return (
    <div className="flex-1 min-w-0 px-3 py-2">
      <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-widest mb-0.5">{label}</div>
      <div className="flex items-baseline gap-0.5 overflow-hidden">
        <span
          className="text-xl font-bold tabular-nums truncate"
          style={textColor ? { color: textColor } : undefined}
        >
          {!connected ? '—' : String(v)}
        </span>
        {connected && <span className="text-[10px] text-[var(--text-secondary)] shrink-0">%</span>}
      </div>
    </div>
  )
})

const DamagePanel = memo(function DamagePanel({ connected, damage, visibleItems, twoRow, isDark, compact }: Props) {
  const allItems = [
    { key: 'tyreDmgFl'  as keyof VisibleItems, label: 'Tyre FL',   v: damage?.tyre_dmg_fl    ?? 0 },
    { key: 'brakeDmgFl' as keyof VisibleItems, label: 'Brake FL',  v: damage?.brake_dmg_fl   ?? 0 },
    { key: 'tyreDmgFr'  as keyof VisibleItems, label: 'Tyre FR',   v: damage?.tyre_dmg_fr    ?? 0 },
    { key: 'brakeDmgFr' as keyof VisibleItems, label: 'Brake FR',  v: damage?.brake_dmg_fr   ?? 0 },
    { key: 'tyreDmgRl'  as keyof VisibleItems, label: 'Tyre RL',   v: damage?.tyre_dmg_rl    ?? 0 },
    { key: 'brakeDmgRl' as keyof VisibleItems, label: 'Brake RL',  v: damage?.brake_dmg_rl   ?? 0 },
    { key: 'tyreDmgRr'  as keyof VisibleItems, label: 'Tyre RR',   v: damage?.tyre_dmg_rr    ?? 0 },
    { key: 'brakeDmgRr' as keyof VisibleItems, label: 'Brake RR',  v: damage?.brake_dmg_rr   ?? 0 },
    { key: 'wingFl'     as keyof VisibleItems, label: 'Wing FL',   v: damage?.wing_fl         ?? 0 },
    { key: 'wingFr'     as keyof VisibleItems, label: 'Wing FR',   v: damage?.wing_fr         ?? 0 },
    { key: 'wingRear'   as keyof VisibleItems, label: 'Wing Rear', v: damage?.wing_rear       ?? 0 },
    { key: 'floor'      as keyof VisibleItems, label: 'Floor',     v: damage?.floor_damage    ?? 0 },
    { key: 'diffuser'   as keyof VisibleItems, label: 'Diffuser',  v: damage?.diffuser_damage ?? 0 },
    { key: 'sidepod'    as keyof VisibleItems, label: 'Sidepod',   v: damage?.sidepod_damage  ?? 0 },
    { key: 'gearbox'    as keyof VisibleItems, label: 'Gearbox',   v: damage?.gearbox_damage  ?? 0 },
    { key: 'engine'     as keyof VisibleItems, label: 'Engine',    v: damage?.engine_damage   ?? 0 },
  ].filter(item => visibleItems[item.key])

  if (twoRow) {
    const split = Math.ceil(allItems.length / 2)
    const row1 = allItems.slice(0, split)
    const row2 = allItems.slice(split)
    return (
      <div className="flex flex-col divide-y divide-[var(--border)]">
        <div className="flex divide-x divide-[var(--border)]">
          {row1.map(item => <DamageCard key={item.key} label={item.label} v={item.v} connected={connected} isDark={isDark} compact={compact} />)}
        </div>
        <div className="flex divide-x divide-[var(--border)]">
          {row2.map(item => <DamageCard key={item.key} label={item.label} v={item.v} connected={connected} isDark={isDark} compact={compact} />)}
        </div>
      </div>
    )
  }

  return (
    <div className="flex divide-x divide-[var(--border)]">
      {allItems.map(item => <DamageCard key={item.key} label={item.label} v={item.v} connected={connected} isDark={isDark} compact={compact} />)}
    </div>
  )
})
export default DamagePanel
