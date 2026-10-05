import { memo, useEffect, useMemo, useRef, useState, type ComponentProps, type ReactNode } from 'react'
import { useTelemetryStore } from '../stores/telemetryStore'
import { useAppConfig } from '../hooks/useAppConfig'
import { useLabels } from '../lib/labels'
import { fmtMs } from '../lib/lapTimeFormat'
import { tyreCompoundColor } from '../lib/tyreCompounds'
import { StintTyreWearChart } from './TyreTrendCharts'
import StintChart, { type StintChartPoint } from './charts/StintChart'
import { ChartCoordinatesProvider, useLapBoundaries } from '../lib/chartCoordinates'
import { DATA_ROW } from '../lib/historyDependencies'
import { claimLapHistoryCar } from '../lib/lapHistoryCar'
import { StintStatusScan, inProgressLap, measureLaps } from '../lib/stintMeasures'
import Select from '../lib/AnimatedSelect'
import { buildSelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import { themeSeriesColor } from '../lib/themeColors'
import { StatCard } from './LiveStats'
import type { DensityMode } from '../lib/graphSections'

const missing = '—'
const lapX = (value: number) => `L${Math.round(value)}`
// Axis labels must fit the shared 42px tyre-chart gutter; tooltips keep full precision.
const lapAxisY = (value: number) => {
  const tenths = Math.round(value * 10)
  return `${Math.floor(tenths / 600)}:${((tenths % 600) / 10).toFixed(1).padStart(4, '0')}`
}
const lapTooltipY = (value: number) => fmtMs(Math.round(value * 1000))
const energyAxisY = (value: number) => `${value.toFixed(1)} MJ`
const energyTooltipY = (value: number) => `${value.toFixed(2)} MJ`
const lapSeries = [{ label: 'Lap time', color: '#5794F2' }]
const deploymentSeries = [{ label: 'Used', color: '#ffd700' }]
const rechargeSeries = [{ label: 'Recharged', color: '#37872D' }]
const rangeOptions = [{ value: 'stint' as const, label: 'Stint Laps' }, { value: 'all' as const, label: 'All Laps' }]
const wearOptions = [{ value: 'wear' as const, label: 'Wear' }, { value: 'life' as const, label: 'Life' }]
const chartSelectStyles = buildSelectStyles(true, { controlHeight: 22, menuWidth: '7rem' })

function Section({ title, children, controls, className = '' }: { title: string; children: ReactNode; controls?: ReactNode; className?: string }) {
  return <section className={`flex flex-col min-w-0 min-h-0 ${className}`} aria-label={title}>
    <div className="shrink-0 flex items-center justify-between gap-3 px-4 pt-3 pb-2">
      <h2 className="text-[10px] text-[var(--text-secondary)] uppercase tracking-widest">{title}</h2>
      {controls}
    </div>
    <div className="flex-1 min-h-0">{children}</div>
  </section>
}
function ChartSection({ title, controls, ...chart }: ComponentProps<typeof StintChart> & { controls?: ReactNode }) {
  const [hidden, setHidden] = useState<Record<string, boolean>>({})
  const series = useMemo(() => chart.series.map(item => ({ ...item, visible: !hidden[item.label] })), [chart.series, hidden])
  return <section className="chart-panel tyre-chart-container min-w-0 min-h-0 flex flex-col" aria-label={title}>
    <div className="tyre-chart-header flex h-[22px] items-center justify-between mb-2 shrink-0">
      <div className="tyre-chart-controls flex min-w-0 flex-1 items-center gap-0">
        <h2 className="chart-panel-title tyre-chart-title shrink-0 text-[10px] leading-none text-[var(--text-secondary)] uppercase tracking-widest">{title}</h2>
        {controls}
      </div>
      <div className="tyre-chart-legend flex shrink-0 items-center gap-3">
        {chart.series.map(series => <button key={series.label} type="button" aria-label={`Toggle ${series.label}`} aria-pressed={!hidden[series.label]}
          onClick={() => setHidden(previous => ({ ...previous, [series.label]: !previous[series.label] }))}
          className="flex items-center gap-1 cursor-pointer select-none" style={{ filter: hidden[series.label] ? 'grayscale(100%)' : undefined }}>
          <svg width="16" height="4" aria-hidden="true"><line x1="0" y1="2" x2="16" y2="2" stroke={themeSeriesColor(series.color, chart.isDark)} strokeWidth="2" /></svg>
          <span className="text-[9px] leading-none text-[var(--text-secondary)]">{series.label}</span>
        </button>)}
      </div>
    </div>
    <div className="flex-1 min-h-0"><StintChart {...chart} title={title} series={series} /></div>
  </section>
}
/** The Overview stat card, with a hover title and no unit beside a missing value. */
function Summary({ label, value, unit, title, color, sub, compact }: {
  label: string; value: string; unit?: string; title?: string; color?: string; sub?: string; compact: DensityMode
}) {
  return <div className="flex-1 min-w-0 flex" title={title}>
    <StatCard label={label} value={value} unit={value === missing ? undefined : unit} textColor={color}
      sub={compact === 'spacious' ? sub : undefined} compact={compact} />
  </div>
}

/** Mean of the measured values, with how many completed laps it covers. */
function average(values: readonly (number | null)[]): { value: string; title: string | undefined; coverage: string | undefined } {
  const measured = values.filter((value): value is number => value !== null)
  if (measured.length === 0) return { value: missing, title: undefined, coverage: undefined }
  const mean = measured.reduce((sum, value) => sum + value, 0) / measured.length
  return {
    value: mean.toFixed(2),
    title: measured.length < values.length ? `Measured on ${measured.length} of ${values.length} completed laps` : undefined,
    coverage: `${measured.length} of ${values.length} laps measured`,
  }
}

// The page follows the streamed driver: the player live, the driver selector's
// car in V6 playback. Every value comes from rows the store already holds.
export default memo(function StintPanel({ isDark, compact, wearMode, onWearModeChange }: { isDark: boolean; compact: DensityMode; wearMode: 'wear' | 'life'; onWearModeChange: (mode: 'wear' | 'life') => void }) {
  const { tn } = useLabels()
  const driver = useTelemetryStore(s => s.playbackDriverIndex ?? s.timing?.player_idx ?? null)
  const pushedHistory = useTelemetryStore(s => s.driverLapHistory)
  const currentLap = useTelemetryStore(s => s.lap?.lap_num ?? null)
  const status = useTelemetryStore(s => s.status)
  const damage = useTelemetryStore(s => s.damage)
  const tyreSets = useTelemetryStore(s => s.tyreSets)
  const statusHistory = useTelemetryStore(s => s.statusHistory)
  // Bumped when a backfill rewrites rows already held, such as V6 ERS fields
  // filled into earlier timestamps after a seek.
  const historyRevision = useTelemetryStore(s => s.analyzeLapRevision)
  const hasMguh = useTelemetryStore(s => s.protocolStatus?.capabilities.hasMguh ?? false)
  const boundaries = useLapBoundaries(true)
  const [range, setRange] = useAppConfig<'stint' | 'all'>('stintTyreRange', 'stint')

  // Completed laps and the stint boundary arrive as a push whenever they change.
  useEffect(() => driver === null ? undefined : claimLapHistoryCar(driver), [driver])
  const history = pushedHistory?.car_idx === driver ? pushedHistory : null
  const stintStart = history?.stint_start_lap || 1
  const stintLaps = useMemo(() => (history?.laps ?? []).filter(lap => lap.lap_num >= stintStart), [history, stintStart])

  // The status history grows in place every frame; scan only what is new, and
  // start again when held rows were rewritten. Rendering is driven by the
  // `status` subscription above.
  const scanRef = useRef({ revision: historyRevision, scan: new StintStatusScan() })
  /* eslint-disable react-hooks/refs */
  if (scanRef.current.revision !== historyRevision || !scanRef.current.scan.update(statusHistory)) {
    scanRef.current = { revision: historyRevision, scan: new StintStatusScan() }
    scanRef.current.scan.update(statusHistory)
  }
  const scan = scanRef.current.scan
  /* eslint-enable react-hooks/refs */
  const measures = measureLaps(scan, statusHistory, boundaries, stintLaps.map(lap => lap.lap_num), hasMguh)
  const lastCompleted = stintLaps.at(-1)?.lap_num ?? stintStart - 1
  const inProgress = currentLap !== null && currentLap > lastCompleted
    ? inProgressLap(scan, statusHistory, boundaries, currentLap, hasMguh)
    : null

  const fastestLap = stintLaps.reduce<(typeof stintLaps)[number] | null>((best, lap) =>
    lap.lap_valid && lap.lap_time_ms > 0 && (best === null || lap.lap_time_ms < best.lap_time_ms) ? lap : best, null)
  const fastest = fastestLap?.lap_time_ms ?? null
  const previous = stintLaps.at(-1) ?? null
  const deployedAverage = average(stintLaps.map(lap => measures.get(lap.lap_num)?.deployedMj ?? null))
  const harvestedAverage = average(stintLaps.map(lap => measures.get(lap.lap_num)?.harvestedMj ?? null))
  const fuelAverage = average(stintLaps.map(lap => measures.get(lap.lap_num)?.fuelKg ?? null))
  const wearPerLap = tyreSets?.sets.find(set => set.fitted)?.avg_wear_per_lap

  const lapPoints = useMemo<StintChartPoint[]>(() => stintLaps.map(lap => ({
    x: lap.lap_num, values: [lap.lap_time_ms > 0 ? lap.lap_time_ms / 1000 : null], label: `Lap ${lap.lap_num}`,
    invalid: lap.lap_time_ms > 0 && !lap.lap_valid, fastest: lap.lap_valid && lap.lap_time_ms === fastest,
  })), [stintLaps, fastest])
  const energyPoints = (pick: 'deployedMj' | 'harvestedMj'): StintChartPoint[] => {
    const points: StintChartPoint[] = stintLaps.map(lap => ({
      x: lap.lap_num, values: [measures.get(lap.lap_num)?.[pick] ?? null], label: `Lap ${lap.lap_num}`,
    }))
    if (inProgress && currentLap !== null)
      points.push({ x: currentLap, values: [inProgress[pick]], label: `Lap ${currentLap}`, provisional: true })
    return points
  }

  const tyreCompound = status?.tyre_compound ?? 0
  const damageCell = (key: 'wing_fl' | 'wing_fr' | 'wing_rear' | 'floor_damage' | 'sidepod_damage' | 'diffuser_damage' | 'engine_damage' | 'gearbox_damage', label: string, className = '') => {
    const value = damage?.[key]
    const shown = typeof value === 'number' && Number.isFinite(value) ? value : null
    return <div className={`min-w-0 min-h-0 flex flex-col justify-center bg-[var(--bg-card)] border border-[var(--border)] px-2 py-1 text-center ${className}`}>
      <div className="text-[9px] uppercase tracking-wider text-[var(--text-secondary)] mb-1">{label}</div>
      <div className="text-lg font-bold tabular-nums" style={{ color: shown === null ? 'var(--text-muted)' : shown > 0 ? '#C4162A' : isDark ? '#37872D' : '#137333' }}>
        {shown === null ? missing : `${shown}%`}
      </div>
    </div>
  }
  return <div className="h-full min-h-0 overflow-hidden bg-[var(--bg-panel)] border-t border-[var(--border)] flex flex-col">
    <div className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]">
      <Summary compact={compact} label="Stint Laps" value={history ? String(stintLaps.length) : missing} title={history ? `Current stint starts on lap ${stintStart}` : undefined}
        sub={history ? `From lap ${stintStart}` : undefined} />
      <Summary compact={compact} label="Wear/Lap" value={wearPerLap != null ? wearPerLap.toFixed(2) : missing} unit="%/L" title="Average wear per lap on the fitted set in this session; same calculation as the Tyres page."
        sub="Fitted set, this session" />
      <Summary compact={compact} label="Rec/Lap" value={harvestedAverage.value} title={harvestedAverage.title} unit="MJ/L" color={isDark ? '#37872D' : '#137333'} sub={harvestedAverage.coverage} />
      <Summary compact={compact} label="ERS/Lap" value={deployedAverage.value} title={deployedAverage.title} unit="MJ/L" color="var(--compound-medium)" sub={deployedAverage.coverage} />
      <Summary compact={compact} label="Fuel/Lap" value={fuelAverage.value} title={fuelAverage.title} unit="kg/L" sub={fuelAverage.coverage} />
      <Summary compact={compact} label="Fastest Lap" value={fastestLap ? fmtMs(fastestLap.lap_time_ms) : missing} title="Fastest valid lap in the current stint" color="var(--color-fastest)"
        sub={fastestLap ? `Lap ${fastestLap.lap_num}` : undefined} />
      <Summary compact={compact} label="Previous Lap" value={previous && previous.lap_time_ms > 0 ? fmtMs(previous.lap_time_ms) : missing} title={previous && !previous.lap_valid ? 'Previous completed lap · Invalid' : 'Previous completed lap'} color={previous && !previous.lap_valid ? '#C4162A' : undefined}
        sub={previous ? `Lap ${previous.lap_num}${previous.lap_valid ? '' : ' · Invalid'}` : undefined} />
      <Summary compact={compact} label="Tyre" value={tyreCompound > 0 ? tn('tyre.actual', tyreCompound) : missing} color={tyreCompound > 0 ? tyreCompoundColor(tyreCompound, status?.visual_compound ?? 0) : undefined}
        sub={status && Number.isFinite(status.tyre_age_laps) ? `${status.tyre_age_laps}L age` : undefined} />
    </div>
    <div className="flex-1 min-h-0 grid grid-cols-[minmax(0,1fr)_200px] xl:grid-cols-[minmax(0,1fr)_220px]">
      <div className="min-w-0 min-h-0 grid grid-rows-[minmax(0,1fr)_minmax(0,1fr)_minmax(0,1.1fr)] divide-y divide-[var(--border)]">
        <ChartSection title="Lap Times · Stint" points={lapPoints} series={lapSeries} isDark={isDark} discrete formatX={lapX} formatY={lapAxisY} formatTooltipY={lapTooltipY} />
        <div className="grid grid-cols-2 divide-x divide-[var(--border)] min-h-0">
          <ChartSection title="ERS Usage · Stint" points={energyPoints('deployedMj')} series={deploymentSeries} isDark={isDark} discrete zeroBaselineHeadroom={0.5} formatX={lapX} formatY={energyAxisY} formatTooltipY={energyTooltipY} />
          <ChartSection title="Recharge · Stint" points={energyPoints('harvestedMj')} series={rechargeSeries} isDark={isDark} discrete zeroBaselineHeadroom={0.5} formatX={lapX} formatY={energyAxisY} formatTooltipY={energyTooltipY} />
        </div>
        <ChartCoordinatesProvider mode={range === 'all' ? 'AL' : 'SL'} referenceLapNum={null} rowTypeMask={DATA_ROW.damage} sectorBoundaries={false} stintStartLap={history?.stint_start_lap || undefined}>
          <StintTyreWearChart isDark={isDark} wearMode={wearMode}
            controls={<div className="flex shrink-0 items-center gap-1 normal-case tracking-normal">
              <Select aria-label="Tyre graph range" options={rangeOptions} value={rangeOptions.find(option => option.value === range)} onChange={option => { if (option) setRange(option.value) }} styles={chartSelectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} menuPosition="fixed" menuShouldScrollIntoView={false} />
              <Select aria-label="Tyre wear or life" options={wearOptions} value={wearOptions.find(option => option.value === wearMode)} onChange={option => { if (option) onWearModeChange(option.value) }} styles={chartSelectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} menuPosition="fixed" menuShouldScrollIntoView={false} />
            </div>} />
        </ChartCoordinatesProvider>
      </div>
      <aside className="min-h-0 border-l border-[var(--border)] grid grid-rows-[minmax(0,2fr)_minmax(0,1fr)]" aria-label="Car condition">
        <Section title="Damage">
          <div className="h-full min-h-0 grid grid-cols-2 grid-rows-[repeat(4,minmax(0,1fr))] gap-2 px-3 pt-1 pb-3">
            {damageCell('wing_fl', 'Wing L')}{damageCell('wing_fr', 'Wing R')}
            {damageCell('floor_damage', 'Floor', 'col-span-2 mx-6')}
            {damageCell('sidepod_damage', 'Sidepod')}{damageCell('diffuser_damage', 'Diffuser')}
            {damageCell('wing_rear', 'Rear Wing', 'col-span-2 mx-6')}
          </div>
        </Section>
        <Section title="Engine Wear" className="border-t border-[var(--border)]">
          <div className="h-full min-h-0 grid grid-rows-2 gap-2 px-3 pt-1 pb-3">{damageCell('engine_damage', 'Engine')}{damageCell('gearbox_damage', 'Gearbox')}</div>
        </Section>
      </aside>
    </div>
  </div>
})
