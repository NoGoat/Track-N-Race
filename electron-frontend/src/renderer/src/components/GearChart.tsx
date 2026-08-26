import { useCallback, useMemo, useState } from 'react'
import type { AlignedTable, TelemetryRow } from '../types'
import { useTelemetryStore } from '../stores/telemetryStore'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef } from './charts/TimeChartView'
import { useChartCoordinates } from '../lib/chartCoordinates'
import { formatChartComparisonTooltip } from '../lib/chartComparisonTooltip'
import { ChartWindowOverrideSelect, ChartWindowScope, useChartWindowSeconds } from '../lib/chartWindowOverrides'
import { themeSeriesColor } from '../lib/themeColors'

interface Props { isDark: boolean; view?: 'chart' | 'table'; windowSeconds?: number }
const COLOR_GEAR = '#5794F2'
const SERIES: SeriesDef<TelemetryRow>[] = [{
  label: 'Gear',
  color: COLOR_GEAR,
  getY: d => d.gear,
  lineWidth: 2,
  lineType: 1,
  stepLocation: 1,
  fill: 'rgba(87,148,242,0.2)',
  fillBaseline: 0.5,
}]
const TABLE_COLS: GraphTableColumn[] = [{ header: 'Gear', color: COLOR_GEAR, format: v => String(Math.round(v)) }]
const GEAR_TICKS = [1, 2, 3, 4, 5, 6, 7, 8]
const EMPTY_ALIGNED: AlignedTable = [new Float64Array(0), new Float64Array(0)]
function fmtTime(s: number) { return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}` }

function GearChartContent({ isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const coordinates = useChartCoordinates()
  const scopedWindowSeconds = useChartWindowSeconds(windowSeconds)
  const colorGear = themeSeriesColor(COLOR_GEAR, isDark)
  const [hiddenSeries, setHiddenSeries] = useState<Record<string, boolean>>({})
  const toggleSeries = useCallback((label: string) => {
    setHiddenSeries(prev => ({ ...prev, [label]: !prev[label] }))
  }, [])
  const chartSeries = useMemo(() => SERIES.map(series => ({
    ...series,
    color: colorGear,
    visible: !hiddenSeries[series.label],
  })), [colorGear, hiddenSeries])
  const tableColumns = useMemo(() => TABLE_COLS.map(column => ({ ...column, color: colorGear })), [colorGear])
  const data = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapTelemetry : s.telemetry)
  const getTableValues = useCallback((row: TelemetryRow) => [row.gear], [])
  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table' || coordinates.allLapsMode) return EMPTY_ALIGNED
    const ts = new Float64Array(data.length), gear = new Float64Array(data.length)
    data.forEach((d, i) => { ts[i] = d.session_time; gear[i] = d.gear })
    return [ts, gear]
  }, [coordinates.allLapsMode, data, view])
  const axisColor = isDark ? '#7c8098' : '#596168'
  const tooltipFormat = useCallback((x: number, v: number[], comparison?: number[]) => {
    const formatValues = (values: number[]) => hiddenSeries['Gear'] ? '' : `<div style="color:${colorGear}">Gear ${Math.round(values[0])}</div>`
    return [
      `<div style="color:${axisColor};margin-bottom:4px">${coordinates.distanceMode ? coordinates.formatX(x) : fmtTime(x)}</div>`,
      formatValues(v),
      formatChartComparisonTooltip(comparison, coordinates.mode, formatValues),
    ].join('')
  }, [axisColor, colorGear, coordinates, hiddenSeries])

  return <div className="bg-[var(--bg-panel)] px-4 pb-4 pt-3 h-full flex flex-col">
    <div className="flex h-[22px] items-center justify-between mb-3 shrink-0">
      <div className="flex items-center gap-0">
        <h2 className="pr-[4px] text-[10px] leading-none text-[var(--text-secondary)] uppercase tracking-widest">Gear</h2>
        <ChartWindowOverrideSelect />
      </div>
      {view !== 'table' && <div className="flex items-center gap-4 text-xs">
        <span
          onClick={() => toggleSeries('Gear')}
          className="cursor-pointer select-none"
          style={{ color: colorGear, filter: hiddenSeries['Gear'] ? 'grayscale(100%)' : undefined }}
        >
          — Gear
        </span>
      </div>}
    </div>
    <div className="flex-1 min-h-0 relative">
      {data.length === 0 ? <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        : view === 'table' ? <GraphTable columns={tableColumns} data={tableData} liveRows={data} getLiveValues={getTableValues} />
          : <TimeChartView<TelemetryRow> key={coordinates.mode ?? (coordinates.allLapsMode ? 'AL' : 'time')} isDark={isDark} rows={data} comparisonRows={coordinates.comparisonMode ? coordinates.lapData?.telemetry : undefined} getX={d => d.session_time} series={chartSeries}
              windowSeconds={scopedWindowSeconds} yRange={{ kind: 'fixed', min: 0.5, max: 8.5 }} yAxisSize={28}
              yTickValues={() => GEAR_TICKS} yTickFormat={v => String(v)} xTickFormat={fmtTime}
              refLines={[2, 4, 6].map(y => ({ y, dashed: true }))} tooltipFormat={tooltipFormat} />}
    </div>
  </div>
}

export default function GearChart(props: Props) {
  return <ChartWindowScope section="inputGear"><GearChartContent {...props} /></ChartWindowScope>
}
