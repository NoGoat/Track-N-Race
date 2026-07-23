import { useCallback, useMemo } from 'react'
import type { AlignedTable, TelemetryRow } from '../types'
import { useTelemetryStore } from '../stores/telemetryStore'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef } from './charts/TimeChartView'

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

export default function GearChart({ isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const data = useTelemetryStore(s => s.telemetry)
  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table') return EMPTY_ALIGNED
    const ts = new Float64Array(data.length), gear = new Float64Array(data.length)
    data.forEach((d, i) => { ts[i] = d.session_time; gear[i] = d.gear })
    return [ts, gear]
  }, [data, view])
  const axisColor = isDark ? '#7c8098' : '#6b7280'
  const tooltipFormat = useCallback((x: number, v: number[]) => [
    `<div style="color:${axisColor};margin-bottom:4px">${fmtTime(x)}</div>`,
    `<div style="color:${COLOR_GEAR}">Gear ${Math.round(v[0])}</div>`,
  ].join(''), [axisColor])

  return <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
    <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest mb-3 shrink-0">Gear</h2>
    <div className="flex-1 min-h-0 relative">
      {data.length === 0 ? <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        : view === 'table' ? <GraphTable columns={TABLE_COLS} data={tableData} />
          : <TimeChartView<TelemetryRow> isDark={isDark} rows={data} getX={d => d.session_time} series={SERIES}
              windowSeconds={windowSeconds} yRange={{ kind: 'fixed', min: 0.5, max: 8.5 }} yAxisSize={28}
              yTickValues={() => GEAR_TICKS} yTickFormat={v => String(v)} xTickFormat={fmtTime}
              refLines={[2, 4, 6].map(y => ({ y, dashed: true }))} tooltipFormat={tooltipFormat} />}
    </div>
  </div>
}
