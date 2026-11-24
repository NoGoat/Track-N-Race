import { memo, useEffect, useMemo, useState, type ComponentProps, type ReactNode } from 'react'
import { useTelemetryStore } from '../stores/telemetryStore'
import { useAppConfig } from '../hooks/useAppConfig'
import { useLabels } from '../lib/labels'
import { fmtMs } from '../lib/lapTimeFormat'
import { tyreCompoundColor } from '../lib/tyreCompounds'
import { StintTyreWearChart, cornerColors } from './TyreTrendCharts'
import TrendChart, { quarterAxisRange, type TrendAxis, type TrendChartPoint } from './charts/TrendChart'
import { ChartCoordinatesProvider, lapStartSessionTime, useLapBoundaries } from '../lib/chartCoordinates'
import { ChartCursorSyncFileProvider, ChartCursorSyncProvider } from '../lib/chartCursorSync'
import { DATA_ROW } from '../lib/historyDependencies'
import { claimLapHistoryCar } from '../lib/lapHistoryCar'
import Select from '../lib/AnimatedSelect'
import { buildSelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import { themeSeriesColor } from '../lib/themeColors'
import { StatCard } from './LiveStats'
import type { DensityMode, YAxisBehavior } from '../lib/graphSections'
import type { TrendsLayout, TrendsPageLayout } from '../app/appConfig'
import type { ColumnView } from '../lib/columnStore'
import type { AnalyzeLapData, DamageRow, DriverLapHistoryLap, LapMeasure } from '../types'
import { useSecondaryLapState } from '../lib/chartSplit'
import { useSecondaryBackgroundFile } from '../lib/secondaryComparison'
import { fileDateLabels } from '../lib/secondaryFile'
import { useSecondaryTrends } from '../app/hooks/useSecondaryTrends'
import { useSecondaryHistoryNeed, useSecondaryState } from '../lib/secondaryStream'
import { LapDataSideProvider, type LapDataSide } from '../lib/lapDataSide'
import { emptyView } from '../lib/columnStore'
import type { SecondaryFileData } from './AnalyzeScreen'
import FileTableCards, { type FileTableCard } from './FileTableCards'

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

/** File B's half of a chart while two files are open, with each half's caption. */
interface SecondarySection {
  points: TrendChartPoint[]
  lapPoints?: TrendChartPoint[]
  primaryCaption: ReactNode
  caption: ReactNode
}

function ChartSection({ title, controls, secondary, ...chart }: ComponentProps<typeof TrendChart> & { controls?: ReactNode; secondary?: SecondarySection }) {
  const [hidden, setHidden] = useState<Record<string, boolean>>({})
  const series = useMemo(() => chart.series.map(item => ({ ...item, visible: !hidden[item.label] })), [chart.series, hidden])
  return <section className="chart-panel tyre-chart-container min-w-0 min-h-0 flex flex-col" aria-label={title}>
    <div className="tyre-chart-header flex h-[22px] items-center justify-between mb-2 shrink-0">
      <div className="tyre-chart-controls flex min-w-0 flex-1 items-center gap-0">
        <h2 className="chart-panel-title tyre-chart-title shrink-0 text-[10px] leading-none text-[var(--text-secondary)] uppercase tracking-widest">{title}</h2>
        {controls}
        {secondary && <span className="ml-2 shrink-0 whitespace-nowrap text-[10.5px] normal-case tracking-normal text-[var(--text-secondary)]">both files</span>}
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
    {secondary ? <div className="flex-1 min-h-0 grid grid-cols-2 divide-x divide-[var(--border)]">
      <SplitHalf caption={secondary.primaryCaption}><TrendChart {...chart} title={title} series={series} axisLabels="left" /></SplitHalf>
      <SplitHalf caption={secondary.caption}>
        {/* Syncs with A's half and the other graphs by lap number. */}
        <ChartCursorSyncFileProvider>
          <TrendChart {...chart} title={`${title} · second file`} points={secondary.points} lapPoints={secondary.lapPoints} series={series} axisLabels="right" />
        </ChartCursorSyncFileProvider>
      </SplitHalf>
    </div> : <div className="flex-1 min-h-0"><TrendChart {...chart} title={title} series={series} /></div>}
  </section>
}

/** One file's half of a split Trends chart: the chart, then its caption. */
function SplitHalf({ caption, children }: { caption: ReactNode; children: ReactNode }) {
  return <div className="min-w-0 min-h-0 flex flex-col">
    <div className="flex-1 min-h-0">{children}</div>
    <div className="shrink-0 h-6 flex items-center gap-2 px-4 font-mono text-[10.5px] text-[var(--text-secondary)] whitespace-nowrap overflow-hidden">{caption}</div>
  </div>
}

/** A half's caption: the file's coloured date and what it holds. */
function FileCaption({ date, colorVar, detail }: { date: string; colorVar: '--file-a' | '--file-b'; detail: string }) {
  return <>
    <span className="text-[11.5px] font-bold" style={{ color: `var(${colorVar})` }}>{date}</span>
    <span className="truncate">{detail}</span>
  </>
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
  const pushedMeasures = useTelemetryStore(s => s.lapMeasures)
  // Bumped when a backfill rewrites rows already held, such as V6 damage
  // fields filled into earlier timestamps after a seek.
  const historyRevision = useTelemetryStore(s => s.analyzeLapRevision)
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

  // Each completed lap's ERS and fuel, measured by the engine.
  const measures = useMemo(() => new Map<number, LapMeasure>(pushedMeasures?.car_idx === driver
    ? pushedMeasures.laps.map(lap => [lap.lap_num, lap]) : []), [pushedMeasures, driver])

  const { fastestLap, previous, deployedAverage, harvestedAverage, fuelAverage } = stintSummary(stintLaps, measures)
  const wearPerLap = tyreSets?.sets.find(set => set.fitted)?.avg_wear_per_lap

  // Fastest marks the quickest valid lap among those the graph shows.
  const lapPointLaps = lapsFor(lapRange)
  const lapPoints = useMemo(() => buildLapTimePoints(lapPointLaps, boundaries), [lapPointLaps, boundaries])

  // The page renders on every status frame, but the combined points change only
  // when the tyre history does: rebuild them only then, so the chart is not
  // handed a new dataset every frame. The tyre history can grow in place, so it
  // is keyed by its extent rather than its identity.
  const damageKey = damageHistory && damageHistory.length > 0
    ? `${damageHistory.length}:${damageHistory.time(0)}:${damageHistory.time(damageHistory.length - 1)}:${historyRevision}`
    : ''
  const deploymentLaps = lapsFor(deploymentRange)
  const deploymentPoints = useMemo(() => energyPoints('ers_deployed_mj', deploymentLaps, measures, boundaries),
    [deploymentLaps, measures, boundaries])
  const rechargeLaps = lapsFor(rechargeRange)
  const rechargePoints = useMemo(() => energyPoints('ers_harvested_mj', rechargeLaps, measures, boundaries),
    [rechargeLaps, measures, boundaries])
  const heldDamage = useHeldByKey({ rows: damageHistory }, damageKey)
  const combinedLaps = lapsFor(combinedRange)
  const combinedFirstLap = combinedRange === 'all' ? allLaps[0]?.lap_num ?? 1 : stintStart
  const combinedLapPoints = useMemo(() => combined ? buildLapEndPoints(combinedLaps, measures) : NO_POINTS,
    [combined, combinedLaps, measures])
  const combinedTyrePoints = useMemo(() => combined
    ? buildTyreSamples(combinedLaps, combinedFirstLap, currentLap, boundaries, heldDamage.rows, wearMode)
    : NO_POINTS,
  [combined, combinedLaps, combinedFirstLap, currentLap, boundaries, heldDamage, wearMode])
  const combinedAxes = useCombinedAxes(combined ? combinedLapPoints : null, combinedTyrePoints, showRecharge, colors.lapTime, tyreYAxis)
  const barPoints = useMemo(() => bars ? buildBarPoints(combinedLaps, measures, boundaries, heldDamage.rows, wearMode) : NO_POINTS,
    [bars, combinedLaps, measures, boundaries, heldDamage, wearMode])
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

  // ── File B, side by side: the same graphs from B's own completed laps ──
  // B's laps stay in step with A's by number: those before the lap A is on.
  // Side-by-side on: B gets its own half of each chart. Off, with B open: B
  // is drawn dimmed behind A on the same charts, as on the other pages.
  const secondaryLap = useSecondaryLapState()
  const backgroundFile = useSecondaryBackgroundFile()
  const split = secondaryLap !== null
  const secondaryFile = secondaryLap?.file ?? backgroundFile?.file ?? null
  const bOpen = secondaryFile !== null
  const overlay = bOpen && !split
  const [dateA, dateB] = bOpen ? fileDateLabels(secondaryLap?.primaryFilename ?? backgroundFile?.primaryFilename, secondaryFile.filename) : [null, null]
  const streamedTrends = useSecondaryTrends()
  const secondaryTrends = bOpen ? streamedTrends : null
  const bAllLaps = secondaryTrends?.laps ?? NO_LAPS
  const bStintStart = secondaryTrends?.stintStartLap ?? 1
  const bStintLaps = useMemo(() => bAllLaps.filter(lap => lap.lap_num >= bStintStart), [bAllLaps, bStintStart])
  const bLapsFor = (range: LapRange) => range === 'all' ? bAllLaps : bStintLaps
  const bMeasures = secondaryTrends?.measures ?? NO_MEASURES
  const bSummary = stintSummary(bStintLaps, bMeasures)
  const bMissing = useMemo(() => missingLaps(secondaryFile), [secondaryFile])
  const bLapTimeLaps = bLapsFor(lapRange)
  const bLapPoints = useMemo(() => bOpen ? withGaps(buildLapTimePoints(bLapTimeLaps, NO_BOUNDARIES), bMissing, 1) : NO_POINTS,
    [bOpen, bLapTimeLaps, bMissing])
  const bDeploymentLaps = bLapsFor(deploymentRange)
  const bDeploymentPoints = useMemo(() => bOpen ? withGaps(energyPoints('ers_deployed_mj', bDeploymentLaps, bMeasures, NO_BOUNDARIES), bMissing, 1) : NO_POINTS,
    [bOpen, bDeploymentLaps, bMeasures, bMissing])
  const bRechargeLaps = bLapsFor(rechargeRange)
  const bRechargePoints = useMemo(() => bOpen ? withGaps(energyPoints('ers_harvested_mj', bRechargeLaps, bMeasures, NO_BOUNDARIES), bMissing, 1) : NO_POINTS,
    [bOpen, bRechargeLaps, bMeasures, bMissing])
  const bCombinedLaps = bLapsFor(combinedRange)
  const bCombinedLapPoints = useMemo(() => bOpen && combined ? withGaps(buildLapEndPoints(bCombinedLaps, bMeasures), bMissing, 3) : NO_POINTS,
    [bOpen, combined, bCombinedLaps, bMeasures, bMissing])
  // B's tyre wear, built into points by the same functions as A's damage
  // history. B's stream holds its whole race of tyre wear while this page shows it,
  // as A's store holds A's for Trends; the rows run to B's playhead.
  useSecondaryHistoryNeed(bOpen ? B_TYRE_WEAR_NEED : null)
  const bHistory = useSecondaryState().history
  // B's lap starts from its lap catalog, in time order.
  const bBoundaries = useMemo(() => secondaryFile
    ? secondaryFile.blocks
      .filter(block => block.lapNum >= 1)
      .map(block => ({ lapNum: block.lapNum, sessionTime: block.startSessionTime }))
      .sort((a, b) => a.sessionTime - b.sessionTime)
    : NO_BOUNDARIES, [secondaryFile])
  const bRows = bOpen ? bHistory?.damageHistory ?? null : null
  const bCombinedFirstLap = combinedRange === 'all' ? bAllLaps[0]?.lap_num ?? 1 : bStintStart
  const bCombinedTyrePoints = useMemo(() => bOpen && combined
    ? buildTyreSamples(bCombinedLaps, bCombinedFirstLap, currentLap, bBoundaries, bRows, wearMode)
    : NO_POINTS, [bOpen, combined, bCombinedLaps, bCombinedFirstLap, currentLap, bBoundaries, bRows, wearMode])
  const bBarPoints = useMemo(() => bOpen && bars
    ? withGaps(buildBarPoints(bCombinedLaps, bMeasures, bBoundaries, bRows, wearMode), bMissing, 6)
    : NO_POINTS, [bOpen, bars, bCombinedLaps, bMeasures, bBoundaries, bRows, bMissing, wearMode])
  // B's tyre chart and A's sync by lap fraction: a B time lands at the same
  // fraction of A's lap with that number, and back.
  const bTyreToShared = useMemo(() => lapStretchMap(boundaries, bBoundaries), [boundaries, bBoundaries])
  const bTyreFromShared = useMemo(() => lapStretchMap(bBoundaries, boundaries), [bBoundaries, boundaries])
  // The separate layout's tyre chart reads its rows through the side context:
  // on B's side, B's wear history in place of the store's damage history.
  const bTyreSide = useMemo<LapDataSide>(() => ({
    // B's wear runs to B's playhead already (B's stream).
    side: 'B', lapNum: currentLap,
    lapData: {
      lapNum: currentLap ?? 0, startSessionTime: 0, endSessionTime: 0,
      telemetry: emptyView('telemetry'), motion: emptyView('motion'), motionEx: emptyView('motion_ex'),
      statusHistory: emptyView('status'), damageHistory: bRows ?? emptyView<DamageRow>('damage'),
      lapProgress: emptyView('lap'), playerPositions: [], rowTypeMask: DATA_ROW.damage,
    },
  }), [bRows, currentLap])
  // ── Side-by-side off: B behind A on the same charts ──
  // Combined: B's per-lap values ride on the lap points as channels 3-5.
  const overlayCombinedLapPoints = useMemo(() => {
    if (!overlay || !combined) return combinedLapPoints
    const byLap = new Map(bCombinedLapPoints.map(point => [point.x, point.values]))
    return combinedLapPoints.map(point => ({ ...point, values: [...point.values, ...(byLap.get(point.x) ?? [null, null, null])] }))
  }, [overlay, combined, combinedLapPoints, bCombinedLapPoints])
  const overlayCombinedSeries = useMemo(() => {
    if (!overlay) return combinedSeries
    const name = dateB ?? 'second file'
    return [
      ...combinedSeries,
      { label: `Used · ${name}`, color: dimColor(colors.used, isDark), axis: 1, perLap: true, channel: 3 },
      ...showRecharge ? [{ label: `Recharged · ${name}`, color: dimColor(colors.recharged, isDark), axis: 1, perLap: true, channel: 4 }] : [],
      { label: `Lap time · ${name}`, color: dimColor(colors.lapTime, isDark), axis: 0, perLap: true, channel: 5 },
    ]
  }, [overlay, combinedSeries, dateB, colors.used, colors.recharged, colors.lapTime, showRecharge, isDark])
  // Separate tyre chart: B's wear history placed on A's time axis lap by lap,
  // each B lap stretched to A's lap of the same number.
  const tyreComparison = useMemo(() => {
    if (!overlay || !bRows || bRows.length === 0 || bBoundaries.length === 0) return null
    const firstLine = bBoundaries[0].sessionTime
    const rows = bRows.slice(bRows.lowerBound(firstLine, true))
    return {
      label: dateB ?? 'Second file',
      getComparisonX: lapStretchMap(boundaries, bBoundaries),
      lapData: {
        lapNum: currentLap ?? 0, startSessionTime: firstLine, endSessionTime: firstLine,
        telemetry: emptyView('telemetry'), motion: emptyView('motion'), motionEx: emptyView('motion_ex'),
        statusHistory: emptyView('status'), damageHistory: rows, lapProgress: emptyView('lap'), playerPositions: [],
        rowTypeMask: DATA_ROW.damage,
      } satisfies AnalyzeLapData,
    }
  }, [overlay, bRows, bBoundaries, boundaries, dateB, currentLap])
  const bWearPerLap = useMemo(() => averageWearPerLap(bStintLaps, bBoundaries, bRows), [bStintLaps, bBoundaries, bRows])
  // Both halves share the scales, so the same lap or value sits at the same place.
  const mergedCombinedLapPoints = useMemo(() => bOpen ? [...combinedLapPoints, ...bCombinedLapPoints] : combinedLapPoints,
    [bOpen, combinedLapPoints, bCombinedLapPoints])
  const mergedCombinedTyrePoints = useMemo(() => bOpen ? [...combinedTyrePoints, ...bCombinedTyrePoints] : combinedTyrePoints,
    [bOpen, combinedTyrePoints, bCombinedTyrePoints])
  const sharedCombinedAxes = useCombinedAxes(combined && bOpen ? mergedCombinedLapPoints : null, mergedCombinedTyrePoints, showRecharge, colors.lapTime, tyreYAxis)
  const mergedBarPoints = useMemo(() => bOpen ? [...barPoints, ...bBarPoints] : barPoints, [bOpen, barPoints, bBarPoints])
  const sharedBarAxes = useBarAxes(bars && bOpen ? mergedBarPoints : null, colors.lapTime, tyreYAxis)
  const playbackBlocks = useTelemetryStore(s => bOpen ? s.speedRpmBlocks : null)
  const aTotalLaps = useMemo(() => (playbackBlocks ?? []).reduce((max, block) => Math.max(max, Number(block.lapNum) || 0), 0), [playbackBlocks])
  const bTotalLaps = useMemo(() => (secondaryFile?.blocks ?? []).reduce((max, block) => Math.max(max, block.lapNum), 0), [secondaryFile])
  const bDetail = [
    `${bTotalLaps} laps`,
    ...bMissing.size > 0 ? [`no ${[...bMissing].map(lap => `L${lap}`).join(', ')}`] : [],
    ...aTotalLaps > 0 && bTotalLaps > 0 && bTotalLaps < aTotalLaps ? [`ends L${bTotalLaps}`] : [],
  ].join(' · ')
  const section = (points: TrendChartPoint[], lapPoints?: TrendChartPoint[]): SecondarySection | undefined => split ? {
    points, lapPoints,
    primaryCaption: <FileCaption date={dateA ?? 'File A'} colorVar="--file-a" detail={aTotalLaps > 0 ? `${aTotalLaps} laps` : ''} />,
    caption: <FileCaption date={dateB ?? 'File B'} colorVar="--file-b" detail={bDetail} />,
  } : undefined
  const bLap = secondaryFile && currentLap !== null ? secondaryFile.blocks.find(block => block.lapNum === currentLap) : undefined
  const bCompound = bLap?.statusHistory?.[0]?.tyre_compound ?? 0
  const bVisualCompound = bLap?.statusHistory?.[0]?.visual_compound ?? 0

  const tyreCompound = status?.tyre_compound ?? 0
  const cards = visible.statsCards
  const charts = visible.charts
  // The Separate layout's rows hold only the charts the Layout Editor shows.
  const ersCharts = Number(charts.ersUsage) + Number(charts.recharge)
  const chartRows = [charts.lapTimes && '1fr', ersCharts > 0 && '1fr', charts.tyreWear && '1.1fr']
    .filter((row): row is string => row !== false).map(row => `minmax(0,${row})`).join(' ')
  // The cards as one table while two files are open (Cards 5).
  const bHasTrends = secondaryTrends !== null
  const fileCards: FileTableCard[] = !bOpen ? [] : [
    ...cards.stintLaps ? [{ key: 'stintLaps', label: 'Stint Laps',
      a: { value: history ? String(stintLaps.length) : missing, title: history ? `Current stint starts on lap ${stintStart}` : undefined },
      b: { value: bHasTrends ? String(bStintLaps.length) : missing, title: bHasTrends ? `Stint starts on lap ${bStintStart}` : undefined } }] : [],
    ...cards.wearPerLap ? [{ key: 'wearPerLap', label: 'Wear/Lap',
      a: { value: wearPerLap != null ? wearPerLap.toFixed(2) : missing, unit: '%/L' },
      b: { value: bWearPerLap != null ? bWearPerLap.toFixed(2) : missing, unit: '%/L',
        title: bWearPerLap != null ? 'Average of the four tyres\' wear per lap over consecutive laps of the stint' : undefined } }] : [],
    ...cards.recPerLap ? [{ key: 'recPerLap', label: 'Rec/Lap',
      a: { value: harvestedAverage.value, unit: 'MJ/L', color: colors.recharged, title: harvestedAverage.title },
      b: { value: bSummary.harvestedAverage.value, unit: 'MJ/L', color: colors.recharged, title: bSummary.harvestedAverage.title } }] : [],
    ...cards.ersPerLap ? [{ key: 'ersPerLap', label: 'ERS/Lap',
      a: { value: deployedAverage.value, unit: 'MJ/L', color: colors.used, title: deployedAverage.title },
      b: { value: bSummary.deployedAverage.value, unit: 'MJ/L', color: colors.used, title: bSummary.deployedAverage.title } }] : [],
    ...cards.fuelPerLap ? [{ key: 'fuelPerLap', label: 'Fuel/Lap',
      a: { value: fuelAverage.value, unit: 'kg/L', title: fuelAverage.title },
      b: { value: bSummary.fuelAverage.value, unit: 'kg/L', title: bSummary.fuelAverage.title } }] : [],
    ...cards.fastestLap ? [{ key: 'fastestLap', label: 'Fastest',
      a: { value: fastestLap ? fmtMs(fastestLap.lap_time_ms) : missing, color: 'var(--color-fastest)', title: fastestLap ? `Lap ${fastestLap.lap_num}` : undefined },
      b: { value: bSummary.fastestLap ? fmtMs(bSummary.fastestLap.lap_time_ms) : missing, color: 'var(--color-fastest)', title: bSummary.fastestLap ? `Lap ${bSummary.fastestLap.lap_num}` : undefined } }] : [],
    ...cards.previousLap ? [{ key: 'previousLap', label: 'Previous',
      a: { value: previous && previous.lap_time_ms > 0 ? fmtMs(previous.lap_time_ms) : missing, color: previous && !previous.lap_valid ? '#C4162A' : undefined },
      b: { value: bSummary.previous && bSummary.previous.lap_time_ms > 0 ? fmtMs(bSummary.previous.lap_time_ms) : missing,
        color: bSummary.previous && !bSummary.previous.lap_valid ? '#C4162A' : undefined,
        title: bSummary.previous ? `Lap ${bSummary.previous.lap_num}${bSummary.previous.lap_valid ? '' : ' · Invalid'}` : undefined } }] : [],
    ...cards.tyre ? [{ key: 'tyre', label: 'Tyre',
      a: { value: tyreCompound > 0 ? tn('tyre.actual', tyreCompound) : missing, color: tyreCompound > 0 ? tyreCompoundColor(tyreCompound, status?.visual_compound ?? 0) : undefined },
      b: { value: bCompound > 0 ? tn('tyre.actual', bCompound) : missing, color: bCompound > 0 ? tyreCompoundColor(bCompound, bVisualCompound) : undefined } }] : [],
  ]
  return <div className="h-full min-h-0 overflow-hidden bg-[var(--bg-panel)] border-t border-[var(--border)] flex flex-col">
    {bOpen ? <FileTableCards dateA={dateA ?? 'File A'} dateB={dateB ?? 'File B'} cards={fileCards} /> : Object.values(cards).some(Boolean) && <div className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]">
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
      {charts.bars && <ChartSection title={wearMode === 'life' ? 'Tyre Life · ERS · Lap Times' : 'Tyre Wear · ERS · Lap Times'} points={barPoints} series={barSeries} axes={bOpen ? sharedBarAxes : barAxes}
        isDark={isDark} bars discrete formatX={lapX} formatY={lapAxisY}
        xDomain={sharedDomain(bOpen, barPoints, bBarPoints)} secondary={section(bBarPoints)}
        comparisonPoints={overlay ? bBarPoints : undefined} comparisonLabel={dateB ?? undefined}
        controls={<RangeSelect label="Bar graph range" range={combinedRange} onChange={setCombinedRange} />} />}
    </div> : combined ? <div className="flex-1 min-w-0 min-h-0 grid grid-rows-[minmax(0,1fr)]">
      {charts.combined && <ChartSection title={wearMode === 'life' ? 'Lap Times · ERS · Tyre Life' : 'Lap Times · ERS · Tyre Wear'} points={combinedTyrePoints} lapPoints={overlayCombinedLapPoints} series={overlayCombinedSeries} axes={bOpen ? sharedCombinedAxes : combinedAxes}
        isDark={isDark} formatX={lapX} formatY={lapAxisY}
        xDomain={sharedDomain(bOpen, combinedLapPoints, combinedTyrePoints, bCombinedLapPoints)} secondary={section(bCombinedTyrePoints, bCombinedLapPoints)}
        comparisonPoints={overlay ? bCombinedTyrePoints : undefined} comparisonLabel={dateB ?? undefined}
        controls={<RangeSelect label="Combined graph range" range={combinedRange} onChange={setCombinedRange} />} />}
    </div> : <div className="flex-1 min-w-0 min-h-0">
      <ChartCursorSyncProvider enabled={cursorSyncEnabled} secondaryHorizontalCrosshair={secondaryHorizontalCrosshair} secondaryVerticalCrosshair={secondaryVerticalCrosshair}>
        <div className="h-full min-w-0 min-h-0 grid divide-y divide-[var(--border)]" style={{ gridTemplateRows: chartRows }}>
          {charts.lapTimes && <ChartSection title="Lap Times" points={lapPoints} series={lapSeries} isDark={isDark} discrete formatX={lapX} formatY={lapAxisY} formatTooltipY={lapTooltipY} cursorSync={lapTimeSync}
            xDomain={sharedDomain(bOpen, lapPoints, bLapPoints)} fixedRange={bOpen ? paddedRange(lapPoints, bLapPoints) : undefined} secondary={section(bLapPoints)}
            comparisonPoints={overlay ? bLapPoints : undefined} comparisonLabel={dateB ?? undefined}
            controls={<RangeSelect label="Lap times range" range={lapRange} onChange={setLapRange} />} />}
          {ersCharts > 0 && <div className={`grid ${ersCharts === 2 && !split ? 'grid-cols-2 divide-x' : ersCharts === 2 ? 'grid-rows-2 divide-y' : 'grid-cols-1'} divide-[var(--border)] min-h-0`}>
            {charts.ersUsage && <ChartSection title="ERS Usage" points={deploymentPoints} series={deploymentSeries} isDark={isDark} discrete zeroBaselineHeadroom={0.5} formatX={lapX} formatY={energyAxisY} formatTooltipY={energyTooltipY} cursorSync={deploymentSync}
              xDomain={sharedDomain(bOpen, deploymentPoints, bDeploymentPoints)} fixedRange={bOpen ? zeroBasedRange(0.5, deploymentPoints, bDeploymentPoints) : undefined} secondary={section(bDeploymentPoints)}
              comparisonPoints={overlay ? bDeploymentPoints : undefined} comparisonLabel={dateB ?? undefined}
              controls={<RangeSelect label="ERS usage range" range={deploymentRange} onChange={setDeploymentRange} />} />}
            {charts.recharge && <ChartSection title="Recharge" points={rechargePoints} series={rechargeSeries} isDark={isDark} discrete zeroBaselineHeadroom={0.5} formatX={lapX} formatY={energyAxisY} formatTooltipY={energyTooltipY} cursorSync={rechargeSync}
              xDomain={sharedDomain(bOpen, rechargePoints, bRechargePoints)} fixedRange={bOpen ? zeroBasedRange(0.5, rechargePoints, bRechargePoints) : undefined} secondary={section(bRechargePoints)}
              comparisonPoints={overlay ? bRechargePoints : undefined} comparisonLabel={dateB ?? undefined}
              controls={<RangeSelect label="Recharge range" range={rechargeRange} onChange={setRechargeRange} />} />}
          </div>}
          {charts.tyreWear && <div className={split ? 'min-h-0 grid grid-cols-2 divide-x divide-[var(--border)]' : 'contents'}>
            <ChartCoordinatesProvider mode={tyreRange === 'all' ? 'AL' : 'SL'} referenceLapNum={null} rowTypeMask={DATA_ROW.damage} sectorBoundaries={false} stintStartLap={history?.stint_start_lap || undefined}
              comparison={tyreComparison}>
              <StintTyreWearChart isDark={isDark} wearMode={wearMode} yAxis={tyreYAxis}
                controls={<RangeSelect label="Tyre graph range" range={tyreRange} onChange={setTyreRange} />} />
            </ChartCoordinatesProvider>
            {/* B's half: A's own tyre chart, fed B's wear history and lap starts. */}
            {split && <ChartCursorSyncFileProvider toShared={bTyreToShared} fromShared={bTyreFromShared}>
            <LapDataSideProvider value={bTyreSide}>
              <ChartCoordinatesProvider mode={tyreRange === 'all' ? 'AL' : 'SL'} referenceLapNum={null} rowTypeMask={DATA_ROW.damage} sectorBoundaries={false}
                stintStartLap={bStintStart} lapBoundaries={bBoundaries}>
                <StintTyreWearChart isDark={isDark} wearMode={wearMode} yAxis={tyreYAxis} followSessionClock={false}
                  controls={<span className="ml-2 text-[11px] font-mono font-bold normal-case tracking-normal" style={{ color: 'var(--file-b)' }}>{dateB ?? 'File B'}</span>} />
              </ChartCoordinatesProvider>
            </LapDataSideProvider>
            </ChartCursorSyncFileProvider>}
          </div>}
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
type LapMeasures = ReadonlyMap<number, LapMeasure>

function energyPoints(pick: 'ers_deployed_mj' | 'ers_harvested_mj', laps: readonly { lap_num: number }[],
  measures: LapMeasures, boundaries: ReturnType<typeof useLapBoundaries>): TrendChartPoint[] {
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
  measures: LapMeasures): TrendChartPoint[] {
  const fastest = laps.reduce<number | null>((best, lap) =>
    lap.lap_valid && lap.lap_time_ms > 0 && (best === null || lap.lap_time_ms < best) ? lap.lap_time_ms : best, null)
  return laps.map(lap => {
    const measure = measures.get(lap.lap_num)
    return {
      x: lap.lap_num, label: `Lap ${lap.lap_num}`,
      values: [measure?.ers_deployed_mj ?? null, measure?.ers_harvested_mj ?? null, lap.lap_time_ms > 0 ? lap.lap_time_ms / 1000 : null],
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
  measures: LapMeasures, boundaries: ReturnType<typeof useLapBoundaries>,
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
      values: [...wear, measures.get(lap.lap_num)?.ers_deployed_mj ?? null, lap.lap_time_ms > 0 ? lap.lap_time_ms / 1000 : null],
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

const NO_LAPS: DriverLapHistoryLap[] = []
const NO_MEASURES: LapMeasures = new Map()
const NO_BOUNDARIES: ReturnType<typeof useLapBoundaries> = []
// B's tyre wear for the whole race (damage rows), from B's stream.
const B_TYRE_WEAR_NEED = { mask: DATA_ROW.damage, seconds: -1 }

/** The stint cards' values from one file's laps and their measures. */
function stintSummary(stintLaps: readonly DriverLapHistoryLap[], measures: LapMeasures) {
  const fastestLap = stintLaps.reduce<DriverLapHistoryLap | null>((best, lap) =>
    lap.lap_valid && lap.lap_time_ms > 0 && (best === null || lap.lap_time_ms < best.lap_time_ms) ? lap : best, null)
  return {
    fastestLap,
    previous: stintLaps.at(-1) ?? null,
    deployedAverage: average(stintLaps.map(lap => measures.get(lap.lap_num)?.ers_deployed_mj ?? null)),
    harvestedAverage: average(stintLaps.map(lap => measures.get(lap.lap_num)?.ers_harvested_mj ?? null)),
    fuelAverage: average(stintLaps.map(lap => measures.get(lap.lap_num)?.fuel_used_kg ?? null)),
  }
}

/** Lap time per completed lap; Fastest marks the quickest valid lap among them. */
function buildLapTimePoints(laps: readonly DriverLapHistoryLap[], boundaries: ReturnType<typeof useLapBoundaries>): TrendChartPoint[] {
  const shownFastest = laps.reduce<number | null>((best, lap) =>
    lap.lap_valid && lap.lap_time_ms > 0 && (best === null || lap.lap_time_ms < best) ? lap.lap_time_ms : best, null)
  return laps.map(lap => ({
    x: lap.lap_num, values: [lap.lap_time_ms > 0 ? lap.lap_time_ms / 1000 : null], label: `Lap ${lap.lap_num}`,
    invalid: lap.lap_time_ms > 0 && !lap.lap_valid, fastest: lap.lap_valid && lap.lap_time_ms === shownFastest,
    lapStart: lapStartSessionTime(boundaries, lap.lap_num),
  }))
}

/** Lap numbers the second file never ran within its race (a red flag), from its lap catalog. */
function missingLaps(file: SecondaryFileData | null): ReadonlySet<number> {
  if (!file || file.blocks.length === 0) return new Set()
  const held = new Set(file.blocks.map(block => block.lapNum))
  const last = Math.max(...held)
  const missingSet = new Set<number>()
  for (let lap = 1; lap < last; lap++) if (!held.has(lap)) missingSet.add(lap)
  return missingSet
}

/** A break in the line at each missing lap inside the points' span. */
function withGaps(points: TrendChartPoint[], missingSet: ReadonlySet<number>, channels: number): TrendChartPoint[] {
  if (points.length < 2 || missingSet.size === 0) return points
  const first = points[0].x, last = points.at(-1)!.x
  const gaps = [...missingSet].filter(lap => lap > first && lap < last && !points.some(point => point.x === lap))
  if (gaps.length === 0) return points
  return [...points, ...gaps.map(lap => ({ x: lap, values: Array<null>(channels).fill(null), label: `No lap ${lap}` }))]
    .sort((a, b) => a.x - b.x)
}

/** One lap span for both halves: the first and last lap either file shows. */
function sharedDomain(split: boolean, ...lists: readonly TrendChartPoint[][]): [number, number] | undefined {
  if (!split) return undefined
  let first = Infinity, last = -Infinity
  for (const list of lists) {
    if (list.length === 0) continue
    first = Math.min(first, list[0].x)
    last = Math.max(last, list.at(-1)!.x)
  }
  return Number.isFinite(first) ? [first, last] : undefined
}

function valueExtent(lists: readonly TrendChartPoint[][]): { min: number; max: number } {
  let min = Infinity, max = -Infinity
  for (const list of lists) for (const point of list) for (const value of point.values)
    if (value != null && Number.isFinite(value)) { min = Math.min(min, value); max = Math.max(max, value) }
  return { min, max }
}

/** The lap-time scale both halves share, padded as a single chart pads its own. */
function paddedRange(...lists: readonly TrendChartPoint[][]): [number, number] | undefined {
  const { min, max } = valueExtent(lists)
  if (!Number.isFinite(min)) return undefined
  const pad = max > min ? (max - min) * 0.1 : 1
  return [min - pad, max + pad]
}

/** A scale from zero to `headroom` above the higher file's peak. */
function zeroBasedRange(headroom: number, ...lists: readonly TrendChartPoint[][]): [number, number] | undefined {
  const { max } = valueExtent(lists)
  return [0, (Number.isFinite(max) ? Math.max(max, 0) : 0) + headroom]
}

/**
 * Mean wear per lap across the four tyres: the wear at each lap line read from
 * the damage history, over consecutive laps of the stint (the first stint lap
 * has no line before it on this set).
 */
function averageWearPerLap(stintLaps: readonly DriverLapHistoryLap[], boundaries: ReturnType<typeof useLapBoundaries>,
  rows: ColumnView<DamageRow> | null): number | null {
  if (!rows || rows.length === 0) return null
  const starts = new Map<number, number>()
  for (const boundary of boundaries) starts.set(boundary.lapNum, boundary.sessionTime)
  // The newest reading before lap `lap + 1` starts: the set's wear as lap `lap` closed.
  const meanAtEnd = (lap: number) => {
    const end = starts.get(lap + 1)
    const index = end !== undefined ? rows.lowerBound(end, true) - 1 : -1
    if (index < 0) return null
    let sum = 0
    for (const corner of CORNERS) {
      const value = rows.num(`tyre_wear_${corner}`, index)
      if (!Number.isFinite(value)) return null
      sum += value
    }
    return sum / 4
  }
  let total = 0, count = 0
  for (let i = 1; i < stintLaps.length; i++) {
    const previous = stintLaps[i - 1].lap_num, lap = stintLaps[i].lap_num
    if (lap !== previous + 1) continue
    const before = meanAtEnd(previous), after = meanAtEnd(lap)
    if (before === null || after === null || after < before) continue
    total += after - before
    count++
  }
  return count > 0 ? total / count : null
}

/** A series colour mixed toward the panel, as comparison traces are drawn. */
function dimColor(hex: string, isDark: boolean): string {
  const match = /^#([0-9a-f]{6})$/i.exec(hex)
  if (!match) return hex
  const value = Number.parseInt(match[1], 16)
  const bg = isDark ? [0x12, 0x14, 0x1f] : [0xeb, 0xea, 0xe6]
  const rgb = [(value >> 16) & 255, (value >> 8) & 255, value & 255]
  return `#${rgb.map((channel, i) => Math.round(channel * 0.35 + bg[i] * 0.65).toString(16).padStart(2, '0')).join('')}`
}

/**
 * Maps a session time in file B onto file A's clock: the time falls in some
 * B lap, and lands at the same fraction of A's lap with that number. The
 * last lap of either, with no following line, keeps B's own pace.
 */
function lapStretchMap(aBoundaries: ReturnType<typeof useLapBoundaries>, bBoundaries: ReturnType<typeof useLapBoundaries>): (time: number) => number {
  const aStarts = new Map<number, number>()
  for (const boundary of aBoundaries) aStarts.set(boundary.lapNum, boundary.sessionTime)
  const bLines = [...bBoundaries]
  return (time: number) => {
    let lo = 0, hi = bLines.length
    while (lo < hi) { const mid = (lo + hi) >> 1; if (bLines[mid].sessionTime <= time) lo = mid + 1; else hi = mid }
    const index = lo - 1
    if (index < 0) return NaN
    const bLap = bLines[index]
    const aStart = aStarts.get(bLap.lapNum)
    if (aStart === undefined) return NaN
    const bEnd = bLines[index + 1]?.sessionTime
    const aEnd = aStarts.get(bLap.lapNum + 1)
    if (bEnd === undefined || aEnd === undefined || bEnd <= bLap.sessionTime) return aStart + (time - bLap.sessionTime)
    return aStart + (time - bLap.sessionTime) * (aEnd - aStart) / (bEnd - bLap.sessionTime)
  }
}
