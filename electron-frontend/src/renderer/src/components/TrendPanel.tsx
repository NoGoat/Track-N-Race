import { memo, useEffect, useMemo, useState, type ComponentProps, type ReactNode } from 'react'
import { useTelemetryStore } from '../stores/telemetryStore'
import { useAppConfig } from '../hooks/useAppConfig'
import { useLabels } from '../lib/labels'
import { fmtMs } from '../lib/lapTimeFormat'
import { tyreCompoundColor } from '../lib/tyreCompounds'
import { StintTyreWearChart, cornerColors } from './TyreTrendCharts'
import TrendChart, { quarterAxisRange, type TrendAxis, type TrendChartPoint } from './charts/TrendChart'
import { ChartCoordinatesProvider, lapStartSessionTime, useLapBoundaries } from '../lib/chartCoordinates'
import { ChartCursorSyncProvider } from '../lib/chartCursorSync'
import { DATA_ROW } from '../lib/historyDependencies'
import { claimLapHistoryCar } from '../lib/lapHistoryCar'
import { StintStatusScanner, measureLaps } from '../lib/trendMeasures'
import Select from '../lib/AnimatedSelect'
import { buildSelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import { themeSeriesColor } from '../lib/themeColors'
import { StatCard } from './LiveStats'
import type { DensityMode, YAxisBehavior } from '../lib/graphSections'
import type { TrendsLayout, TrendsPageLayout } from '../app/appConfig'
import type { ColumnView } from '../lib/columnStore'
import type { DamageRow } from '../types'

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
const energyAxisLabel = (value: number) => `${+value.toFixed(2)} MJ`
const percentAxisLabel = (value: number) => `${+value.toFixed(1)}%`
const percentTooltip = (value: number) => `${value.toFixed(1)}%`
// Kept apart from each other and from the four tyre corner colours, which
// share the page and, in the combined layout, the plot. Checked with the dataviz
// palette validator: in dark mode every pair differs by at least 15 OKLab ΔE in
// normal vision and 9.1 under protanopia/deuteranopia. Light mode's recharge
// sits 6.9 from ERS used under those, so the legend and tooltip labels carry it.
function trendColors(isDark: boolean) {
  return isDark
    ? { lapTime: '#B0E0FF', used: '#FF9830', recharged: '#FF6EC7' }
    : { lapTime: '#004D61', used: '#4A148C', recharged: '#880E4F' }
}
const CORNERS = ['fl', 'fr', 'rl', 'rr'] as const
type LapRange = 'stint' | 'all'
const rangeOptions = [{ value: 'stint' as const, label: 'Stint Laps' }, { value: 'all' as const, label: 'All Laps' }]
const chartSelectStyles = buildSelectStyles(true, { controlHeight: 22, menuWidth: '7rem' })
// Shared-tooltip order: top to bottom, ahead of the tyre graph.
const lapTimeSync = { id: 'trendsLapTime', order: 1 }
const deploymentSync = { id: 'trendsErsUsage', order: 2 }
const rechargeSync = { id: 'trendsRecharge', order: 3 }

function RangeSelect({ label, range, onChange }: { label: string; range: LapRange; onChange: (range: LapRange) => void }) {
  return <div className="flex shrink-0 items-center gap-1 normal-case tracking-normal">
    <Select aria-label={label} options={rangeOptions} value={rangeOptions.find(option => option.value === range)} onChange={option => { if (option) onChange(option.value) }} styles={chartSelectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} menuPosition="fixed" menuShouldScrollIntoView={false} />
  </div>
}

function ChartSection({ title, controls, ...chart }: ComponentProps<typeof TrendChart> & { controls?: ReactNode }) {
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
    <div className="flex-1 min-h-0"><TrendChart {...chart} title={title} series={series} /></div>
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

// The Trends page follows the streamed driver: the player live, the driver
// selector's car in V6 playback. Every value comes from rows the store already
// holds. The summary covers the current stint; each graph can show all laps.
export default memo(function TrendPanel({ isDark, compact, layout, visible, wearMode, tyreYAxis, cursorSyncEnabled, secondaryHorizontalCrosshair, secondaryVerticalCrosshair }: {
  isDark: boolean; compact: DensityMode; layout: TrendsPageLayout; visible: TrendsLayout; wearMode: 'wear' | 'life'; tyreYAxis: YAxisBehavior
  cursorSyncEnabled: boolean; secondaryHorizontalCrosshair: boolean; secondaryVerticalCrosshair: boolean
}) {
  const { tn } = useLabels()
  const driver = useTelemetryStore(s => s.playbackDriverIndex ?? s.timing?.player_idx ?? null)
  const pushedHistory = useTelemetryStore(s => s.driverLapHistory)
  const currentLap = useTelemetryStore(s => s.lap?.lap_num ?? null)
  const status = useTelemetryStore(s => s.status)
  const tyreSets = useTelemetryStore(s => s.tyreSets)
  const statusHistory = useTelemetryStore(s => s.statusHistory)
  // Bumped when a backfill rewrites rows already held, such as V6 ERS fields
  // filled into earlier timestamps after a seek.
  const historyRevision = useTelemetryStore(s => s.analyzeLapRevision)
  const hasMguh = useTelemetryStore(s => s.protocolStatus?.capabilities.hasMguh ?? false)
  const boundaries = useLapBoundaries(true)
  const [lapRange, setLapRange] = useAppConfig<LapRange>('trendsLapTimeRange', 'stint')
  const [deploymentRange, setDeploymentRange] = useAppConfig<LapRange>('trendsErsUsageRange', 'stint')
  const [rechargeRange, setRechargeRange] = useAppConfig<LapRange>('trendsRechargeRange', 'stint')
  const [tyreRange, setTyreRange] = useAppConfig<LapRange>('stintTyreRange', 'stint')
  const [combinedRange, setCombinedRange] = useAppConfig<LapRange>('trendsCombinedRange', 'stint')
  const combined = layout === 'combined' || layout === 'combinedNoRecharge'
  const bars = layout === 'bars'
  const lapCharts = combined || bars
  const showRecharge = layout !== 'combinedNoRecharge'
  // The combined and bar graphs read the tyre history here; the separate
  // layout's tyre graph reads it itself.
  const damageHistory = useTelemetryStore(s => lapCharts ? s.damageHistory : null)
  const colors = trendColors(isDark)
  const lapSeries = useMemo(() => [{ label: 'Lap time', color: colors.lapTime }], [colors.lapTime])
  const deploymentSeries = useMemo(() => [{ label: 'Used', color: colors.used }], [colors.used])
  const rechargeSeries = useMemo(() => [{ label: 'Recharged', color: colors.recharged }], [colors.recharged])

  // Completed laps and the stint boundary arrive as a push whenever they change.
  useEffect(() => driver === null ? undefined : claimLapHistoryCar(driver), [driver])
  const history = pushedHistory?.car_idx === driver ? pushedHistory : null
  const stintStart = history?.stint_start_lap || 1
  const allLaps = useMemo(() => history?.laps ?? [], [history])
  const stintLaps = useMemo(() => allLaps.filter(lap => lap.lap_num >= stintStart), [allLaps, stintStart])
  const lapsFor = (range: LapRange) => range === 'all' ? allLaps : stintLaps

  // The status history grows in place every frame; scan only what is new, and
  // start again when held rows were rewritten. Rendering is driven by the
  // `status` subscription above. Not memoized on the history: in All Laps mode
  // it is the table's live view, whose identity never changes as it grows.
  const [scanner] = useState(() => new StintStatusScanner())
  const scan = scanner.scanFor(statusHistory, historyRevision)
  const measures = measureLaps(scan, statusHistory, boundaries, allLaps.map(lap => lap.lap_num), hasMguh)

  const fastestLap = stintLaps.reduce<(typeof stintLaps)[number] | null>((best, lap) =>
    lap.lap_valid && lap.lap_time_ms > 0 && (best === null || lap.lap_time_ms < best.lap_time_ms) ? lap : best, null)
  const previous = stintLaps.at(-1) ?? null
  const deployedAverage = average(stintLaps.map(lap => measures.get(lap.lap_num)?.deployedMj ?? null))
  const harvestedAverage = average(stintLaps.map(lap => measures.get(lap.lap_num)?.harvestedMj ?? null))
  const fuelAverage = average(stintLaps.map(lap => measures.get(lap.lap_num)?.fuelKg ?? null))
  const wearPerLap = tyreSets?.sets.find(set => set.fitted)?.avg_wear_per_lap

  // Fastest marks the quickest valid lap among those the graph shows.
  const lapPointLaps = lapsFor(lapRange)
  const lapPoints = useMemo<TrendChartPoint[]>(() => {
    const shownFastest = lapPointLaps.reduce<number | null>((best, lap) =>
      lap.lap_valid && lap.lap_time_ms > 0 && (best === null || lap.lap_time_ms < best) ? lap.lap_time_ms : best, null)
    return lapPointLaps.map(lap => ({
      x: lap.lap_num, values: [lap.lap_time_ms > 0 ? lap.lap_time_ms / 1000 : null], label: `Lap ${lap.lap_num}`,
      invalid: lap.lap_time_ms > 0 && !lap.lap_valid, fastest: lap.lap_valid && lap.lap_time_ms === shownFastest,
      lapStart: lapStartSessionTime(boundaries, lap.lap_num),
    }))
  }, [lapPointLaps, boundaries])

  // The page renders on every status frame, but the combined points change only
  // when a lap's measures or the tyre history do: rebuild them only then, so the
  // chart is not handed a new dataset every frame. The tyre history can grow in
  // place, so it is keyed by its extent rather than its identity.
  const measuresKey = allLaps.map(lap => { const m = measures.get(lap.lap_num); return `${m?.deployedMj}:${m?.harvestedMj}` }).join('|')
  const damageKey = damageHistory && damageHistory.length > 0
    ? `${damageHistory.length}:${damageHistory.time(0)}:${damageHistory.time(damageHistory.length - 1)}:${historyRevision}`
    : ''
  const heldMeasures = useHeldByKey({ measures }, measuresKey)
  const deploymentLaps = lapsFor(deploymentRange)
  const deploymentPoints = useMemo(() => energyPoints('deployedMj', deploymentLaps, heldMeasures.measures, boundaries),
    [deploymentLaps, heldMeasures, boundaries])
  const rechargeLaps = lapsFor(rechargeRange)
  const rechargePoints = useMemo(() => energyPoints('harvestedMj', rechargeLaps, heldMeasures.measures, boundaries),
    [rechargeLaps, heldMeasures, boundaries])
  const heldDamage = useHeldByKey({ rows: damageHistory }, damageKey)
  const combinedLaps = lapsFor(combinedRange)
  const combinedFirstLap = combinedRange === 'all' ? allLaps[0]?.lap_num ?? 1 : stintStart
  const combinedLapPoints = useMemo(() => combined ? buildLapEndPoints(combinedLaps, heldMeasures.measures) : NO_POINTS,
    [combined, combinedLaps, heldMeasures])
  const combinedTyrePoints = useMemo(() => combined
    ? buildTyreSamples(combinedLaps, combinedFirstLap, currentLap, boundaries, heldDamage.rows, wearMode)
    : NO_POINTS,
  [combined, combinedLaps, combinedFirstLap, currentLap, boundaries, heldDamage, wearMode])
  const combinedAxes = useCombinedAxes(combined ? combinedLapPoints : null, combinedTyrePoints, showRecharge, colors.lapTime, tyreYAxis)
  const barPoints = useMemo(() => bars ? buildBarPoints(combinedLaps, heldMeasures.measures, boundaries, heldDamage.rows, wearMode) : NO_POINTS,
    [bars, combinedLaps, heldMeasures, boundaries, heldDamage, wearMode])
  const barAxes = useBarAxes(bars ? barPoints : null, colors.lapTime, tyreYAxis)
  // Left to right in each lap: the four tyres stacked FL to RR from the
  // bottom, ERS used, lap time.
  const barSeries = useMemo(() => {
    const corners = cornerColors(isDark)
    return [
      ...CORNERS.map((corner, channel) => ({ label: corner.toUpperCase(), color: corners[corner], axis: 2, channel, stack: 'tyres' })),
      { label: 'Used', color: colors.used, axis: 1, channel: 4 },
      { label: 'Lap time', color: colors.lapTime, axis: 0, channel: 5 },
    ]
  }, [isDark, colors.used, colors.lapTime])
  const combinedSeries = useMemo(() => {
    const corners = cornerColors(isDark)
    // Channels follow the lap points' values: used, recharged, lap time.
    return [
      { label: 'Used', color: colors.used, axis: 1, perLap: true, channel: 0 },
      ...showRecharge ? [{ label: 'Recharged', color: colors.recharged, axis: 1, perLap: true, channel: 1 }] : [],
      { label: 'Lap time', color: colors.lapTime, axis: 0, perLap: true, channel: 2 },
      ...CORNERS.map(corner => ({ label: corner.toUpperCase(), color: corners[corner], axis: 2 })),
    ]
  }, [isDark, showRecharge, colors.lapTime, colors.used, colors.recharged])

  const tyreCompound = status?.tyre_compound ?? 0
  const cards = visible.statsCards
  const charts = visible.charts
  // The Separate layout's rows hold only the charts the Layout Editor shows.
  const ersCharts = Number(charts.ersUsage) + Number(charts.recharge)
  const chartRows = [charts.lapTimes && '1fr', ersCharts > 0 && '1fr', charts.tyreWear && '1.1fr']
    .filter((row): row is string => row !== false).map(row => `minmax(0,${row})`).join(' ')
  return <div className="h-full min-h-0 overflow-hidden bg-[var(--bg-panel)] border-t border-[var(--border)] flex flex-col">
    {Object.values(cards).some(Boolean) && <div className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]">
      {cards.stintLaps && <Summary compact={compact} label="Stint Laps" value={history ? String(stintLaps.length) : missing} title={history ? `Current stint starts on lap ${stintStart}` : undefined}
        sub={history ? `From lap ${stintStart}` : undefined} />}
      {cards.wearPerLap && <Summary compact={compact} label="Wear/Lap" value={wearPerLap != null ? wearPerLap.toFixed(2) : missing} unit="%/L" title="Average wear per lap on the fitted set in this session; same calculation as the Tyres page."
        sub="Fitted set, this session" />}
      {cards.recPerLap && <Summary compact={compact} label="Rec/Lap" value={harvestedAverage.value} title={harvestedAverage.title} unit="MJ/L" color={colors.recharged} sub={harvestedAverage.coverage} />}
      {cards.ersPerLap && <Summary compact={compact} label="ERS/Lap" value={deployedAverage.value} title={deployedAverage.title} unit="MJ/L" color={colors.used} sub={deployedAverage.coverage} />}
      {cards.fuelPerLap && <Summary compact={compact} label="Fuel/Lap" value={fuelAverage.value} title={fuelAverage.title} unit="kg/L" sub={fuelAverage.coverage} />}
      {cards.fastestLap && <Summary compact={compact} label="Fastest Lap" value={fastestLap ? fmtMs(fastestLap.lap_time_ms) : missing} title="Fastest valid lap in the current stint" color="var(--color-fastest)"
        sub={fastestLap ? `Lap ${fastestLap.lap_num}` : undefined} />}
      {cards.previousLap && <Summary compact={compact} label="Previous Lap" value={previous && previous.lap_time_ms > 0 ? fmtMs(previous.lap_time_ms) : missing} title={previous && !previous.lap_valid ? 'Previous completed lap · Invalid' : 'Previous completed lap'} color={previous && !previous.lap_valid ? '#C4162A' : undefined}
        sub={previous ? `Lap ${previous.lap_num}${previous.lap_valid ? '' : ' · Invalid'}` : undefined} />}
      {cards.tyre && <Summary compact={compact} label="Tyre" value={tyreCompound > 0 ? tn('tyre.actual', tyreCompound) : missing} color={tyreCompound > 0 ? tyreCompoundColor(tyreCompound, status?.visual_compound ?? 0) : undefined}
        sub={status && Number.isFinite(status.tyre_age_laps) ? `${status.tyre_age_laps}L age` : undefined} />}
    </div>}
    {bars ? <div className="flex-1 min-w-0 min-h-0 grid grid-rows-[minmax(0,1fr)]">
      {charts.bars && <ChartSection title={wearMode === 'life' ? 'Tyre Life · ERS · Lap Times' : 'Tyre Wear · ERS · Lap Times'} points={barPoints} series={barSeries} axes={barAxes}
        isDark={isDark} bars discrete formatX={lapX} formatY={lapAxisY}
        controls={<RangeSelect label="Bar graph range" range={combinedRange} onChange={setCombinedRange} />} />}
    </div> : combined ? <div className="flex-1 min-w-0 min-h-0 grid grid-rows-[minmax(0,1fr)]">
      {charts.combined && <ChartSection title={wearMode === 'life' ? 'Lap Times · ERS · Tyre Life' : 'Lap Times · ERS · Tyre Wear'} points={combinedTyrePoints} lapPoints={combinedLapPoints} series={combinedSeries} axes={combinedAxes}
        isDark={isDark} formatX={lapX} formatY={lapAxisY}
        controls={<RangeSelect label="Combined graph range" range={combinedRange} onChange={setCombinedRange} />} />}
    </div> : <div className="flex-1 min-w-0 min-h-0">
      <ChartCursorSyncProvider enabled={cursorSyncEnabled} secondaryHorizontalCrosshair={secondaryHorizontalCrosshair} secondaryVerticalCrosshair={secondaryVerticalCrosshair}>
        <div className="h-full min-w-0 min-h-0 grid divide-y divide-[var(--border)]" style={{ gridTemplateRows: chartRows }}>
          {charts.lapTimes && <ChartSection title="Lap Times" points={lapPoints} series={lapSeries} isDark={isDark} discrete formatX={lapX} formatY={lapAxisY} formatTooltipY={lapTooltipY} cursorSync={lapTimeSync}
            controls={<RangeSelect label="Lap times range" range={lapRange} onChange={setLapRange} />} />}
          {ersCharts > 0 && <div className={`grid ${ersCharts === 2 ? 'grid-cols-2' : 'grid-cols-1'} divide-x divide-[var(--border)] min-h-0`}>
            {charts.ersUsage && <ChartSection title="ERS Usage" points={deploymentPoints} series={deploymentSeries} isDark={isDark} discrete zeroBaselineHeadroom={0.5} formatX={lapX} formatY={energyAxisY} formatTooltipY={energyTooltipY} cursorSync={deploymentSync}
              controls={<RangeSelect label="ERS usage range" range={deploymentRange} onChange={setDeploymentRange} />} />}
            {charts.recharge && <ChartSection title="Recharge" points={rechargePoints} series={rechargeSeries} isDark={isDark} discrete zeroBaselineHeadroom={0.5} formatX={lapX} formatY={energyAxisY} formatTooltipY={energyTooltipY} cursorSync={rechargeSync}
              controls={<RangeSelect label="Recharge range" range={rechargeRange} onChange={setRechargeRange} />} />}
          </div>}
          {charts.tyreWear && <ChartCoordinatesProvider mode={tyreRange === 'all' ? 'AL' : 'SL'} referenceLapNum={null} rowTypeMask={DATA_ROW.damage} sectorBoundaries={false} stintStartLap={history?.stint_start_lap || undefined}>
            <StintTyreWearChart isDark={isDark} wearMode={wearMode} yAxis={tyreYAxis}
              controls={<RangeSelect label="Tyre graph range" range={tyreRange} onChange={setTyreRange} />} />
          </ChartCoordinatesProvider>}
        </div>
      </ChartCursorSyncProvider>
    </div>}
  </div>
})

// Tyre samples kept per lap: the line stays live without carrying every
// damage packet of the session into the chart.
const TYRE_SAMPLES_PER_LAP = 40
const FALLBACK_LAP_S = 90
const NO_POINTS: TrendChartPoint[] = []

/** The value last held under `key`: a new key replaces it, the same key keeps the old one. */
function useHeldByKey<T>(value: T, key: string): T {
  const [held, setHeld] = useState({ key, value })
  if (held.key !== key) {
    setHeld({ key, value })
    return value
  }
  return held.value
}

/** ERS used or recharged per completed lap; it gains a point only when a lap completes, like lap time. */
function energyPoints(pick: 'deployedMj' | 'harvestedMj', laps: readonly { lap_num: number }[],
  measures: ReturnType<typeof measureLaps>, boundaries: ReturnType<typeof useLapBoundaries>): TrendChartPoint[] {
  return laps.map(lap => ({
    x: lap.lap_num, values: [measures.get(lap.lap_num)?.[pick] ?? null], label: `Lap ${lap.lap_num}`,
    lapStart: lapStartSessionTime(boundaries, lap.lap_num),
  }))
}

/**
 * The combined graph's per-lap values, one point per completed lap at x = N,
 * the line closing lap N: ERS used, ERS recharged and lap time. The chart joins
 * them directly; nothing is plotted for a lap until it is complete.
 */
function buildLapEndPoints(laps: readonly { lap_num: number; lap_time_ms: number; lap_valid: boolean }[],
  measures: ReturnType<typeof measureLaps>): TrendChartPoint[] {
  const fastest = laps.reduce<number | null>((best, lap) =>
    lap.lap_valid && lap.lap_time_ms > 0 && (best === null || lap.lap_time_ms < best) ? lap.lap_time_ms : best, null)
  return laps.map(lap => {
    const measure = measures.get(lap.lap_num)
    return {
      x: lap.lap_num, label: `Lap ${lap.lap_num}`,
      values: [measure?.deployedMj ?? null, measure?.harvestedMj ?? null, lap.lap_time_ms > 0 ? lap.lap_time_ms / 1000 : null],
      invalid: lap.lap_time_ms > 0 && !lap.lap_valid, fastest: lap.lap_valid && lap.lap_time_ms === fastest,
    }
  })
}

/**
 * The combined graph's live tyre wear (or life): a sample taken partway through
 * lap N sits at N - 1 plus the fraction of the lap elapsed, so lap N's closing
 * line is at x = N. The lap in progress is spaced by the previous lap's time.
 */
function buildTyreSamples(laps: readonly { lap_time_ms: number }[], firstLap: number, currentLap: number | null,
  boundaries: ReturnType<typeof useLapBoundaries>, rows: ColumnView<DamageRow> | null, wearMode: 'wear' | 'life'): TrendChartPoint[] {
  if (!rows || rows.length === 0 || currentLap === null) return NO_POINTS
  // Lap start times in one pass; a reused lap number keeps its latest attempt.
  const starts = new Map<number, number>()
  for (const boundary of boundaries) starts.set(boundary.lapNum, boundary.sessionTime)
  const lastLapTime = laps.at(-1)?.lap_time_ms
  const estimatedLapS = lastLapTime && lastLapTime > 0 ? lastLapTime / 1000 : FALLBACK_LAP_S
  const points: TrendChartPoint[] = []
  let previousIndex = -1
  for (let lap = firstLap; lap <= currentLap; lap++) {
    const start = starts.get(lap)
    if (start === undefined) continue
    const end = starts.get(lap + 1)
    const duration = end !== undefined ? end - start : estimatedLapS
    // The newest sample in each slice of the lap stands for it; a binary
    // search finds it, so the cost does not grow with the packet rate.
    for (let slice = 1; slice <= TYRE_SAMPLES_PER_LAP; slice++) {
      // The lap in progress may run longer than estimated: its last slice is open.
      const sliceEnd = slice === TYRE_SAMPLES_PER_LAP ? end ?? Infinity : start + duration * slice / TYRE_SAMPLES_PER_LAP
      const index = rows.lowerBound(sliceEnd, true) - 1
      if (index <= previousIndex || index < 0) continue
      const time = rows.time(index)
      if (time < start) continue
      previousIndex = index
      points.push({
        x: lap - 1 + Math.min((time - start) / duration, 0.999), label: `Lap ${lap}`, lap,
        values: CORNERS.map(corner => {
          const wear = rows.num(`tyre_wear_${corner}`, index)
          return Number.isFinite(wear) ? wearMode === 'life' ? 100 - wear : wear : null
        }),
      })
    }
  }
  return points
}

/**
 * The bar graph's points, one per completed lap at x = lap number: each
 * corner's tyre wear (or life) at the lap's closing line, ERS used, and lap time.
 */
function buildBarPoints(laps: readonly { lap_num: number; lap_time_ms: number; lap_valid: boolean }[],
  measures: ReturnType<typeof measureLaps>, boundaries: ReturnType<typeof useLapBoundaries>,
  rows: ColumnView<DamageRow> | null, wearMode: 'wear' | 'life'): TrendChartPoint[] {
  const starts = new Map<number, number>()
  for (const boundary of boundaries) starts.set(boundary.lapNum, boundary.sessionTime)
  const fastest = laps.reduce<number | null>((best, lap) =>
    lap.lap_valid && lap.lap_time_ms > 0 && (best === null || lap.lap_time_ms < best) ? lap.lap_time_ms : best, null)
  return laps.map(lap => {
    const end = starts.get(lap.lap_num + 1)
    const index = rows && end !== undefined ? rows.lowerBound(end, true) - 1 : -1
    const wear = CORNERS.map(corner => {
      const value = rows && index >= 0 ? rows.num(`tyre_wear_${corner}`, index) : NaN
      return Number.isFinite(value) ? wearMode === 'life' ? 100 - value : value : null
    })
    return {
      x: lap.lap_num, label: `Lap ${lap.lap_num}`,
      values: [...wear, measures.get(lap.lap_num)?.deployedMj ?? null, lap.lap_time_ms > 0 ? lap.lap_time_ms / 1000 : null],
      invalid: lap.lap_time_ms > 0 && !lap.lap_valid, fastest: lap.lap_valid && lap.lap_time_ms === fastest,
    }
  })
}

/** The bar graph's scales: lap time on the left, then MJ and tyre %. */
function useBarAxes(points: readonly TrendChartPoint[] | null, lapColor: string, tyreYAxis: YAxisBehavior): TrendAxis[] | undefined {
  const { wear, energy, laps } = useMemo(() => {
    // The tyre scale fits the tallest stack: the four corners together.
    const highest = (from: number, to: number) => {
      let hi = -Infinity
      for (const point of points ?? []) {
        let sum = 0, any = false
        for (let channel = from; channel < to; channel++) {
          const value = point.values[channel]
          if (value != null && Number.isFinite(value)) { sum += value; any = true }
        }
        if (any) hi = Math.max(hi, sum)
      }
      return hi
    }
    return { wear: highest(0, 4), energy: highest(4, 5), laps: highest(5, 6) }
  }, [points])
  const lapMax = Number.isFinite(laps) ? laps + 3 : 1
  const energyMax = Number.isFinite(energy) ? energy + 0.5 : 1
  // Fixed is each tyre's 0-100% scale, so four stacked tyres span 0-400%.
  const wearMax = tyreYAxis === 'fixed' || !Number.isFinite(wear) ? 400 : quarterAxisRange(0, wear, 0).max
  const enabled = points !== null
  return useMemo(() => enabled ? [
    { min: 0, max: lapMax, format: lapAxisY, formatTooltip: lapTooltipY, color: lapColor },
    { min: 0, max: energyMax, format: energyAxisLabel, formatTooltip: energyTooltipY },
    { min: 0, max: wearMax, format: percentAxisLabel, formatTooltip: percentTooltip },
  ] : undefined, [enabled, lapMax, energyMax, wearMax, lapColor])
}

/** The combined graph's three scales: lap time on the left, then MJ and tyre %. */
function useCombinedAxes(lapPoints: readonly TrendChartPoint[] | null, tyrePoints: readonly TrendChartPoint[], showRecharge: boolean, lapColor: string, tyreYAxis: YAxisBehavior): TrendAxis[] | undefined {
  const { laps, energy, tyres } = useMemo(() => {
    const extent = (points: readonly TrendChartPoint[], from: number, to: number) => {
      let lo = Infinity, hi = -Infinity
      for (const point of points) for (let i = from; i < to; i++) {
        const value = point.values[i]
        if (value != null && Number.isFinite(value)) { lo = Math.min(lo, value); hi = Math.max(hi, value) }
      }
      return { lo, hi }
    }
    // Without recharge, the MJ scale fits ERS used alone.
    return { energy: extent(lapPoints ?? [], 0, showRecharge ? 2 : 1), laps: extent(lapPoints ?? [], 2, 3), tyres: extent(tyrePoints, 0, 4) }
  }, [lapPoints, tyrePoints, showRecharge])
  // Lap time is read from 0:00.000 to 3 s above the slowest lap.
  const lapMin = 0
  const lapMax = Number.isFinite(laps.hi) ? laps.hi + 3 : 1
  // Energy is read from 0 MJ to 0.5 MJ above the highest of ERS used and recharged.
  const energyMin = 0
  const energyMax = Number.isFinite(energy.hi) ? energy.hi + 0.5 : 1
  const { min: tyreMin, max: tyreMax } = tyreYAxis === 'fixed' || !Number.isFinite(tyres.lo) ? { min: 0, max: 100 } : quarterAxisRange(tyres.lo, tyres.hi)
  const enabled = lapPoints !== null
  // Built from the range numbers so an unchanged scale keeps its identity between frames.
  return useMemo(() => enabled ? [
    { min: lapMin, max: lapMax, format: lapAxisY, formatTooltip: lapTooltipY, color: lapColor },
    { min: energyMin, max: energyMax, format: energyAxisLabel, formatTooltip: energyTooltipY },
    { min: tyreMin, max: tyreMax, format: percentAxisLabel, formatTooltip: percentTooltip },
  ] : undefined, [enabled, lapMin, lapMax, energyMin, energyMax, tyreMin, tyreMax, lapColor])
}
