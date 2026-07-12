import { useCallback, useMemo } from 'react'
import type uPlot from 'uplot'
import { useChartDataProfiler } from '../hooks/useChartDataProfiler'
import { useTelemetryStore } from '../stores/telemetryStore'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef } from './charts/TimeChartView'
import type { MotionExRow } from '../types'

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

const EMPTY_ALIGNED: uPlot.AlignedData = [new Float64Array(0), new Float64Array(0), new Float64Array(0)]

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

export default function RideHeightChart({ isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const data = useTelemetryStore(s => s.motionEx)
  useChartDataProfiler('RideHeight', data)

  const uData = useMemo((): uPlot.AlignedData => {
    if (view !== 'table') return EMPTY_ALIGNED
    const ts    = new Float64Array(data.length)
    const front = new Float64Array(data.length)
    const rear  = new Float64Array(data.length)
    data.forEach((d, i) => {
      ts[i]    = d.session_time
      front[i] = d.front_aero_height_mm
      rear[i]  = d.rear_aero_height_mm
    })
    return [ts, front, rear]
  }, [data, view])

  const tooltipTimeColor = isDark ? '#7c8098' : '#6b7280'
  const tooltipFormat = useCallback((x: number, v: number[]) => [
    `<div style="color:${tooltipTimeColor};margin-bottom:4px">${fmtTime(x)}</div>`,
    `<div><span style="color:${COLOR_FRONT}">Front</span>: ${v[0].toFixed(1)} mm</div>`,
    `<div><span style="color:${COLOR_REAR}">Rear</span>:  ${v[1].toFixed(1)} mm</div>`,
  ].join(''), [tooltipTimeColor])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Ride Height</h2>
        {view !== 'table' && (
          <div className="flex gap-4 text-xs">
            <span style={{ color: COLOR_FRONT }}>— Front</span>
            <span style={{ color: COLOR_REAR }}>— Rear</span>
            <span className="text-[var(--text-secondary)]">plank edge above road</span>
          </div>
        )}
      </div>
      <div className="flex-1 min-h-0 relative">
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : view === 'table' ? (
          <GraphTable columns={TABLE_COLS} data={uData} />
        ) : (
          <TimeChartView<MotionExRow>
            isDark={isDark}
            rows={data}
            getX={d => d.session_time}
            series={SERIES}
            windowSeconds={windowSeconds}
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
            profilerLabel="RideHeight"
          />
        )}
      </div>
    </div>
  )
}
