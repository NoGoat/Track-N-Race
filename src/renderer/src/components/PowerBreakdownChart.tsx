import { useCallback, useMemo } from 'react'
import type uPlot from 'uplot'
import type { StatusRow } from '../types'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef, type YRangeSpec } from './charts/TimeChartView'

interface CP { data: StatusRow[]; isDark: boolean; view?: 'chart' | 'table'; windowSeconds?: number; fuelUpperLimit?: number | null; hasMguh?: boolean }

const C_ICE    = '#5794F2'
const C_MGUK   = '#FADE2A'
const C_HARV_K = '#37872D'
const C_HARV_H = '#C4162A'
const C_FUEL   = '#F0A500'

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

const SERIES_SPLIT: SeriesDef<StatusRow>[] = [
  { label: 'ICE', color: C_ICE, getY: d => d.engine_power_ice_kw ?? 0 },
  { label: 'MGU-K', color: C_MGUK, getY: d => d.engine_power_mguk_kw ?? 0 },
]
const SERIES_HARVEST: SeriesDef<StatusRow>[] = [
  { label: 'MGU-K', color: C_HARV_K, getY: d => (d.ers_harvested_mguk_j ?? 0) / 1000 },
  { label: 'MGU-H', color: C_HARV_H, getY: d => (d.ers_harvested_mguh_j ?? 0) / 1000 },
]
const SERIES_STORE: SeriesDef<StatusRow>[] = [{ label: 'ERS', color: C_ICE, getY: d => d.ers_pct }]
const SERIES_FUEL: SeriesDef<StatusRow>[] = [{ label: 'Fuel', color: C_FUEL, getY: d => d.fuel_kg }]
const EMPTY_ALIGNED: uPlot.AlignedData = [new Float64Array(0)]

function fmtTime(s: number) {
  return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`
}

interface PowerLineProps extends CP {
  title: string
  series: SeriesDef<StatusRow>[]
  columns: GraphTableColumn[]
  yRange: YRangeSpec
  yFormat: (v: number) => string
  note?: string
  profilerLabel: string
  tooltipDetails?: (values: number[], axisColor: string) => string
}

function PowerLineChart({
  title, data, isDark, view = 'chart', windowSeconds = 30, series, columns,
  yRange, yFormat, note, profilerLabel, tooltipDetails,
}: PowerLineProps) {
  const visibleEntries = useMemo(() => series
    .map((s, i) => ({ series: s, column: columns[i], sourceIndex: i }))
    .filter(({ series: s }) => s.visible !== false), [columns, series])
  const visibleColumns = useMemo(() => visibleEntries.map(e => e.column), [visibleEntries])
  const uData = useMemo((): uPlot.AlignedData => {
    if (view !== 'table') return EMPTY_ALIGNED
    const ts = new Float64Array(data.length)
    const values = visibleEntries.map(() => new Float64Array(data.length))
    data.forEach((row, i) => {
      ts[i] = row.session_time
      visibleEntries.forEach((e, k) => { values[k][i] = e.series.getY(row) })
    })
    return [ts, ...values]
  }, [data, view, visibleEntries])

  const axisColor = isDark ? '#7c8098' : '#6b7280'
  const tooltipFormat = useCallback((x: number, values: number[]) => {
    const rows = visibleEntries.map(e =>
      `<div><span style="color:${e.series.color}">${e.series.label}</span>: ${e.column.format(values[e.sourceIndex])}</div>`,
    )
    return [
      `<div style="color:${axisColor};margin-bottom:4px">${fmtTime(x)}</div>`,
      ...rows,
      tooltipDetails?.(values, axisColor) ?? '',
    ].join('')
  }, [axisColor, tooltipDetails, visibleEntries])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">{title}</h2>
        {view !== 'table' && (
          <div className="flex gap-4 text-xs">
            {visibleEntries.map(({ series: s }) => <span key={s.label} style={{ color: s.color }}>— {s.label}</span>)}
            {note && <span className="text-[var(--text-secondary)]">{note}</span>}
          </div>
        )}
      </div>
      <div className="flex-1 min-h-0 relative">
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : view === 'table' ? (
          <GraphTable columns={visibleColumns} data={uData} />
        ) : (
          <TimeChartView<StatusRow>
            isDark={isDark}
            rows={data}
            getX={d => d.session_time}
            series={series}
            windowSeconds={windowSeconds}
            yRange={yRange}
            yAxisSize={52}
            yTickValues={yRange.kind === 'fixed' ? (min, max) => {
              const step = (max - min) / 4
              return Array.from({ length: 5 }, (_, i) => min + step * i)
            } : undefined}
            yTickFormat={yFormat}
            xTickFormat={fmtTime}
            tooltipFormat={tooltipFormat}
            profilerLabel={profilerLabel}
            fastScroll
            followSessionClock
            minScrollStallS={1}
          />
        )}
      </div>
    </div>
  )
}

function PowerSplitChart(props: CP) {
  const details = useCallback((v: number[], ac: string) =>
    `<div style="color:${ac}">Total: ${(v[0] + v[1]).toFixed(1)} kW</div>`, [])
  return <PowerLineChart {...props} title="Power Split" series={SERIES_SPLIT} columns={COLS_SPLIT}
    yRange={{ kind: 'fixed', min: 0, max: 1000 }} yFormat={v => `${v}kW`}
    tooltipDetails={details} profilerLabel="PowerSplit" />
}

function ERSHarvestChart(props: CP) {
  const hasMguh = props.hasMguh ?? false
  const series = useMemo(() => SERIES_HARVEST.map((s, i) =>
    i === 1 ? { ...s, visible: hasMguh } : s), [hasMguh])
  const details = useCallback((v: number[], ac: string) =>
    `<div style="color:${ac}">Total: ${(v[0] + (hasMguh ? v[1] : 0)).toFixed(1)} kJ</div>`, [hasMguh])
  return <PowerLineChart {...props} title="ERS Harvest" series={series} columns={COLS_HARVEST}
    yRange={{ kind: 'auto' }} yFormat={v => `${v}kJ`} note="resets each lap"
    tooltipDetails={details} profilerLabel="ERSHarvest" />
}

function ERSStoreChart(props: CP) {
  const details = useCallback((v: number[], ac: string) =>
    `<div style="color:${ac}">${(v[0] / 100 * 4).toFixed(2)} / 4.00 MJ</div>`, [])
  return <PowerLineChart {...props} title="ERS Store" series={SERIES_STORE} columns={COLS_STORE}
    yRange={{ kind: 'fixed', min: 0, max: 100 }} yFormat={v => `${v}%`} note="max 4.0 MJ"
    tooltipDetails={details} profilerLabel="ERSStore" />
}

function FuelHistoryChart(props: CP) {
  const upperLimit = props.fuelUpperLimit ?? Math.max(1, props.data[0]?.fuel_kg ?? 1)
  return <PowerLineChart {...props} title="Fuel History" series={SERIES_FUEL} columns={COLS_FUEL}
    yRange={{ kind: 'fixed', min: 0, max: upperLimit }} yFormat={v => `${v.toFixed(1)}kg`} profilerLabel="FuelHistory" />
}

interface VisibleCharts { powerSplit: boolean; ersHarvest: boolean; ersStore: boolean; fuelHistory: boolean }
export interface PowerViews {
  powerSplit?: 'chart' | 'table'; ersHarvest?: 'chart' | 'table'
  ersStore?: 'chart' | 'table'; fuelHistory?: 'chart' | 'table'
}

export default function PowerBreakdownChart({ data, isDark, visibleCharts, views, windowSeconds = 30, fuelUpperLimit, hasMguh = false }: { data: StatusRow[]; isDark: boolean; visibleCharts: VisibleCharts; views?: PowerViews; windowSeconds?: number; fuelUpperLimit?: number | null; hasMguh?: boolean }) {
  const items = [
    { key: 'powerSplit', el: <PowerSplitChart data={data} isDark={isDark} view={views?.powerSplit} windowSeconds={windowSeconds} /> },
    { key: 'ersHarvest', el: <ERSHarvestChart data={data} isDark={isDark} view={views?.ersHarvest} windowSeconds={windowSeconds} hasMguh={hasMguh} /> },
    { key: 'ersStore', el: <ERSStoreChart data={data} isDark={isDark} view={views?.ersStore} windowSeconds={windowSeconds} /> },
    { key: 'fuelHistory', el: <FuelHistoryChart data={data} isDark={isDark} view={views?.fuelHistory} windowSeconds={windowSeconds} fuelUpperLimit={fuelUpperLimit} /> },
  ].filter(({ key }) => visibleCharts[key as keyof VisibleCharts])
  const odd = items.length % 2 !== 0

  return (
    <div className="h-full grid grid-cols-2 gap-[1px] bg-[var(--border)] overflow-hidden" style={{ gridAutoRows: '1fr' }}>
      {items.map(({ key, el }, i) => (
        <div key={key} className={`h-full flex flex-col overflow-hidden${odd && i === items.length - 1 ? ' col-span-2' : ''}`}>
          {el}
        </div>
      ))}
    </div>
  )
}
