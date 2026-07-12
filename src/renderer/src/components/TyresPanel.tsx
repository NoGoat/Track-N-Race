import { useState, useMemo, memo } from 'react'
import type uPlot from 'uplot'
import { Maximize2, Minimize2 } from 'lucide-react'
import type { TyreSetsMsg, TyreSetEntry, TelemetryRow, DamageRow } from '../types'
import { useLabels } from '../lib/labels'
import TyreTrendCharts from './TyreTrendCharts'
import { WheelCard, type TyreCardViews } from './ThermalPanel'
import { useColorFn } from '../lib/cards'
import { useSize } from '../hooks/useSize'

interface Props {
  tyreSets:      TyreSetsMsg | null
  latest:        TelemetryRow | null
  damage:        DamageRow | null
  damageHistory: DamageRow[]
  telemetry:     TelemetryRow[]
  tyreWearMode:  'wear' | 'life'
  isDark:        boolean
  visibleGraphs: { surfaceTemp: boolean; innerTemp: boolean; brakeTemp: boolean; tyreLife: boolean }
  graphViews?:   { surfaceTemp?: 'chart' | 'table'; innerTemp?: 'chart' | 'table'; brakeTemp?: 'chart' | 'table'; tyreLife?: 'chart' | 'table' }
  cardViews?:    TyreCardViews
  sessionType:   number | null
  windowSeconds?: number
}

const EMPTY_HISTORY: uPlot.AlignedData = [new Float64Array(0)]
const EMPTY_CORNER_HISTORIES: Record<'fl' | 'fr' | 'rl' | 'rr', uPlot.AlignedData> = {
  fl: EMPTY_HISTORY, fr: EMPTY_HISTORY, rl: EMPTY_HISTORY, rr: EMPTY_HISTORY,
}

function useCornerHistories(telemetry: TelemetryRow[], enabled: boolean): Record<'fl' | 'fr' | 'rl' | 'rr', uPlot.AlignedData> {
  return useMemo(() => {
    if (!enabled) return EMPTY_CORNER_HISTORIES
    const n = telemetry.length
    const ts = new Float64Array(n)
    const flS = new Float64Array(n), frS = new Float64Array(n), rlS = new Float64Array(n), rrS = new Float64Array(n)
    const flI = new Float64Array(n), frI = new Float64Array(n), rlI = new Float64Array(n), rrI = new Float64Array(n)
    const flB = new Float64Array(n), frB = new Float64Array(n), rlB = new Float64Array(n), rrB = new Float64Array(n)
    telemetry.forEach((d, i) => {
      ts[i] = d.session_time
      flS[i] = d.tyre_temp_surface_fl; frS[i] = d.tyre_temp_surface_fr; rlS[i] = d.tyre_temp_surface_rl; rrS[i] = d.tyre_temp_surface_rr
      flI[i] = d.tyre_temp_inner_fl;   frI[i] = d.tyre_temp_inner_fr;   rlI[i] = d.tyre_temp_inner_rl;   rrI[i] = d.tyre_temp_inner_rr
      flB[i] = d.brake_temp_fl;        frB[i] = d.brake_temp_fr;        rlB[i] = d.brake_temp_rl;        rrB[i] = d.brake_temp_rr
    })
    return {
      fl: [ts, flS, flI, flB],
      fr: [ts, frS, frI, frB],
      rl: [ts, rlS, rlI, rlB],
      rr: [ts, rrS, rrI, rrB],
    }
  }, [telemetry, enabled])
}

const WET_COMPOUNDS = new Set([7, 8])


const VISUAL_COLORS: Record<number, string> = {
  16: 'var(--compound-soft)',
  17: 'var(--compound-medium)',
  18: 'var(--compound-hard)',
   7: 'var(--compound-inter)',
   8: 'var(--compound-wet)',
}

const SESSION_LABELS: Record<number, string> = {
  0: '—', 1: 'FP1', 2: 'FP2', 3: 'FP3', 4: 'Q1', 5: 'Q2', 6: 'Q3', 7: 'Race',
}

const DRY_SORT: Record<number, number> = { 16: 0, 17: 1, 18: 2 }
const WET_SORT: Record<number, number> = { 7: 0, 8: 1 }

function sortDry(a: TyreSetEntry, b: TyreSetEntry) {
  const ao = DRY_SORT[a.visual_compound] ?? 3
  const bo = DRY_SORT[b.visual_compound] ?? 3
  return ao !== bo ? ao - bo : a.idx - b.idx
}

function sortWet(a: TyreSetEntry, b: TyreSetEntry) {
  const ao = WET_SORT[a.actual_compound] ?? 2
  const bo = WET_SORT[b.actual_compound] ?? 2
  return ao !== bo ? ao - bo : a.idx - b.idx
}

function getSessionOrder(sessType: number): number {
  if (sessType >= 1 && sessType <= 3) return sessType; // FP1=1, FP2=2, FP3=3
  if (sessType === 4) return 3; // Short Practice -> FP3 slot
  if (sessType === 5) return 4; // Q1
  if (sessType === 6) return 5; // Q2
  if (sessType === 7) return 6; // Q3
  if (sessType >= 8 && sessType <= 9) return 6; // Other Qualis -> Q3 slot
  if (sessType === 10) return 4; // Sprint Shootout 1 -> Q1 slot
  if (sessType === 11) return 5; // Sprint Shootout 2 -> Q2 slot
  if (sessType === 12) return 6; // Sprint Shootout 3 -> Q3 slot
  if (sessType >= 13 && sessType <= 14) return 6; // Other Sprint Shootouts -> Q3 slot
  return 7; // Race, Sprint Race, Time Trial, etc. -> Race slot (7)
}

function getStatus(s: TyreSetEntry, sessionType: number | null): 'FITTED' | 'NEW' | 'USED' | 'RESERVED' | 'RETURNED' {
  if (s.fitted)                    return 'FITTED'
  if (s.available && s.wear === 0) return 'NEW'
  if (s.available && s.wear > 0)   return 'USED'
  
  // If unavailable:
  if (sessionType !== null) {
    const currentOrder = getSessionOrder(sessionType);
    if (s.recommended_session > currentOrder) {
      return 'RESERVED';
    }
  } else {
    // Fallback: recommended session is Q1 (4) or later
    if (s.recommended_session >= 4) {
      return 'RESERVED';
    }
  }
  
  return 'RETURNED'
}

const AllocationWearBar = memo(function AllocationWearBar({ pct, isDark = true }: { pct: number; isDark?: boolean }) {
  const color = useColorFn(null, null, isDark)('wear', pct) ?? '#888'
  return (
    <div className="w-full h-1.5 bg-[var(--border)] rounded-full overflow-hidden">
      <div className="h-full rounded-full transition-all" style={{ width: `${Math.min(pct, 100)}%`, background: color }} />
    </div>
  )
})

const SetRow = memo(function SetRow({ set, isDark = true, sessionType }: { set: TyreSetEntry; isDark?: boolean; sessionType: number | null }) {
  const { tn }     = useLabels()
  const colorFn    = useColorFn(null, null, isDark)
  const status     = getStatus(set, sessionType)
  const compName   = tn('tyre.actual', set.actual_compound)
  const compColor  = VISUAL_COLORS[set.visual_compound] ?? '#ffffff'
  const isReturned = status === 'RETURNED'
  const isReserved = status === 'RESERVED'
  const isFitted   = status === 'FITTED'

  const statusColor =
    isFitted             ? (isDark ? '#5794F2' : '#0B57D0') :
    status === 'NEW'     ? (isDark ? '#37872D' : '#137333') :
    status === 'USED'    ? (isDark ? '#d4ad04' : '#B06000') :
    isReserved           ? (isDark ? '#a78bfa' : '#6d28d9') :
    (isDark ? '#484c62' : '#565B70')

  const showDelta = !isFitted && set.available && set.lap_delta_ms !== 0
  const deltaStr  = showDelta
    ? (set.lap_delta_ms > 0 ? `+${(set.lap_delta_ms / 1000).toFixed(3)}s` : `${(set.lap_delta_ms / 1000).toFixed(3)}s`)
    : null

  return (
    <div
      className={`flex items-center gap-2 px-3 py-1 border-b border-[var(--border)] ${
        isFitted ? (isDark ? 'bg-[#5794F2]/10' : 'bg-[#0B57D0]/10') : isReturned ? 'opacity-40' : ''
      }`}
    >
      <span className="text-[10px] text-[var(--text-secondary)] tabular-nums w-5 shrink-0">
        #{set.idx + 1}
      </span>
      <span className="text-[10px] font-black tabular-nums w-8 shrink-0" style={{ color: compColor }}>
        {compName}
      </span>
      <span
        className="text-[9px] font-bold py-0.5 rounded border shrink-0 w-16 text-center inline-block"
        style={{ color: statusColor, borderColor: statusColor, background: `${statusColor}18` }}
      >
        {status}
      </span>
      <div className="flex-1 min-w-0 flex items-center gap-2">
        <div className="flex-1 min-w-0">
          <AllocationWearBar pct={set.wear} isDark={isDark} />
        </div>
        <span className="text-[10px] tabular-nums w-8 text-right shrink-0" style={{ color: colorFn('wear', set.wear) ?? '#888' }}>
          {set.wear}%
        </span>
      </div>
      <span className="text-[10px] tabular-nums text-[var(--text-secondary)] w-14 text-right shrink-0">
        {set.life_span}/{set.usable_life}L
      </span>
      <span className="text-[10px] text-[var(--text-secondary)] w-8 text-center shrink-0">
        {SESSION_LABELS[set.recommended_session] ?? '—'}
      </span>
      <span
        className="text-[10px] tabular-nums w-14 text-right shrink-0"
        style={{ color: deltaStr ? (set.lap_delta_ms > 0 ? '#C4162A' : (isDark ? '#37872D' : '#137333')) : 'transparent' }}
      >
        {deltaStr ?? '—'}
      </span>
    </div>
  )
})

const COLUMN_HEADERS = (
  <div className="flex gap-3 text-[9px] text-[var(--text-secondary)]">
    <span className="w-8 text-right">Wear</span>
    <span className="w-14 text-right">Life</span>
    <span className="w-8 text-center">Rec.</span>
    <span className="w-14 text-right">Δ Lap</span>
  </div>
)

const SetSection = memo(function SetSection({ title, sets, isDark = true, sessionType }: { title: string; sets: TyreSetEntry[]; isDark?: boolean; sessionType: number | null }) {
  return (
    <div className="flex flex-col overflow-hidden">
      <div className="shrink-0 px-3 py-2 border-b border-[var(--border)] flex items-center justify-between">
        <span className="text-[10px] text-[var(--text-secondary)] uppercase tracking-widest">{title}</span>
        {COLUMN_HEADERS}
      </div>
      <div>
        {sets.map(s => <SetRow key={s.idx} set={s} isDark={isDark} sessionType={sessionType} />)}
      </div>
    </div>
  )
})

const EmptySection = memo(function EmptySection({ title, count }: { title: string; count: number }) {
  return (
    <div className="flex flex-col overflow-hidden">
      <div className="shrink-0 px-3 py-2 border-b border-[var(--border)] flex items-center justify-between">
        <span className="text-[10px] text-[var(--text-secondary)] uppercase tracking-widest">{title}</span>
        {COLUMN_HEADERS}
      </div>
      <div>
        {Array.from({ length: count }, (_, i) => (
          <div key={i} className="flex items-center gap-2 px-3 py-1 border-b border-[var(--border)]">
            <span className="text-[10px] text-[var(--text-secondary)] tabular-nums w-5">#{i + 1}</span>
            <span className="text-[10px] text-[var(--text-muted)] w-8">—</span>
            <span className="text-[9px] text-[var(--text-muted)] px-1.5 py-0.5 rounded border border-[var(--border)]">—</span>
            <div className="flex-1 min-w-0">
              <div className="w-full h-1.5 bg-[var(--border)] rounded-full" />
            </div>
          </div>
        ))}
      </div>
    </div>
  )
})

export default function TyresPanel({ tyreSets, latest, damage, damageHistory, telemetry, tyreWearMode, isDark, visibleGraphs, graphViews, cardViews, sessionType, windowSeconds = 30 }: Props) {
  const { tn } = useLabels()
  const colorFn = useColorFn(null, null, isDark)
  const [expanded, setExpanded] = useState(false)
  const { ref: cardsRef, height: cardsHeight } = useSize()
  // Expanded mode renders the shared TimeCharts directly. Building thirteen
  // full-window Float64 columns for the hidden wheel cards was pure allocation
  // churn (hundreds of MB/s at long 60 Hz windows), so skip it entirely.
  const needsCornerHistory = !expanded && Object.values(cardViews ?? {}).some(v => v === 'table')
  const cornerHistory = useCornerHistories(telemetry, needsCornerHistory)

  const drySets = useMemo(() => {
    return tyreSets?.sets.filter(s => !WET_COMPOUNDS.has(s.actual_compound)).sort(sortDry) ?? null
  }, [tyreSets])

  const wetSets = useMemo(() => {
    return tyreSets?.sets.filter(s =>  WET_COMPOUNDS.has(s.actual_compound)).sort(sortWet) ?? null
  }, [tyreSets])

  const noData = !latest
  const compact = cardsHeight > 0 && cardsHeight < 720

  if (expanded) {
    return (
      <div className="h-full flex flex-col bg-[var(--bg-panel)] overflow-hidden">
        <div className="shrink-0 px-3 py-2 border-b border-[var(--border)] flex items-center">
          <span className="text-[10px] text-[var(--text-secondary)] uppercase tracking-widest w-32 shrink-0">Tyre Conditions</span>
          <div className="flex-1 flex justify-center items-center gap-2">
            {(() => {
              const fitted = tyreSets?.sets.find(s => s.fitted)
              if (!fitted) return null
              const name  = tn('tyre.actual', fitted.actual_compound)
              const color = VISUAL_COLORS[fitted.visual_compound]  ?? 'var(--text-primary)'
              return <>
                <span className="text-[11px] font-black tabular-nums" style={{ color }}>{name}</span>
                <span className="text-[10px] text-[var(--text-secondary)]">·</span>
                <span className="text-[10px] tabular-nums" style={{ color: colorFn('wear', fitted.wear) ?? '#888' }}>{fitted.wear}% wear</span>
                <span className="text-[10px] text-[var(--text-secondary)]">·</span>
                <span className="text-[10px] tabular-nums text-[var(--text-secondary)]">{fitted.life_span}L remaining</span>
              </>
            })()}
          </div>
          <button
            onClick={() => setExpanded(false)}
            className="flex items-center gap-1.5 text-[10px] text-[var(--text-secondary)] hover:text-[var(--text-primary)] transition-colors w-32 shrink-0 justify-end"
          >
            <Minimize2 size={11} />
            <span>Allocation</span>
          </button>
        </div>
        <div className="flex-1 min-h-0">
          <TyreTrendCharts
            telemetry={telemetry}
            damageHistory={damageHistory}
            tyreWearMode={tyreWearMode}
            visibleGraphs={visibleGraphs}
            graphViews={graphViews}
            isDark={isDark}
            layout="grid"
            windowSeconds={windowSeconds}
          />
        </div>
      </div>
    )
  }

  return (
    <div className="h-full flex bg-[var(--bg-panel)] divide-x divide-[var(--border)] overflow-hidden">
      {/* Left: allocation table */}
      <div className="flex-1 min-w-0 h-full overflow-y-auto divide-y divide-[var(--border)]">
        {drySets
          ? <SetSection title="Dry Sets (Slicks)" sets={drySets} isDark={isDark} sessionType={sessionType} />
          : <EmptySection title="Dry Sets (Slicks)" count={13} />
        }
        {wetSets
          ? <SetSection title="Wet / Inter Sets" sets={wetSets} isDark={isDark} sessionType={sessionType} />
          : <EmptySection title="Wet / Inter Sets" count={7} />
        }
      </div>

      {/* Right: wheel condition cards */}
      <div className="w-64 shrink-0 h-full flex flex-col overflow-hidden divide-y divide-[var(--border)]">
        <div className="shrink-0 px-3 py-2 flex items-center justify-between">
          <span className="text-[10px] text-[var(--text-secondary)] uppercase tracking-widest">Conditions</span>
          <button
            onClick={() => setExpanded(true)}
            className="flex items-center gap-1.5 text-[10px] text-[var(--text-secondary)] hover:text-[var(--text-primary)] transition-colors"
          >
            <Maximize2 size={11} />
            <span>Graphs</span>
          </button>
        </div>
        <div ref={cardsRef} className="flex-1 flex flex-col divide-y divide-[var(--border)]">
          <WheelCard pos="Front Left"  surface={latest?.tyre_temp_surface_fl ?? 0} inner={latest?.tyre_temp_inner_fl ?? 0} brake={latest?.brake_temp_fl ?? 0}
            wear={damage?.tyre_wear_fl ?? null} blisters={damage?.blisters_fl ?? null} noData={noData} compact={compact} isDark={isDark}
            view={cardViews?.fl} history={cornerHistory.fl} />
          <WheelCard pos="Front Right" surface={latest?.tyre_temp_surface_fr ?? 0} inner={latest?.tyre_temp_inner_fr ?? 0} brake={latest?.brake_temp_fr ?? 0}
            wear={damage?.tyre_wear_fr ?? null} blisters={damage?.blisters_fr ?? null} noData={noData} compact={compact} isDark={isDark}
            view={cardViews?.fr} history={cornerHistory.fr} />
          <WheelCard pos="Rear Left"   surface={latest?.tyre_temp_surface_rl ?? 0} inner={latest?.tyre_temp_inner_rl ?? 0} brake={latest?.brake_temp_rl ?? 0}
            wear={damage?.tyre_wear_rl ?? null} blisters={damage?.blisters_rl ?? null} noData={noData} compact={compact} isDark={isDark}
            view={cardViews?.rl} history={cornerHistory.rl} />
          <WheelCard pos="Rear Right"  surface={latest?.tyre_temp_surface_rr ?? 0} inner={latest?.tyre_temp_inner_rr ?? 0} brake={latest?.brake_temp_rr ?? 0}
            wear={damage?.tyre_wear_rr ?? null} blisters={damage?.blisters_rr ?? null} noData={noData} compact={compact} isDark={isDark}
            view={cardViews?.rr} history={cornerHistory.rr} />
        </div>
      </div>
    </div>
  )
}
