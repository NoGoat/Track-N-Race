import { useCallback, useMemo, useState } from 'react'
import { useTelemetryStore } from '../stores/telemetryStore'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef } from './charts/TimeChartView'
import type { AlignedTable, MotionExRow } from '../types'
import { useChartCoordinates } from '../lib/chartCoordinates'
import { formatChartComparisonTooltip } from '../lib/chartComparisonTooltip'
import { ChartWindowOverrideSelect, ChartWindowScope, useChartWindowSeconds } from '../lib/chartWindowOverrides'
import { themeSeriesColor } from '../lib/themeColors'
import { HISTORY_ROW } from '../lib/historyDependencies'
import { MISC_CHART_Y_AXIS_SIZE } from '../lib/miscChartLayout'
import type { GraphSection } from '../lib/graphSections'

export type RideHeightChartMode = 'combined' | 'front' | 'rear'
interface Props {
  isDark: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
  mode?: RideHeightChartMode
}

const COLOR_FRONT = '#73BF69'
const COLOR_REAR  = '#B877DB'

// Static y-range whose bounds only ever expand outward (see TimeChartView's
// expand strategy). Ride height is a continuously-varying signal, so rescanning
// the whole window every scroll tick would be wasteful; we only test the newest
// sample and push a bound out when it's exceeded.
const INITIAL_UPPER_MM = 50
const UPPER_PADDING_MM = 5
const INITIAL_LOWER_MM = 0
const LOWER_PADDING_MM = 2

const SERIES: SeriesDef<MotionExRow>[] = [
  { label: 'Front', color: COLOR_FRONT, getY: d => d.front_aero_height_mm },
  { label: 'Rear',  color: COLOR_REAR,  getY: d => d.rear_aero_height_mm },
]

const MODE_CONFIG: Record<RideHeightChartMode, { title: string; section: GraphSection; order: number }> = {
  combined: { title: 'Ride Height', section: 'miscRideHeight', order: 30 },
  front: { title: 'Front Ride Height', section: 'miscRideFront', order: 30 },
  rear: { title: 'Rear Ride Height', section: 'miscRideRear', order: 40 },
}

function fmtTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return `${m}:${String(sec).padStart(2, '0')}`
}

// 5 evenly-spaced, rounded ticks derived from the current y domain.
function computeYSplits(lower: number, upper: number): number[] {
  const step = (upper - lower) / 4
  const splits: number[] = []
  for (let i = 0; i <= 4; i++) splits.push(Math.round(lower + step * i))
  return splits
}

function RideHeightChartContent({ isDark, view = 'chart', windowSeconds = 30, mode = 'combined' }: Props) {
  const coordinates = useChartCoordinates()
  const scopedWindowSeconds = useChartWindowSeconds(windowSeconds)
  const colorFront = themeSeriesColor(COLOR_FRONT, isDark)
  const colorRear = themeSeriesColor(COLOR_REAR, isDark)
  const modeConfig = MODE_CONFIG[mode]
  const selectedSeries = useMemo(() => mode === 'combined'
    ? SERIES
    : SERIES.filter(series => series.label === (mode === 'front' ? 'Front' : 'Rear')),
  [mode])
  const themedSeries = useMemo(() => selectedSeries.map(series => ({
    ...series,
    color: series.label === 'Front' ? colorFront : colorRear,
  })), [colorFront, colorRear, selectedSeries])
  const [hiddenSeries, setHiddenSeries] = useState<Record<string, boolean>>({})
  const toggleSeries = useCallback((label: string) => {
    setHiddenSeries(prev => ({ ...prev, [label]: !prev[label] }))
  }, [])
  const tableColumns = useMemo<GraphTableColumn[]>(() => themedSeries.map(series => ({
    header: series.label,
    color: series.color,
    format: value => `${value.toFixed(1)}mm`,
  })), [themedSeries])
  const chartSeries = useMemo(() => themedSeries.map(series => ({
    ...series,
    visible: !hiddenSeries[series.label],
  })), [hiddenSeries, themedSeries])
  const data = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapMotionEx : s.motionEx)
  const getTableValues = useCallback((row: MotionExRow) => selectedSeries.map(series => series.getY(row)), [selectedSeries])
  const emptyTableData = useMemo((): AlignedTable => [
    new Float64Array(0),
    ...selectedSeries.map(() => new Float64Array(0)),
  ], [selectedSeries])

  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table' || coordinates.allLapsMode) return emptyTableData
    const ts = new Float64Array(data.length)
    const values = selectedSeries.map(() => new Float64Array(data.length))
    data.forEach((d, i) => {
      ts[i] = d.session_time
      selectedSeries.forEach((series, seriesIndex) => { values[seriesIndex][i] = series.getY(d) })
    })
    return [ts, ...values]
  }, [coordinates.allLapsMode, data, emptyTableData, selectedSeries, view])

  const tooltipTimeColor = isDark ? '#7c8098' : '#596168'
  const formatValues = useCallback((values: number[]) => {
    return themedSeries.flatMap((series, index) => hiddenSeries[series.label]
      ? []
      : [`<div><span style="color:${series.color}">${series.label}</span>: ${values[index].toFixed(1)} mm</div>`]).join('')
  }, [hiddenSeries, themedSeries])
  const tooltipFormat = useCallback((x: number, v: number[], comparison?: number[]) => {
    return [
      `<div style="color:${tooltipTimeColor};margin-bottom:4px">${coordinates.distanceMode ? coordinates.formatX(x) : fmtTime(x)}</div>`,
      formatValues(v),
      formatChartComparisonTooltip(comparison, coordinates.mode, formatValues),
    ].join('')
  }, [coordinates, formatValues, tooltipTimeColor])
  const cursorSync = useMemo(() => ({
    id: modeConfig.section,
    order: modeConfig.order,
    formatRow: (row: MotionExRow) => [
      `<div style="color:${tooltipTimeColor};margin-top:3px">${modeConfig.title}</div>`,
      formatValues(selectedSeries.map(series => series.getY(row))),
    ].join(''),
  }), [formatValues, modeConfig, selectedSeries, tooltipTimeColor])

  return (
    <div className="chart-panel bg-[var(--bg-panel)] h-full flex flex-col">
      <div className="flex h-[22px] items-center justify-between mb-3 shrink-0">
        <div className="flex items-center gap-0">
          <h2 className="chart-panel-title text-[10px] leading-none text-[var(--text-secondary)] uppercase tracking-widest">{modeConfig.title}</h2>
          <ChartWindowOverrideSelect />
        </div>
        {view !== 'table' && <div className="flex items-center gap-4 text-xs">
            {themedSeries.map(series => <span
              key={series.label}
              onClick={() => toggleSeries(series.label)}
              className="cursor-pointer select-none"
              style={{ color: series.color, filter: hiddenSeries[series.label] ? 'grayscale(100%)' : undefined }}
            >
              — {series.label}
            </span>)}
            <span className="text-[var(--text-secondary)]">plank edge above road</span>
        </div>}
      </div>
      <div className="flex-1 min-h-0 relative">
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : view === 'table' ? (
          <GraphTable columns={tableColumns} data={tableData} liveRows={data} getLiveValues={getTableValues} allLapsDataMask={HISTORY_ROW.motionEx} />
        ) : (
          <TimeChartView<MotionExRow>
            key={`${mode}:${coordinates.mode ?? (coordinates.allLapsMode ? 'AL' : 'time')}`}
            isDark={isDark}
            rows={data}
            allLapsDataMask={HISTORY_ROW.motionEx}
            comparisonRows={coordinates.comparisonMode ? coordinates.lapData?.motionEx : undefined}
            getX={d => d.session_time}
            series={chartSeries}
            windowSeconds={scopedWindowSeconds}
            yRange={{
              kind: 'expand',
              initialLower: INITIAL_LOWER_MM,
              initialUpper: INITIAL_UPPER_MM,
              lowerPad: LOWER_PADDING_MM,
              upperPad: UPPER_PADDING_MM,
            }}
            yAxisSize={MISC_CHART_Y_AXIS_SIZE}
            yTickValues={computeYSplits}
            yTickFormat={v => `${v}mm`}
            xTickFormat={fmtTime}
            tooltipFormat={tooltipFormat}
            cursorSync={cursorSync}
          />
        )}
      </div>
    </div>
  )
}

export default function RideHeightChart(props: Props) {
  const section = MODE_CONFIG[props.mode ?? 'combined'].section
  return <ChartWindowScope section={section}><RideHeightChartContent {...props} /></ChartWindowScope>
}
