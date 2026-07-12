import { useCallback, useMemo } from 'react'
import type uPlot from 'uplot'
import type { TelemetryRow } from '../types'
import { useChartDataProfiler } from '../hooks/useChartDataProfiler'
import { useTelemetryStore } from '../stores/telemetryStore'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef } from './charts/TimeChartView'

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
const EMPTY_ALIGNED: uPlot.AlignedData = [new Float64Array(0)]
function fmtTime(s: number) { return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}` }

export default function InputsChart({ isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const data = useTelemetryStore(s => s.telemetry)
  useChartDataProfiler('Inputs', data)
  const uData = useMemo((): uPlot.AlignedData => {
    if (view !== 'table') return EMPTY_ALIGNED
    const ts = new Float64Array(data.length), throttle = new Float64Array(data.length), brake = new Float64Array(data.length)
    data.forEach((d, i) => { ts[i] = d.session_time; throttle[i] = d.throttle; brake[i] = -d.brake })
    return [ts, throttle, brake]
  }, [data, view])
  const axisColor = isDark ? '#7c8098' : '#6b7280'
  const tooltipFormat = useCallback((x: number, v: number[]) => [
    `<div style="color:${axisColor};margin-bottom:4px">${fmtTime(x)}</div>`,
    `<div><span style="color:${COLOR_THROTTLE}">Throttle</span>: ${Math.round(v[0] * 100)}%</div>`,
    `<div><span style="color:${COLOR_BRAKE}">Brake</span>: ${Math.round(Math.abs(v[1]) * 100)}%</div>`,
  ].join(''), [axisColor])

  return <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
    <div className="flex items-center justify-between mb-3 shrink-0">
      <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Throttle</h2>
      {view !== 'table' && <div className="flex gap-4 text-xs"><span style={{ color: COLOR_THROTTLE }}>▲ Throttle</span><span style={{ color: COLOR_BRAKE }}>▼ Brake</span></div>}
    </div>
    <div className="flex-1 min-h-0 relative">
      {data.length === 0 ? <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        : view === 'table' ? <GraphTable columns={TABLE_COLS} data={uData} />
          : <TimeChartView<TelemetryRow> isDark={isDark} rows={data} getX={d => d.session_time} series={SERIES}
            windowSeconds={windowSeconds} yRange={{ kind: 'fixed', min: -1, max: 1 }} yAxisSize={40}
            yTickValues={() => Y_TICKS} yTickFormat={v => `${Math.round(Math.abs(v) * 100)}%`} xTickFormat={fmtTime}
            refLines={[{ y: 0, dashed: false }]} tooltipFormat={tooltipFormat} profilerLabel="Inputs" />}
    </div>
  </div>
}
