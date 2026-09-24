import { useCallback, useMemo, useState } from 'react'
import type { AlignedTable, StatusRow } from '../types'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef, type YRangeSpec } from './charts/TimeChartView'
import { alignedFromView, sessionTimeAt, type ColumnView } from '../lib/columnStore'
import type { YAxisBehavior } from '../lib/graphSections'
import { useChartCoordinates } from '../lib/chartCoordinates'
import { formatChartComparisonTooltip } from '../lib/chartComparisonTooltip'
import { useTelemetryStore } from '../stores/telemetryStore'
import { ChartWindowOverrideSelect, ChartWindowScope, useChartWindowSeconds } from '../lib/chartWindowOverrides'
import type { GraphSection } from '../lib/graphSections'
import { themeSeriesColor } from '../lib/themeColors'
import { HISTORY_ROW } from '../lib/historyDependencies'
import { POWER_CHART_Y_AXIS_SIZE } from '../lib/powerChartLayout'
import type { PowerPageLayout } from '../app/appConfig'

interface CP { data: ColumnView<StatusRow>; isDark: boolean; view?: 'chart' | 'table'; windowSeconds?: number; fuelUpperLimit?: number | null; hasMguh?: boolean; harvestUpperLimit?: number; ersHarvestYAxis?: YAxisBehavior }

const C_ICE    = '#5794F2'
const C_MGUK   = '#FADE2A'
const C_HARV_K = '#37872D'
const C_HARV_H = '#C4162A'
const C_FUEL   = '#F0A500'

const POWER_CURSOR_ORDER: Partial<Record<GraphSection, number>> = {
  powerSplit: 10,
  powerHarvest: 20,
  powerStore: 30,
  powerFuel: 40,
}

const COLS_SPLIT: GraphTableColumn[] = [
  { header: 'ICE', color: C_ICE, format: v => `${v.toFixed(1)}kW` },
  { header: 'MGU-K', color: C_MGUK, format: v => `${v.toFixed(1)}kW` },
]
const COLS_HARVEST: GraphTableColumn[] = [
  { header: 'MGU-K', color: C_HARV_K, format: v => `${v.toFixed(1)}kJ` },
  { header: 'MGU-H', color: C_HARV_H, format: v => `${v.toFixed(1)}kJ` },
]
const COLS_STORE: GraphTableColumn[] = [{ header: 'ERS', color: C_ICE, format: v => `${v.toFixed(1)}%` }]
const COLS_FUEL: GraphTableColumn[] = [{ header: 'Fuel', color: C_FUEL, format: v => `${v.toFixed(2)}kg` }]

// Absent power fields read as zero, as the row version's `?? 0` did.
function orZero(value: number): number { return value === value ? value : 0 }

const SERIES_SPLIT: SeriesDef<StatusRow>[] = [
  { label: 'ICE', color: C_ICE, getY: (rows, i) => orZero(rows.num('engine_power_ice_kw', i)) },
  { label: 'MGU-K', color: C_MGUK, getY: (rows, i) => orZero(rows.num('engine_power_mguk_kw', i)) },
]
const SERIES_HARVEST: SeriesDef<StatusRow>[] = [
  { label: 'MGU-K', color: C_HARV_K, getY: (rows, i) => orZero(rows.num('ers_harvested_mguk_j', i)) / 1000 },
  { label: 'MGU-H', color: C_HARV_H, getY: (rows, i) => orZero(rows.num('ers_harvested_mguh_j', i)) / 1000 },
]
const SERIES_STORE: SeriesDef<StatusRow>[] = [{ label: 'ERS', color: C_ICE, getY: (rows, i) => rows.num('ers_pct', i) }]
const SERIES_FUEL: SeriesDef<StatusRow>[] = [{ label: 'Fuel', color: C_FUEL, getY: (rows, i) => rows.num('fuel_kg', i) }]
const EMPTY_ALIGNED: AlignedTable = [new Float64Array(0)]

function fmtTime(s: number) {
  return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`
}

interface PowerLineProps extends CP {
  section: GraphSection
  title: string
  series: SeriesDef<StatusRow>[]
  columns: GraphTableColumn[]
  yRange: YRangeSpec
  yFormat: (v: number) => string
  note?: string
  tooltipDetails?: (values: number[], axisColor: string) => string
}

function PowerLineChartContent(props: PowerLineProps) {
  const {
    section, title, isDark, view = 'chart', windowSeconds = 30, series, columns,
    yRange, yFormat, note, tooltipDetails,
  } = props
  const coordinates = useChartCoordinates()
  const scopedWindowSeconds = useChartWindowSeconds(windowSeconds)
  const [hiddenSeries, setHiddenSeries] = useState<Record<string, boolean>>({})
  const toggleSeries = useCallback((label: string) => {
    setHiddenSeries(prev => ({ ...prev, [label]: !prev[label] }))
  }, [])
  const themedSeries = useMemo(() => series.map(item => ({
    ...item,
    color: themeSeriesColor(item.color, isDark),
    visible: item.visible !== false && !hiddenSeries[item.label],
  })), [hiddenSeries, isDark, series])
  const themedColumns = useMemo(() => columns.map(item => item.color
    ? { ...item, color: themeSeriesColor(item.color, isDark) }
    : item), [columns, isDark])
  const data = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapStatusHistory : s.statusHistory)
  const visibleEntries = useMemo(() => themedSeries.flatMap((series, sourceIndex) => {
    const column = themedColumns[sourceIndex]
    return series.visible !== false && column ? [{ series, column, sourceIndex }] : []
  }), [themedColumns, themedSeries])
  const visibleColumns = useMemo(() => visibleEntries.map(e => e.column), [visibleEntries])
  const getTableValues = useCallback((rows: ColumnView<StatusRow>, i: number) => visibleEntries.map(entry => entry.series.getY(rows, i)), [visibleEntries])
  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table' || coordinates.allLapsMode) return EMPTY_ALIGNED
    return alignedFromView(data, visibleEntries.map(entry => entry.series.getY))
  }, [coordinates.allLapsMode, data, view, visibleEntries])

  const axisColor = isDark ? '#7c8098' : '#596168'
  const formatValues = useCallback((source: number[]) => [
      ...visibleEntries.map(e =>
        `<div><span style="color:${e.series.color}">${e.series.label}</span>: ${e.column.format(source[e.sourceIndex])}</div>`,
      ),
      tooltipDetails?.(source, axisColor) ?? '',
    ].join(''), [axisColor, tooltipDetails, visibleEntries])
  const tooltipFormat = useCallback((x: number, values: number[], comparison?: number[]) => {
    return [
      `<div style="color:${axisColor};margin-bottom:4px">${coordinates.distanceMode ? coordinates.formatX(x) : fmtTime(x)}</div>`,
      formatValues(values),
      formatChartComparisonTooltip(comparison, coordinates.mode, formatValues),
    ].join('')
  }, [axisColor, coordinates, formatValues])
  const cursorSync = useMemo(() => ({
    id: section,
    order: POWER_CURSOR_ORDER[section] ?? 100,
    formatRow: (rows: ColumnView<StatusRow>, i: number) => [
      `<div style="color:${axisColor};margin-top:3px">${title}</div>`,
      formatValues(themedSeries.map(item => item.getY(rows, i))),
    ].join(''),
  }), [axisColor, formatValues, section, themedSeries, title])

  return (
    <div className="chart-panel bg-[var(--bg-panel)] h-full flex flex-col">
      <div className="flex h-[22px] items-center justify-between mb-3 shrink-0">
        <div className="flex items-center gap-0">
          <h2 className="chart-panel-title text-[10px] leading-none text-[var(--text-secondary)] uppercase tracking-widest">{title}</h2>
          <ChartWindowOverrideSelect />
        </div>
        {view !== 'table' && <div className="flex items-center gap-4 text-xs">
            {series.filter(s => s.visible !== false).map((s) => (
              <span
                key={s.label}
                onClick={() => toggleSeries(s.label)}
                className="cursor-pointer select-none"
                style={{
                  color: themeSeriesColor(s.color, isDark),
                  filter: hiddenSeries[s.label] ? 'grayscale(100%)' : undefined,
                }}
              >
                — {s.label}
              </span>
            ))}
            {note && <span className="text-[var(--text-secondary)]">{note}</span>}
        </div>}
      </div>
      <div className="flex-1 min-h-0 relative">
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : view === 'table' ? (
          <GraphTable columns={visibleColumns} data={tableData} liveRows={data} getLiveValues={getTableValues} allLapsDataMask={HISTORY_ROW.status} />
        ) : (
          <TimeChartView<StatusRow>
            key={coordinates.mode ?? (coordinates.allLapsMode ? 'AL' : 'time')}
            isDark={isDark}
            rows={data}
            allLapsDataMask={HISTORY_ROW.status}
            comparisonRows={coordinates.comparisonMode ? coordinates.lapData?.statusHistory : undefined}
            getX={sessionTimeAt}
            series={themedSeries}
            windowSeconds={scopedWindowSeconds}
            yRange={yRange}
            yAxisSize={POWER_CHART_Y_AXIS_SIZE}
            yTickValues={yRange.kind !== 'auto' ? (min, max) => {
              const step = (max - min) / 4
              return Array.from({ length: 5 }, (_, i) => min + step * i)
            } : undefined}
            yTickFormat={yFormat}
            xTickFormat={fmtTime}
            tooltipFormat={tooltipFormat}
            cursorSync={cursorSync}
            fastScroll
            followSessionClock
            minScrollStallS={1}
          />
        )}
      </div>
    </div>
  )
}

function PowerLineChart(props: PowerLineProps) {
  return <ChartWindowScope section={props.section}><PowerLineChartContent {...props} /></ChartWindowScope>
}

function PowerSplitChart(props: CP) {
  const details = useCallback((v: number[], ac: string) =>
    `<div style="color:${ac}">Total: ${(v[0] + v[1]).toFixed(1)} kW</div>`, [])
  return <PowerLineChart {...props} section="powerSplit" title="Power" series={SERIES_SPLIT} columns={COLS_SPLIT}
    yRange={{ kind: 'expand', initialLower: 0, initialUpper: 500, lowerPad: 0, upperPad: 0, expandLower: false }} yFormat={v => `${Math.round(v)}kW`}
    tooltipDetails={details} />
}

function ERSHarvestChart(props: CP) {
  const hasMguh = props.hasMguh ?? false
  const series = useMemo(() => SERIES_HARVEST.map((s, i) =>
    i === 1 ? { ...s, visible: hasMguh } : s), [hasMguh])
  const details = useCallback((v: number[], ac: string) =>
    `<div style="color:${ac}">Total: ${(v[0] + (hasMguh ? v[1] : 0)).toFixed(1)} kJ</div>`, [hasMguh])
  return <PowerLineChart {...props} section="powerHarvest" title="ERS Harvest" series={series} columns={COLS_HARVEST}
    yRange={props.ersHarvestYAxis === 'dynamic'
      ? { kind: 'auto' }
      : { kind: 'expand', initialLower: 0, initialUpper: props.harvestUpperLimit ?? 8000, lowerPad: 0, upperPad: 0, expandLower: false }}
    yFormat={v => `${v}kJ`} note="resets each lap"
    tooltipDetails={details} />
}

function ERSStoreChart(props: CP) {
  const details = useCallback((v: number[], ac: string) =>
    `<div style="color:${ac}">${(v[0] / 100 * 4).toFixed(2)} / 4.00 MJ</div>`, [])
  return <PowerLineChart {...props} section="powerStore" title="ERS Store" series={SERIES_STORE} columns={COLS_STORE}
    yRange={{ kind: 'fixed', min: 0, max: 100 }} yFormat={v => `${v}%`} note="max 4.0 MJ"
    tooltipDetails={details} />
}

function FuelHistoryChart(props: CP) {
  const firstFuel = props.data.length ? props.data.num('fuel_kg', 0) : NaN
  const upperLimit = props.fuelUpperLimit ?? Math.max(1, (firstFuel === firstFuel ? firstFuel : 0) + 1)
  return <PowerLineChart {...props} section="powerFuel" title="Fuel History" series={SERIES_FUEL} columns={COLS_FUEL}
    yRange={{ kind: 'fixed', min: 0, max: upperLimit }} yFormat={v => `${v.toFixed(1)}kg`} />
}

interface VisibleCharts { powerSplit: boolean; ersHarvest: boolean; ersStore: boolean; fuelHistory: boolean }
export interface PowerViews {
  powerSplit?: 'chart' | 'table'; ersHarvest?: 'chart' | 'table'
  ersStore?: 'chart' | 'table'; fuelHistory?: 'chart' | 'table'
}

export default function PowerBreakdownChart({ data, isDark, visibleCharts, views, windowSeconds = 30, fuelUpperLimit, hasMguh = false, harvestUpperLimit = 8000, ersHarvestYAxis = 'fixed', layout = 'grid' }: { data: ColumnView<StatusRow>; isDark: boolean; visibleCharts: VisibleCharts; views?: PowerViews; windowSeconds?: number; fuelUpperLimit?: number | null; hasMguh?: boolean; harvestUpperLimit?: number; ersHarvestYAxis?: YAxisBehavior; layout?: PowerPageLayout }) {
  const items = [
    { key: 'powerSplit', el: <PowerSplitChart data={data} isDark={isDark} view={views?.powerSplit} windowSeconds={windowSeconds} /> },
    { key: 'ersHarvest', el: <ERSHarvestChart data={data} isDark={isDark} view={views?.ersHarvest} windowSeconds={windowSeconds} hasMguh={hasMguh} harvestUpperLimit={harvestUpperLimit} ersHarvestYAxis={ersHarvestYAxis} /> },
    { key: 'ersStore', el: <ERSStoreChart data={data} isDark={isDark} view={views?.ersStore} windowSeconds={windowSeconds} /> },
    { key: 'fuelHistory', el: <FuelHistoryChart data={data} isDark={isDark} view={views?.fuelHistory} windowSeconds={windowSeconds} fuelUpperLimit={fuelUpperLimit} /> },
  ].filter(({ key }) => visibleCharts[key as keyof VisibleCharts])
  const odd = items.length % 2 !== 0
  const vertical = layout === 'vertical'

  return (
    <div
      className={vertical
        ? 'h-full flex flex-col divide-y divide-[var(--border)] bg-[var(--border)] overflow-hidden'
        : 'h-full grid grid-cols-2 gap-[1px] bg-[var(--border)] overflow-hidden'}
      style={vertical ? undefined : { gridAutoRows: '1fr' }}
    >
      {items.map(({ key, el }, i) => (
        <div key={key} className={`${vertical ? 'flex-1 min-h-0' : 'h-full'} flex flex-col overflow-hidden${!vertical && odd && i === items.length - 1 ? ' col-span-2' : ''}`}>
          {el}
        </div>
      ))}
    </div>
  )
}
