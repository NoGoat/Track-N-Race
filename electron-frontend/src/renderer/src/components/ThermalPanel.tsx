import { memo, useMemo } from 'react'
import type { AlignedTable, TelemetryRow, DamageRow } from '../types'
import TyreTrendCharts from './TyreTrendCharts'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import { useColorFn } from '../lib/cards'
import type { TyreYAxisGroupState } from '../lib/graphSections'

type TyreGraphViews = { surfaceTemp?: 'chart' | 'table'; innerTemp?: 'chart' | 'table'; brakeTemp?: 'chart' | 'table'; tyreLife?: 'chart' | 'table' }
export type TyreCardViews = { fl?: 'chart' | 'table'; fr?: 'chart' | 'table'; rl?: 'chart' | 'table'; rr?: 'chart' | 'table' }

interface Props {
  latest: TelemetryRow | null
  damage: DamageRow | null
  telemetry: TelemetryRow[]
  damageHistory: DamageRow[]
  view: 'cards' | 'graphs'
  tyreWearMode: 'wear' | 'life'
  thermalGraphs: { surfaceTemp: boolean; innerTemp: boolean; brakeTemp: boolean; tyreLife: boolean }
  thermalCards:  { fl: boolean; fr: boolean; rl: boolean; rr: boolean }
  isDark: boolean
  // Overview tyre-card density, 0 Normal … 5 Compact 5 (Compact 5 = the app's
  // previous height-derived compact layout, now a selectable level).
  tyresLevel?: number
  graphViews?: TyreGraphViews
  cardViews?: TyreCardViews
  windowSeconds?: number
  yAxis: TyreYAxisGroupState
}

// Tyre-card density levels, ported from native TyreCardsWidget::Level:
//   0 Normal        — full column card (Surface/Inner/Brake/Wear rows + wear bar)
//   1 Compact 1     — centred corner name + rule, then one horizontal row:
//                     SURFACE [v]  INNER [v]  BRAKE [v]  WEAR [v]   (native "Compact")
//   2 Compact 2     — the same horizontal row, without the name heading (UltraCompact1)
//   3 Compact 3     — one line: full corner name + the four bare values (UltraCompact2)
//   4 Compact 4     — one line: abbreviated name (FL) + labelled values (UltraCompact3)
//   5 Compact 5     — the app's earlier compact column card, kept as a level

// Temp/wear colours now come from the shared library spec (temp.tyre / temp.brake
// / wear) via the card colour evaluator, so they stay in lockstep with the native
// recorder and the stat cards.

export const TempRow = memo(function TempRow({ label, value, color, noData, compact }: {
  label: string; value: number; color: string; noData?: boolean; compact?: boolean
}) {
  return (
    <div className={`flex justify-between items-center ${compact ? 'py-px' : 'py-0.5'}`}>
      <span className={`text-[var(--text-secondary)] ${compact ? 'text-xs' : 'text-sm'}`}>{label}</span>
      <span className={`font-bold tabular-nums ${compact ? 'text-xs' : 'text-sm'}`} style={{ color: noData ? '#4a4a4a' : color }}>
        {noData ? '—' : `${value}°C`}
      </span>
    </div>
  )
})

export const WearBar = memo(function WearBar({ pct, blisters, noData, compact, isDark = true }: {
  pct: number | null; blisters: number | null; noData?: boolean; compact?: boolean; isDark?: boolean
}) {
  const ramp = useColorFn(null, null, isDark)
  const color = pct !== null ? (ramp('wear', pct) ?? '#888') : '#4a4a4a'
  return (
    <div className={compact ? '' : 'pt-2 border-t border-[var(--border)]'}>
      {!compact && (
        <div className="flex justify-between text-[9px] text-[var(--text-secondary)] mb-1">
          <span>Wear</span>
          <span style={{ color: noData ? '#4a4a4a' : color }}>
            {noData || pct === null ? '—' : `${pct.toFixed(1)}%${blisters ? ` · ${blisters}% blisters` : ''}`}
          </span>
        </div>
      )}
      <div className={`w-full bg-[var(--border)] rounded-full overflow-hidden ${compact ? 'h-1.5' : 'h-2'}`}>
        {!noData && pct !== null && (
          <div className="h-full rounded-full" style={{ width: `${Math.min(pct, 100)}%`, background: color }} />
        )}
      </div>
      {compact && (
        <div className="text-xs tabular-nums mt-0.5" style={{ color: noData ? '#4a4a4a' : color }}>
          {noData || pct === null ? '—' : `${pct.toFixed(1)}%`}
        </div>
      )}
    </div>
  )
})

export const WheelCard = memo(function WheelCard({
  pos, surface, inner, brake, wear, blisters, noData, compact, level, isDark = true, view, history,
}: {
  pos: string; surface: number; inner: number; brake: number
  wear: number | null; blisters: number | null; noData?: boolean; compact?: boolean; level?: number; isDark?: boolean
  view?: 'chart' | 'table'; history?: AlignedTable
}) {
  const ramp = useColorFn(null, null, isDark)

  const tableCols = useMemo((): GraphTableColumn[] => [
    { header: 'Surface', color: ramp('temp.tyre', surface) ?? '#888',  format: v => `${Math.round(v)}°C` },
    { header: 'Inner',   color: ramp('temp.tyre', inner) ?? '#888',    format: v => `${Math.round(v)}°C` },
    { header: 'Brake',   color: ramp('temp.brake', brake) ?? '#888',   format: v => `${Math.round(v)}°C` },
  ], [ramp, surface, inner, brake])

  if (view === 'table' && history) {
    // Tyre cards are otherwise content-height (sized to their live-value rows), but a
    // scrolling table needs a real box to fill — give it an explicit height rather than
    // relying on flex-1 against an ancestor that may not itself have a definite height.
    return (
      <div className="flex-1 min-w-0 overflow-hidden relative h-64">
        <GraphTable columns={tableCols} data={history} edgePadRem={0} noBorderTop />
      </div>
    )
  }
  // `level` (Overview) takes precedence; `compact` (Tyres page) maps to level 5 / 0.
  const lvl = Math.max(0, Math.min(5, level ?? (compact ? 5 : 0)))
  // Only the Overview page passes `level` explicitly — the Tyres page's own WheelCard
  // usage (via `compact`) keeps the original stretched/justify-between layout, since
  // it stacks 4 cards in a fixed-height flex column that relies on that fill behaviour.
  const isOverviewNormal = level !== undefined && lvl === 0
  const abbrev = pos.split(/\s+/).map(w => w[0]).join('').toUpperCase()

  // The four metrics, in native's Surface/Inner/Brake/Wear order, each with its
  // ramped colour. Temps are integers with °C; wear is an integer percent.
  const noWear = noData || wear === null
  const metrics = [
    { label: 'Surface', text: noData ? '—' : `${Math.round(surface)}°C`, color: noData ? '#4a4a4a' : (ramp('temp.tyre', surface) ?? '#888') },
    { label: 'Inner',   text: noData ? '—' : `${Math.round(inner)}°C`,   color: noData ? '#4a4a4a' : (ramp('temp.tyre', inner) ?? '#888') },
    { label: 'Brake',   text: noData ? '—' : `${Math.round(brake)}°C`,   color: noData ? '#4a4a4a' : (ramp('temp.brake', brake) ?? '#888') },
    { label: 'Wear',    text: noWear ? '—' : `${Math.round(wear!)}%`,     color: noWear ? '#4a4a4a' : (ramp('wear', wear!) ?? '#888') },
  ]

  // ── Compact 1 & 2: the four metrics as one horizontal row (± a name heading) ──
  if (lvl === 1 || lvl === 2) {
    const row = (
      <div className="flex items-stretch w-full">
        {metrics.map((m, i) => (
          <div key={m.label} className={`flex-1 min-w-0 flex items-baseline justify-between gap-2 px-3 py-1 ${i > 0 ? 'border-l border-[var(--border)]' : ''}`}>
            <span className="text-[9px] uppercase tracking-wide text-[var(--text-secondary)]">{m.label}</span>
            <span className="text-[11px] font-bold tabular-nums" style={{ color: m.color }}>{m.text}</span>
          </div>
        ))}
      </div>
    )
    return (
      <div className="flex-1 min-w-0 overflow-hidden flex flex-col justify-center">
        {lvl === 1 && (
          <div className="text-[10px] font-bold uppercase tracking-widest text-[var(--text-secondary)] text-center pt-1 pb-1 border-b border-[var(--border)]">
            {pos}
          </div>
        )}
        {row}
      </div>
    )
  }

  // ── Compact 3 & 4: a single line — name (± metric labels) then the values ──
  if (lvl === 3 || lvl === 4) {
    const showLabels = lvl === 4
    return (
      <div className="flex-1 min-w-0 overflow-hidden flex items-center gap-2 px-3 py-1">
        <span className="text-[10px] font-bold uppercase tracking-widest text-[var(--text-secondary)] shrink-0">
          {lvl === 4 ? abbrev : pos}
        </span>
        <div className="flex-1 min-w-0" />
        <div className={`flex items-baseline ${showLabels ? 'gap-1.5' : 'gap-3'} shrink-0`}>
          {metrics.map(m => (
            <span key={m.label} className={`flex items-baseline gap-1 ${showLabels ? 'pl-1.5' : ''}`}>
              {showLabels && <span className="text-[9px] text-[var(--text-secondary)]">{m.label}</span>}
              <span className="text-[11px] font-bold tabular-nums" style={{ color: m.color }}>{m.text}</span>
            </span>
          ))}
        </div>
      </div>
    )
  }

  // ── Level 0 (Normal) & 5 (the app's earlier compact column card) ──
  const isCompactCol = lvl === 5
  const padding = isCompactCol ? 'p-2' : (isOverviewNormal ? 'p-3' : 'p-4')
  const labelMb = isCompactCol ? 'mb-1' : (isOverviewNormal ? 'mb-2' : 'mb-3')
  const dividerMy = isOverviewNormal ? 'my-1' : 'my-1.5'
  return (
    <div className={`flex-1 min-w-0 overflow-hidden flex flex-col ${isOverviewNormal ? '' : 'justify-between'} ${padding}`}>
      <div className={`text-[10px] text-[var(--text-secondary)] font-bold uppercase tracking-widest ${labelMb}`}>
        {pos}
      </div>
      <TempRow label="Surface" value={surface} color={ramp('temp.tyre', surface) ?? '#888'} noData={noData} compact={isCompactCol} />
      <TempRow label="Inner"   value={inner}   color={ramp('temp.tyre', inner) ?? '#888'}   noData={noData} compact={isCompactCol} />
      {!isCompactCol && <div className={`${dividerMy} border-t border-[var(--border)]`} />}
      <TempRow label="Brake"   value={brake}   color={ramp('temp.brake', brake) ?? '#888'}  noData={noData} compact={isCompactCol} />
      <div className={isOverviewNormal && !isCompactCol ? 'mt-2' : undefined}>
        <WearBar pct={wear} blisters={blisters} noData={noData} compact={isCompactCol} isDark={isDark} />
      </div>
    </div>
  )
})


const EMPTY_HISTORY: AlignedTable = [new Float64Array(0)]
const EMPTY_CORNER_HISTORIES: Record<'fl' | 'fr' | 'rl' | 'rr', AlignedTable> = {
  fl: EMPTY_HISTORY, fr: EMPTY_HISTORY, rl: EMPTY_HISTORY, rr: EMPTY_HISTORY,
}

const CORNERS = ['fl', 'fr', 'rl', 'rr'] as const
type Corner = typeof CORNERS[number]

function useCornerHistories(telemetry: TelemetryRow[], enabled: Record<Corner, boolean>): Record<Corner, AlignedTable> {
  return useMemo(() => {
    const requested = CORNERS.filter(corner => enabled[corner])
    if (requested.length === 0) return EMPTY_CORNER_HISTORIES
    const n = telemetry.length
    const ts = new Float64Array(n)
    const histories = { ...EMPTY_CORNER_HISTORIES }
    for (const corner of requested) histories[corner] = [ts, new Float64Array(n), new Float64Array(n), new Float64Array(n)]
    telemetry.forEach((d, i) => {
      ts[i] = d.session_time
      for (const corner of requested) {
        const history = histories[corner]
        ;(history[1] as Float64Array)[i] = d[`tyre_temp_surface_${corner}`]
        ;(history[2] as Float64Array)[i] = d[`tyre_temp_inner_${corner}`]
        ;(history[3] as Float64Array)[i] = d[`brake_temp_${corner}`]
      }
    })
    return histories
  }, [telemetry, enabled])
}

const ThermalPanel = memo(function ThermalPanel({ latest, damage, telemetry, damageHistory, view, tyreWearMode, thermalGraphs, thermalCards, isDark, tyresLevel = 0, graphViews, cardViews, windowSeconds = 30, yAxis }: Props) {
  const sf = { fl: latest?.tyre_temp_surface_fl ?? 0, fr: latest?.tyre_temp_surface_fr ?? 0, rl: latest?.tyre_temp_surface_rl ?? 0, rr: latest?.tyre_temp_surface_rr ?? 0 }
  const inn = { fl: latest?.tyre_temp_inner_fl   ?? 0, fr: latest?.tyre_temp_inner_fr   ?? 0, rl: latest?.tyre_temp_inner_rl   ?? 0, rr: latest?.tyre_temp_inner_rr   ?? 0 }
  const brk = { fl: latest?.brake_temp_fl        ?? 0, fr: latest?.brake_temp_fr        ?? 0, rl: latest?.brake_temp_rl        ?? 0, rr: latest?.brake_temp_rr        ?? 0 }

  // Graph view does not consume per-corner uPlot columns; avoid rebuilding all
  // thirteen typed arrays on every telemetry publication while it is active.
  const tableCorners = useMemo(() => Object.fromEntries(CORNERS.map(corner => [
    corner,
    view !== 'graphs' && thermalCards[corner] && cardViews?.[corner] === 'table',
  ])) as Record<Corner, boolean>, [cardViews, thermalCards, view])
  const cornerHistory = useCornerHistories(telemetry, tableCorners)

  // Tyre cards are content-height (at every density level) so the strip is short —
  // stretching them to fill the flex region just spreads the rows apart with gaps.
  const compactCards = view === 'cards'

  return (
    <div className={compactCards ? '' : 'h-full'}>
      {view === 'graphs' ? (
        <div className="h-full">
          <TyreTrendCharts telemetry={telemetry} damageHistory={damageHistory} tyreWearMode={tyreWearMode} visibleGraphs={thermalGraphs} isDark={isDark} graphViews={graphViews} windowSeconds={windowSeconds} fastScroll yAxis={yAxis} />
        </div>
      ) : (
        <div className={`flex divide-x divide-[var(--border)] ${compactCards ? '' : 'h-full'}`}>
          {thermalCards.fl && <WheelCard pos="Front Left"  surface={sf.fl}  inner={inn.fl}  brake={brk.fl}
            wear={damage?.tyre_wear_fl ?? null} blisters={damage?.blisters_fl ?? null} noData={!latest} level={tyresLevel} isDark={isDark}
            view={cardViews?.fl} history={cornerHistory.fl} />}
          {thermalCards.fr && <WheelCard pos="Front Right" surface={sf.fr}  inner={inn.fr}  brake={brk.fr}
            wear={damage?.tyre_wear_fr ?? null} blisters={damage?.blisters_fr ?? null} noData={!latest} level={tyresLevel} isDark={isDark}
            view={cardViews?.fr} history={cornerHistory.fr} />}
          {thermalCards.rl && <WheelCard pos="Rear Left"   surface={sf.rl}  inner={inn.rl}  brake={brk.rl}
            wear={damage?.tyre_wear_rl ?? null} blisters={damage?.blisters_rl ?? null} noData={!latest} level={tyresLevel} isDark={isDark}
            view={cardViews?.rl} history={cornerHistory.rl} />}
          {thermalCards.rr && <WheelCard pos="Rear Right"  surface={sf.rr}  inner={inn.rr}  brake={brk.rr}
            wear={damage?.tyre_wear_rr ?? null} blisters={damage?.blisters_rr ?? null} noData={!latest} level={tyresLevel} isDark={isDark}
            view={cardViews?.rr} history={cornerHistory.rr} />}
        </div>
      )}
    </div>
  )
})
export default ThermalPanel
