import type { StatusRow } from '../types'

function Card({
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
}

interface VisibleCards {
  totalPower: boolean; ice: boolean; mguk: boolean; split: boolean
  ersStore: boolean; ersPct: boolean; fuel: boolean
}

interface Props {
  status: StatusRow | null
  visibleCards: VisibleCards
  isDark: boolean
}

export default function PowerStatsBar({ status, visibleCards, isDark }: Props) {
  const cIce  = isDark ? '#5794F2' : '#0B57D0'
  const cMguk = isDark ? '#FADE2A' : '#B06000'
  const cFuel = isDark ? '#F0A500' : '#C26400'
  const red   = '#C4162A'
  const green = isDark ? '#37872D' : '#137333'

  const ersColor = (pct: number) => {
    if (pct > 60) return cIce
    if (pct > 30) return cMguk
    return red
  }

  const noData = !status
  const iceKw   = status?.engine_power_ice_kw  ?? 0
  const mgukKw  = status?.engine_power_mguk_kw ?? 0
  const totalKw = iceKw + mgukKw
  const iceSplit = totalKw > 0 ? (iceKw  / totalKw * 100) : 0
  const ersSplit = totalKw > 0 ? (mgukKw / totalKw * 100) : 0
  const ersMJ    = ((status?.ers_pct ?? 0) / 100) * 4
  const ersPct   = status?.ers_pct ?? 0

  return (
    <div className="flex divide-x divide-[var(--border)]">
      {visibleCards.totalPower && <Card label="Total Power" value={noData ? '—' : totalKw.toFixed(0)} unit={noData ? undefined : 'kW'} color={noData ? undefined : totalKw > 800 ? red : totalKw > 600 ? cMguk : green} />}
      {visibleCards.ice        && <Card label="ICE"         value={noData ? '—' : iceKw.toFixed(0)}   unit={noData ? undefined : 'kW'} color={noData ? undefined : cIce}  />}
      {visibleCards.mguk       && <Card label="MGU-K"       value={noData ? '—' : mgukKw.toFixed(0)}  unit={noData ? undefined : 'kW'} color={noData ? undefined : cMguk} />}
      {visibleCards.split      && <Card label="Split"       value={noData ? '—' : `${iceSplit.toFixed(0)}:${ersSplit.toFixed(0)}`} sub={noData ? undefined : 'ICE : Electric'} />}
      {visibleCards.ersStore   && <Card label="ERS Store"   value={noData ? '—' : ersMJ.toFixed(2)}   unit={noData ? undefined : 'MJ'} color={noData ? undefined : ersColor(ersPct)} />}
      {visibleCards.ersPct     && <Card label="ERS %"       value={noData ? '—' : ersPct.toFixed(0)}  unit={noData ? undefined : '%'}  color={noData ? undefined : ersColor(ersPct)} />}
      {visibleCards.fuel       && <Card label="Fuel"        value={noData ? '—' : (status?.fuel_kg.toFixed(1) ?? '—')} unit={noData ? undefined : 'kg'} color={noData ? undefined : cFuel} />}
    </div>
  )
}
