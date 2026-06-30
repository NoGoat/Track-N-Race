import { memo } from 'react'
import type { TelemetryRow, DamageRow } from '../types'
import TyreTrendCharts from './TyreTrendCharts'
import { useSize } from '../hooks/useSize'
import { useColorFn } from '../lib/cards'

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
}

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
  pos, surface, inner, brake, wear, blisters, noData, compact, isDark = true,
}: {
  pos: string; surface: number; inner: number; brake: number
  wear: number | null; blisters: number | null; noData?: boolean; compact?: boolean; isDark?: boolean
}) {
  const ramp = useColorFn(null, null, isDark)
  return (
    <div className={`flex-1 min-w-0 overflow-hidden flex flex-col justify-between ${compact ? 'p-2' : 'p-4'}`}>
      <div className={`text-[10px] text-[var(--text-secondary)] font-bold uppercase tracking-widest ${compact ? 'mb-1' : 'mb-3'}`}>
        {pos}
      </div>
      <TempRow label="Surface" value={surface} color={ramp('temp.tyre', surface) ?? '#888'} noData={noData} compact={compact} />
      <TempRow label="Inner"   value={inner}   color={ramp('temp.tyre', inner) ?? '#888'}   noData={noData} compact={compact} />
      {!compact && <div className="my-1.5 border-t border-[var(--border)]" />}
      <TempRow label="Brake"   value={brake}   color={ramp('temp.brake', brake) ?? '#888'}  noData={noData} compact={compact} />
      <WearBar pct={wear} blisters={blisters} noData={noData} compact={compact} isDark={isDark} />
    </div>
  )
})


const ThermalPanel = memo(function ThermalPanel({ latest, damage, telemetry, damageHistory, view, tyreWearMode, thermalGraphs, thermalCards, isDark }: Props) {
  const { ref: cardsRef, height: cardsHeight } = useSize()
  const compact = cardsHeight > 0 && cardsHeight < 200

  const sf = { fl: latest?.tyre_temp_surface_fl ?? 0, fr: latest?.tyre_temp_surface_fr ?? 0, rl: latest?.tyre_temp_surface_rl ?? 0, rr: latest?.tyre_temp_surface_rr ?? 0 }
  const inn = { fl: latest?.tyre_temp_inner_fl   ?? 0, fr: latest?.tyre_temp_inner_fr   ?? 0, rl: latest?.tyre_temp_inner_rl   ?? 0, rr: latest?.tyre_temp_inner_rr   ?? 0 }
  const brk = { fl: latest?.brake_temp_fl        ?? 0, fr: latest?.brake_temp_fr        ?? 0, rl: latest?.brake_temp_rl        ?? 0, rr: latest?.brake_temp_rr        ?? 0 }

  return (
    <div className="h-full">
      {view === 'graphs' ? (
        <div className="h-full">
          <TyreTrendCharts telemetry={telemetry} damageHistory={damageHistory} tyreWearMode={tyreWearMode} visibleGraphs={thermalGraphs} isDark={isDark} />
        </div>
      ) : (
        <div ref={cardsRef} className="flex h-full divide-x divide-[var(--border)]">
          {thermalCards.fl && <WheelCard pos="Front Left"  surface={sf.fl}  inner={inn.fl}  brake={brk.fl}
            wear={damage?.tyre_wear_fl ?? null} blisters={damage?.blisters_fl ?? null} noData={!latest} compact={compact} isDark={isDark} />}
          {thermalCards.fr && <WheelCard pos="Front Right" surface={sf.fr}  inner={inn.fr}  brake={brk.fr}
            wear={damage?.tyre_wear_fr ?? null} blisters={damage?.blisters_fr ?? null} noData={!latest} compact={compact} isDark={isDark} />}
          {thermalCards.rl && <WheelCard pos="Rear Left"   surface={sf.rl}  inner={inn.rl}  brake={brk.rl}
            wear={damage?.tyre_wear_rl ?? null} blisters={damage?.blisters_rl ?? null} noData={!latest} compact={compact} isDark={isDark} />}
          {thermalCards.rr && <WheelCard pos="Rear Right"  surface={sf.rr}  inner={inn.rr}  brake={brk.rr}
            wear={damage?.tyre_wear_rr ?? null} blisters={damage?.blisters_rr ?? null} noData={!latest} compact={compact} isDark={isDark} />}
        </div>
      )}
    </div>
  )
})
export default ThermalPanel
