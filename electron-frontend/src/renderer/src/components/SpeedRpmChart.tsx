import { useCallback, useMemo } from 'react'
import type { AlignedTable, StatusRow, TelemetryRow } from '../types'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import SpeedRpmTimeChart from './charts/SpeedRpmTimeChart'
import { useChartCoordinates } from '../lib/chartCoordinates'
import { formatChartComparisonTooltip } from '../lib/chartComparisonTooltip'

interface Props {
  data: TelemetryRow[]
  statusHistory: StatusRow[]
  isDark: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
}

const COLOR_SPEED = '#37872D'
const COLOR_RPM = '#C4162A'
const COLOR_ERS = '#FADE2A'
const TABLE_COLS: GraphTableColumn[] = [
  { header: 'Speed', color: COLOR_SPEED, format: v => `${Math.round(v)}` },
  { header: 'RPM', color: COLOR_RPM, format: v => Math.round(v).toLocaleString() },
  { header: 'ERS', color: COLOR_ERS, format: v => `${Math.round(v)}%` },
]
const EMPTY_TABLE: AlignedTable = [new Float64Array(0)]

function fmtTime(s: number) { return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}` }

function raw(value: number, max: number): number {
  return Number.isNaN(value) ? NaN : Math.round(value * max)
}

export default function SpeedRpmChart({ data, statusHistory, isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const coordinates = useChartCoordinates()
  const tooltipFormat = useCallback((x: number, current: number[], comparison?: number[]) => {
    const formatValues = (source: number[]) => {
      const values = [raw(source[0], 380), raw(source[1], 16000), raw(source[2], 100)]
      return [
        `<div><span style="color:${COLOR_SPEED}">Speed</span>: ${values[0]} kph</div>`,
        `<div><span style="color:${COLOR_RPM}">RPM</span>: ${values[1].toLocaleString()}</div>`,
        `<div><span style="color:${COLOR_ERS}">ERS</span>: ${values[2]}%</div>`,
      ].join('')
    }
    return [
      `<div style="color:var(--text-secondary);margin-bottom:4px">${coordinates.distanceMode ? coordinates.formatX(x) : fmtTime(x)}</div>`,
      formatValues(current),
      formatChartComparisonTooltip(comparison, coordinates.mode, formatValues),
    ].join('')
  }, [coordinates])

  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table' || data.length === 0) return EMPTY_TABLE
    const ts = new Float64Array(data.length)
    const speed = new Float64Array(data.length)
    const rpm = new Float64Array(data.length)
    const ers = new Float64Array(data.length)
    let si = 0
    data.forEach((row, i) => {
      while (si + 1 < statusHistory.length && statusHistory[si + 1].session_time <= row.session_time) si++
      ts[i] = row.session_time
      speed[i] = row.speed_kph
      rpm[i] = row.rpm
      ers[i] = statusHistory[si]?.ers_pct ?? 0
    })
    return [ts, speed, rpm, ers]
  }, [coordinates, data, statusHistory, view])

  return <div className="bg-[var(--bg-panel)] p-4 flex flex-col h-full">
    <div className="flex items-center justify-between mb-3 shrink-0">
      <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Speed + RPM + ERS</h2>
      {view !== 'table' && <div className="flex gap-4 text-xs">
        <span style={{ color: COLOR_SPEED }}>— Speed (kph)</span>
        <span style={{ color: COLOR_RPM }}>— RPM</span>
        <span style={{ color: COLOR_ERS }}>— ERS (%)</span>
      </div>}
    </div>
    <div className="flex-1 min-h-0 relative">
      {data.length === 0
        ? <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data — start driving to see telemetry</div>
        : view === 'table'
          ? <GraphTable columns={TABLE_COLS} data={tableData} />
          : <SpeedRpmTimeChart key={isDark ? 'dark' : 'light'} isDark={isDark} telemetry={data} statuses={statusHistory}
              comparisonTelemetry={coordinates.comparisonMode ? coordinates.lapData?.telemetry : undefined}
              comparisonStatuses={coordinates.comparisonMode ? coordinates.lapData?.statusHistory : undefined}
              windowSeconds={windowSeconds} xTickFormat={fmtTime} tooltipFormat={tooltipFormat} />}
    </div>
  </div>
}
