import { useCallback, useMemo } from 'react'
import { useTelemetryStore } from '../stores/telemetryStore'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef } from './charts/TimeChartView'
import type { AlignedTable, MotionRow } from '../types'
import { useChartCoordinates } from '../lib/chartCoordinates'

interface Props {
  isDark: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
}

const COLOR_LAT  = '#F0A500'
const COLOR_LONG = '#5794F2'

const Y_TICKS = [-6, -4, -2, 0, 2, 4, 6]

const TABLE_COLS: GraphTableColumn[] = [
  { header: 'Lateral',      color: COLOR_LAT,  format: v => `${v.toFixed(2)}g` },
  { header: 'Longitudinal', color: COLOR_LONG, format: v => `${v.toFixed(2)}g` },
]

const SERIES: SeriesDef<MotionRow>[] = [
  { label: 'Lateral',      color: COLOR_LAT,  getY: d => d.g_lat },
  { label: 'Longitudinal', color: COLOR_LONG, getY: d => d.g_long },
]

const EMPTY_ALIGNED: AlignedTable = [new Float64Array(0), new Float64Array(0), new Float64Array(0)]

function fmtTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return `${m}:${String(sec).padStart(2, '0')}`
}

export default function GForceChart({ isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const coordinates = useChartCoordinates()
  const data = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapMotion : s.motion)

  // Only the table view needs the columnar AlignedData; the chart feeds rows
  // straight into TimeChartView.
  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table') return EMPTY_ALIGNED
    const ts   = new Float64Array(data.length)
    const lat  = new Float64Array(data.length)
    const long = new Float64Array(data.length)
    data.forEach((d, i) => {
      ts[i]   = d.session_time
      lat[i]  = d.g_lat
      long[i] = d.g_long
    })
    return [ts, lat, long]
  }, [data, view])

  const tooltipTimeColor = isDark ? '#7c8098' : '#6b7280'
  const tooltipFormat = useCallback((x: number, v: number[]) => [
    `<div style="color:${tooltipTimeColor};margin-bottom:4px">${coordinates.distanceMode ? coordinates.formatX(x) : fmtTime(x)}</div>`,
    `<div><span style="color:${COLOR_LAT}">Lateral</span>: ${v[0].toFixed(2)} g</div>`,
    `<div><span style="color:${COLOR_LONG}">Longitudinal</span>: ${v[1].toFixed(2)} g</div>`,
  ].join(''), [coordinates, tooltipTimeColor])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">G-Force</h2>
        {view !== 'table' && (
          <div className="flex gap-4 text-xs">
            <span style={{ color: COLOR_LAT }}>— Lateral</span>
            <span style={{ color: COLOR_LONG }}>— Longitudinal</span>
            <span className="text-[var(--text-secondary)]">+ve = right / accel</span>
          </div>
        )}
      </div>
      <div className="flex-1 min-h-0 relative">
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : view === 'table' ? (
          <GraphTable columns={TABLE_COLS} data={tableData} />
        ) : (
          <TimeChartView<MotionRow>
            isDark={isDark}
            rows={data}
            getX={d => d.session_time}
            series={SERIES}
            windowSeconds={windowSeconds}
            yRange={{ kind: 'fixed', min: -6, max: 6 }}
            yAxisSize={36}
            yTickValues={() => Y_TICKS}
            yTickFormat={v => `${v}g`}
            xTickFormat={fmtTime}
            refLines={[{ y: 0, dashed: false }, { y: 4, dashed: true }, { y: -4, dashed: true }]}
            tooltipFormat={tooltipFormat}
          />
        )}
      </div>
    </div>
  )
}
