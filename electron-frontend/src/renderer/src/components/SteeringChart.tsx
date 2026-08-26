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
const COLOR_STEER = '#BF5FFF'
const Y_TICKS = [-1, -0.5, 0, 0.5, 1]
const SERIES: SeriesDef<TelemetryRow>[] = [{ label: 'Steering', color: COLOR_STEER, getY: d => d.steering }]
const TABLE_COLS: GraphTableColumn[] = [{ header: 'Steering', color: COLOR_STEER, format: v => `${Math.round(v * 100)}%` }]
const EMPTY_ALIGNED: AlignedTable = [new Float64Array(0), new Float64Array(0)]

function fmtTime(s: number) { return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}` }
function fmtSteer(v: number) {
  if (Math.abs(v) < 0.005) return '0%'
  return `${Math.abs(v * 100).toFixed(0)}% ${v < 0 ? 'L' : 'R'}`
}

function SteeringChartContent({ isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const coordinates = useChartCoordinates()
  const scopedWindowSeconds = useChartWindowSeconds(windowSeconds)
  const colorSteer = themeSeriesColor(COLOR_STEER, isDark)
  const [hiddenSeries, setHiddenSeries] = useState<Record<string, boolean>>({})
  const toggleSeries = useCallback((label: string) => {
    setHiddenSeries(prev => ({ ...prev, [label]: !prev[label] }))
  }, [])
  const chartSeries = useMemo(() => SERIES.map(series => ({
    ...series,
    color: colorSteer,
    visible: !hiddenSeries[series.label],
  })), [colorSteer, hiddenSeries])
  const tableColumns = useMemo(() => TABLE_COLS.map(column => ({ ...column, color: colorSteer })), [colorSteer])
  const data = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapTelemetry : s.telemetry)
  const getTableValues = useCallback((row: TelemetryRow) => [row.steering], [])
  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table' || coordinates.allLapsMode) return EMPTY_ALIGNED
    const ts = new Float64Array(data.length), steer = new Float64Array(data.length)
    data.forEach((d, i) => { ts[i] = d.session_time; steer[i] = d.steering })
    return [ts, steer]
  }, [coordinates.allLapsMode, data, view])
  const axisColor = isDark ? '#7c8098' : '#596168'
  const tooltipFormat = useCallback((x: number, v: number[], comparison?: number[]) => {
    const formatValues = (values: number[]) => hiddenSeries['Steering'] ? '' : `<div><span style="color:${colorSteer}">Steering</span>: ${fmtSteer(values[0])}</div>`
    return [
      `<div style="color:${axisColor};margin-bottom:4px">${coordinates.distanceMode ? coordinates.formatX(x) : fmtTime(x)}</div>`,
      formatValues(v),
      formatChartComparisonTooltip(comparison, coordinates.mode, formatValues),
    ].join('')
  }, [axisColor, colorSteer, coordinates, hiddenSeries])

  return <div className="bg-[var(--bg-panel)] px-4 pb-4 pt-3 h-full flex flex-col">
    <div className="flex h-[22px] items-center justify-between mb-3 shrink-0">
      <div className="flex items-center gap-0">
        <h2 className="pr-[4px] text-[10px] leading-none text-[var(--text-secondary)] uppercase tracking-widest">Steering</h2>
        <ChartWindowOverrideSelect />
      </div>
      {view !== 'table' && <div className="flex items-center gap-4 text-xs">
        <span
          onClick={() => toggleSeries('Steering')}
          className="cursor-pointer select-none"
          style={{ color: colorSteer, filter: hiddenSeries['Steering'] ? 'grayscale(100%)' : undefined }}
        >
          — Input
        </span>
        <span className="text-[var(--text-secondary)]">L = left / R = right</span>
      </div>}
    </div>
    <div className="flex-1 min-h-0 relative">
      {data.length === 0 ? <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        : view === 'table' ? <GraphTable columns={tableColumns} data={tableData} liveRows={data} getLiveValues={getTableValues} />
          : <TimeChartView<TelemetryRow> key={coordinates.mode ?? (coordinates.allLapsMode ? 'AL' : 'time')} isDark={isDark} rows={data} comparisonRows={coordinates.comparisonMode ? coordinates.lapData?.telemetry : undefined} getX={d => d.session_time} series={chartSeries}
            windowSeconds={scopedWindowSeconds} yRange={{ kind: 'fixed', min: -1, max: 1 }} yAxisSize={52}
            yTickValues={() => Y_TICKS} yTickFormat={fmtSteer} xTickFormat={fmtTime} refLines={[{ y: 0, dashed: false }]}
            tooltipFormat={tooltipFormat} />}
    </div>
  </div>
}

export default function SteeringChart(props: Props) {
  return <ChartWindowScope section="inputSteering"><SteeringChartContent {...props} /></ChartWindowScope>
}
