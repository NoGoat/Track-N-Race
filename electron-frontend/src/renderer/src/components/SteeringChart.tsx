import { useCallback, useMemo } from 'react'
import type { AlignedTable, TelemetryRow } from '../types'
import { useChartDataProfiler } from '../hooks/useChartDataProfiler'
import { useTelemetryStore } from '../stores/telemetryStore'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef } from './charts/TimeChartView'

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

export default function SteeringChart({ isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const data = useTelemetryStore(s => s.telemetry)
  useChartDataProfiler('Steering', data)
  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table') return EMPTY_ALIGNED
    const ts = new Float64Array(data.length), steer = new Float64Array(data.length)
    data.forEach((d, i) => { ts[i] = d.session_time; steer[i] = d.steering })
    return [ts, steer]
  }, [data, view])
  const axisColor = isDark ? '#7c8098' : '#6b7280'
  const tooltipFormat = useCallback((x: number, v: number[]) => [
    `<div style="color:${axisColor};margin-bottom:4px">${fmtTime(x)}</div>`,
    `<div><span style="color:${COLOR_STEER}">Steering</span>: ${fmtSteer(v[0])}</div>`,
  ].join(''), [axisColor])

  return <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
    <div className="flex items-center justify-between mb-3 shrink-0">
      <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Steering</h2>
      {view !== 'table' && <div className="flex gap-4 text-xs"><span style={{ color: COLOR_STEER }}>— Input</span><span className="text-[var(--text-secondary)]">L = left / R = right</span></div>}
    </div>
    <div className="flex-1 min-h-0 relative">
      {data.length === 0 ? <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        : view === 'table' ? <GraphTable columns={TABLE_COLS} data={tableData} />
          : <TimeChartView<TelemetryRow> isDark={isDark} rows={data} getX={d => d.session_time} series={SERIES}
            windowSeconds={windowSeconds} yRange={{ kind: 'fixed', min: -1, max: 1 }} yAxisSize={52}
            yTickValues={() => Y_TICKS} yTickFormat={fmtSteer} xTickFormat={fmtTime} refLines={[{ y: 0, dashed: false }]}
            tooltipFormat={tooltipFormat} profilerLabel="Steering" />}
    </div>
  </div>
}
