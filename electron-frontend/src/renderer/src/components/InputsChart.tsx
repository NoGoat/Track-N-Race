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
const COLOR_THROTTLE = '#37872D', COLOR_BRAKE = '#C4162A'
const Y_TICKS = [-1, -0.5, 0, 0.5, 1]
const SERIES: SeriesDef<TelemetryRow>[] = [
  { label: 'Throttle', color: COLOR_THROTTLE, getY: d => d.throttle, lineType: 1, stepLocation: 0, fill: 'rgba(55,135,45,0.2)', fillBaseline: 0 },
  { label: 'Brake', color: COLOR_BRAKE, getY: d => -d.brake, lineType: 1, stepLocation: 0, fill: 'rgba(196,22,42,0.2)', fillBaseline: 0 },
]
const TABLE_COLS: GraphTableColumn[] = [
  { header: 'Throttle', color: COLOR_THROTTLE, format: v => `${Math.round(v * 100)}%` },
  { header: 'Brake', color: COLOR_BRAKE, format: v => `${Math.round(Math.abs(v) * 100)}%` },
]
const EMPTY_ALIGNED: AlignedTable = [new Float64Array(0)]
function fmtTime(s: number) { return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}` }

function InputsChartContent({ isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const coordinates = useChartCoordinates()
  const scopedWindowSeconds = useChartWindowSeconds(windowSeconds)
  const colorThrottle = themeSeriesColor(COLOR_THROTTLE, isDark)
  const colorBrake = themeSeriesColor(COLOR_BRAKE, isDark)
  const [hiddenSeries, setHiddenSeries] = useState<Record<string, boolean>>({})
  const toggleSeries = useCallback((label: string) => {
    setHiddenSeries(prev => ({ ...prev, [label]: !prev[label] }))
  }, [])
  const chartSeries = useMemo(() => SERIES.map((series, index) => ({
    ...series,
    color: index === 0 ? colorThrottle : colorBrake,
    visible: !hiddenSeries[series.label],
  })), [colorBrake, colorThrottle, hiddenSeries])
  const tableColumns = useMemo(() => TABLE_COLS.map((column, index) => ({ ...column, color: index === 0 ? colorThrottle : colorBrake })), [colorBrake, colorThrottle])
  const data = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapTelemetry : s.telemetry)
  const getTableValues = useCallback((row: TelemetryRow) => [row.throttle, -row.brake], [])
  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table' || coordinates.allLapsMode) return EMPTY_ALIGNED
    const ts = new Float64Array(data.length), throttle = new Float64Array(data.length), brake = new Float64Array(data.length)
    data.forEach((d, i) => { ts[i] = d.session_time; throttle[i] = d.throttle; brake[i] = -d.brake })
    return [ts, throttle, brake]
  }, [coordinates.allLapsMode, data, view])
  const axisColor = isDark ? '#7c8098' : '#596168'
  const tooltipFormat = useCallback((x: number, v: number[], comparison?: number[]) => {
    const formatValues = (values: number[]) => {
      const parts: string[] = []
      if (!hiddenSeries['Throttle']) parts.push(`<div><span style="color:${colorThrottle}">Throttle</span>: ${Math.round(values[0] * 100)}%</div>`)
      if (!hiddenSeries['Brake']) parts.push(`<div><span style="color:${colorBrake}">Brake</span>: ${Math.round(Math.abs(values[1]) * 100)}%</div>`)
      return parts.join('')
    }
    return [
      `<div style="color:${axisColor};margin-bottom:4px">${coordinates.distanceMode ? coordinates.formatX(x) : fmtTime(x)}</div>`,
      formatValues(v),
      formatChartComparisonTooltip(comparison, coordinates.mode, formatValues),
    ].join('')
  }, [axisColor, colorBrake, colorThrottle, coordinates, hiddenSeries])

  return <div className="bg-[var(--bg-panel)] px-4 pb-4 pt-3 h-full flex flex-col">
    <div className="flex h-[22px] items-center justify-between mb-3 shrink-0">
      <div className="flex items-center gap-0">
        <h2 className="pr-[4px] text-[10px] leading-none text-[var(--text-secondary)] uppercase tracking-widest">Throttle</h2>
        <ChartWindowOverrideSelect />
      </div>
      {view !== 'table' && <div className="flex items-center gap-4 text-xs">
        <span
          onClick={() => toggleSeries('Throttle')}
          className="cursor-pointer select-none"
          style={{ color: colorThrottle, filter: hiddenSeries['Throttle'] ? 'grayscale(100%)' : undefined }}
        >
          ▲ Throttle
        </span>
        <span
          onClick={() => toggleSeries('Brake')}
          className="cursor-pointer select-none"
          style={{ color: colorBrake, filter: hiddenSeries['Brake'] ? 'grayscale(100%)' : undefined }}
        >
          ▼ Brake
        </span>
      </div>}
    </div>
    <div className="flex-1 min-h-0 relative">
      {data.length === 0 ? <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        : view === 'table' ? <GraphTable columns={tableColumns} data={tableData} liveRows={data} getLiveValues={getTableValues} />
          : <TimeChartView<TelemetryRow> key={coordinates.mode ?? (coordinates.allLapsMode ? 'AL' : 'time')} isDark={isDark} rows={data} comparisonRows={coordinates.comparisonMode ? coordinates.lapData?.telemetry : undefined} getX={d => d.session_time} series={chartSeries}
            windowSeconds={scopedWindowSeconds} yRange={{ kind: 'fixed', min: -1, max: 1 }} yAxisSize={40}
            yTickValues={() => Y_TICKS} yTickFormat={v => `${Math.round(Math.abs(v) * 100)}%`} xTickFormat={fmtTime}
            refLines={[{ y: 0, dashed: false }]} tooltipFormat={tooltipFormat} />}
    </div>
  </div>
}

export default function InputsChart(props: Props) {
  return <ChartWindowScope section="inputThrottleBrake"><InputsChartContent {...props} /></ChartWindowScope>
}
