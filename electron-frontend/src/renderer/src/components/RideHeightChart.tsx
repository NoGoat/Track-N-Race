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

interface Props {
  isDark: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
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

const TABLE_COLS: GraphTableColumn[] = [
  { header: 'Front', color: COLOR_FRONT, format: v => `${v.toFixed(1)}mm` },
  { header: 'Rear',  color: COLOR_REAR,  format: v => `${v.toFixed(1)}mm` },
]

const SERIES: SeriesDef<MotionExRow>[] = [
  { label: 'Front', color: COLOR_FRONT, getY: d => d.front_aero_height_mm },
  { label: 'Rear',  color: COLOR_REAR,  getY: d => d.rear_aero_height_mm },
]

const EMPTY_ALIGNED: AlignedTable = [new Float64Array(0), new Float64Array(0), new Float64Array(0)]

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

function RideHeightChartContent({ isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const coordinates = useChartCoordinates()
  const scopedWindowSeconds = useChartWindowSeconds(windowSeconds)
  const colorFront = themeSeriesColor(COLOR_FRONT, isDark)
  const colorRear = themeSeriesColor(COLOR_REAR, isDark)
  const [hiddenSeries, setHiddenSeries] = useState<Record<string, boolean>>({})
  const toggleSeries = useCallback((label: string) => {
    setHiddenSeries(prev => ({ ...prev, [label]: !prev[label] }))
  }, [])
  const tableColumns = useMemo(() => TABLE_COLS.map((column, index) => ({ ...column, color: index === 0 ? colorFront : colorRear })), [colorFront, colorRear])
  const chartSeries = useMemo(() => SERIES.map((series, index) => ({
    ...series,
    color: index === 0 ? colorFront : colorRear,
    visible: !hiddenSeries[series.label],
  })), [colorFront, colorRear, hiddenSeries])
  const data = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapMotionEx : s.motionEx)
  const getTableValues = useCallback((row: MotionExRow) => [row.front_aero_height_mm, row.rear_aero_height_mm], [])

  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table' || coordinates.allLapsMode) return EMPTY_ALIGNED
    const ts    = new Float64Array(data.length)
    const front = new Float64Array(data.length)
    const rear  = new Float64Array(data.length)
    data.forEach((d, i) => {
      ts[i]    = d.session_time
      front[i] = d.front_aero_height_mm
      rear[i]  = d.rear_aero_height_mm
    })
    return [ts, front, rear]
  }, [coordinates.allLapsMode, data, view])

  const tooltipTimeColor = isDark ? '#7c8098' : '#596168'
  const tooltipFormat = useCallback((x: number, v: number[], comparison?: number[]) => {
    const formatValues = (values: number[]) => {
      const parts: string[] = []
      if (!hiddenSeries['Front']) parts.push(`<div><span style="color:${colorFront}">Front</span>: ${values[0].toFixed(1)} mm</div>`)
      if (!hiddenSeries['Rear']) parts.push(`<div><span style="color:${colorRear}">Rear</span>:  ${values[1].toFixed(1)} mm</div>`)
      return parts.join('')
    }
    return [
      `<div style="color:${tooltipTimeColor};margin-bottom:4px">${coordinates.distanceMode ? coordinates.formatX(x) : fmtTime(x)}</div>`,
      formatValues(v),
      formatChartComparisonTooltip(comparison, coordinates.mode, formatValues),
    ].join('')
  }, [colorFront, colorRear, coordinates, hiddenSeries, tooltipTimeColor])

  return (
    <div className="chart-panel bg-[var(--bg-panel)] h-full flex flex-col">
      <div className="flex h-[22px] items-center justify-between mb-3 shrink-0">
        <div className="flex items-center gap-0">
          <h2 className="chart-panel-title text-[10px] leading-none text-[var(--text-secondary)] uppercase tracking-widest">Ride Height</h2>
          <ChartWindowOverrideSelect />
        </div>
        {view !== 'table' && <div className="flex items-center gap-4 text-xs">
            <span
              onClick={() => toggleSeries('Front')}
              className="cursor-pointer select-none"
              style={{ color: colorFront, filter: hiddenSeries['Front'] ? 'grayscale(100%)' : undefined }}
            >
              — Front
            </span>
            <span
              onClick={() => toggleSeries('Rear')}
              className="cursor-pointer select-none"
              style={{ color: colorRear, filter: hiddenSeries['Rear'] ? 'grayscale(100%)' : undefined }}
            >
              — Rear
            </span>
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
            key={coordinates.mode ?? (coordinates.allLapsMode ? 'AL' : 'time')}
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
            yAxisSize={40}
            yTickValues={computeYSplits}
            yTickFormat={v => `${v}mm`}
            xTickFormat={fmtTime}
            tooltipFormat={tooltipFormat}
          />
        )}
      </div>
    </div>
  )
}

export default function RideHeightChart(props: Props) {
  return <ChartWindowScope section="miscRideHeight"><RideHeightChartContent {...props} /></ChartWindowScope>
}
