import { memo, useCallback, useMemo, useState } from 'react'
import type { AlignedTable, TelemetryRow } from '../types'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import SpeedRpmTimeChart, {
  type SpeedRpmSeriesColors,
  type SpeedRpmSeriesId,
  type SpeedRpmSeriesVisibility,
} from './charts/SpeedRpmTimeChart'
import { useChartCoordinates } from '../lib/chartCoordinates'
import { formatChartComparisonTooltip } from '../lib/chartComparisonTooltip'
import { useTelemetryStore } from '../stores/telemetryStore'
import { ChartWindowOverrideSelect, ChartWindowScope, useChartWindowSeconds } from '../lib/chartWindowOverrides'
import { themeSeriesColor } from '../lib/themeColors'
import { HISTORY_ROW } from '../lib/historyDependencies'

// Dashboard shell: selects store data and owns the panel, table, legend, and
// tooltip presentation. WebGL lifecycle and data projection stay in the leaf.
interface Props {
  isDark: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
}

const COLOR_SPEED = '#37872D'
const COLOR_RPM = '#C4162A'
const COLOR_ERS_DARK = '#FADE2A'
const COLOR_ERS_LIGHT = '#765900'
const EMPTY_TABLE: AlignedTable = [new Float64Array(0)]
const LEGEND_ITEMS: ReadonlyArray<{ id: SpeedRpmSeriesId; label: string }> = [
  { id: 'speed', label: 'Speed (kph)' },
  { id: 'rpm', label: 'RPM' },
  { id: 'ers', label: 'ERS (%)' },
]

function fmtTime(s: number) { return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}` }

function SpeedRpmChartContent(props: Props) {
  const { isDark, view = 'chart', windowSeconds = 30 } = props
  const coordinates = useChartCoordinates()
  const scopedWindowSeconds = useChartWindowSeconds(windowSeconds)
  const colorSpeed = themeSeriesColor(COLOR_SPEED, isDark)
  const colorRpm = themeSeriesColor(COLOR_RPM, isDark)
  const colorErs = isDark ? COLOR_ERS_DARK : COLOR_ERS_LIGHT
  const colors = useMemo<SpeedRpmSeriesColors>(() => ({
    speed: colorSpeed,
    rpm: colorRpm,
    ers: colorErs,
  }), [colorErs, colorRpm, colorSpeed])
  const [visibleSeries, setVisibleSeries] = useState<SpeedRpmSeriesVisibility>({
    speed: true,
    rpm: true,
    ers: true,
  })
  const toggleSeries = useCallback((id: SpeedRpmSeriesId) => {
    setVisibleSeries(prev => ({ ...prev, [id]: !prev[id] }))
  }, [])
  const tableColumns = useMemo<GraphTableColumn[]>(() => [
    { header: 'Speed', color: colorSpeed, format: v => `${Math.round(v)}` },
    { header: 'RPM', color: colorRpm, format: v => Math.round(v).toLocaleString() },
    { header: 'ERS', color: colorErs, format: v => `${Math.round(v)}%` },
  ], [colorErs, colorRpm, colorSpeed])
  const data = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapTelemetry : s.telemetry)
  const statusHistory = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapStatusHistory : s.statusHistory)
  const getTableValues = useCallback((row: TelemetryRow) => {
    let lo = 0, hi = statusHistory.length
    while (lo < hi) {
      const mid = (lo + hi) >> 1
      if (statusHistory[mid].session_time <= row.session_time) lo = mid + 1
      else hi = mid
    }
    return [row.speed_kph, row.rpm, statusHistory[Math.max(0, lo - 1)]?.ers_pct ?? 0]
  }, [statusHistory])
  const tooltipFormat = useCallback((x: number, current: number[], comparison?: number[]) => {
    const formatValues = (source: number[]) => {
      const parts: string[] = []
      if (visibleSeries.speed) parts.push(`<div><span style="color:${colorSpeed}">Speed</span>: ${Math.round(source[0])} kph</div>`)
      if (visibleSeries.rpm) parts.push(`<div><span style="color:${colorRpm}">RPM</span>: ${Math.round(source[1]).toLocaleString()}</div>`)
      if (visibleSeries.ers) parts.push(`<div><span style="color:${colorErs}">ERS</span>: ${Math.round(source[2])}%</div>`)
      return parts.join('')
    }
    return [
      `<div style="color:var(--text-secondary);margin-bottom:4px">${coordinates.distanceMode ? coordinates.formatX(x) : fmtTime(x)}</div>`,
      formatValues(current),
      formatChartComparisonTooltip(comparison, coordinates.mode, formatValues),
    ].join('')
  }, [colorErs, colorRpm, colorSpeed, coordinates, visibleSeries])

  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table' || data.length === 0 || coordinates.allLapsMode) return EMPTY_TABLE
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

  return <div className="bg-[var(--bg-panel)] px-4 pb-4 pt-3 flex flex-col h-full">
    <div className="flex h-[22px] items-center justify-between mb-3 shrink-0">
      <div className="flex items-center gap-0">
        <h2 className="pr-[4px] text-[10px] leading-none text-[var(--text-secondary)] uppercase tracking-widest">Speed + RPM + ERS</h2>
        <ChartWindowOverrideSelect />
      </div>
      {view !== 'table' && <div className="flex items-center gap-4 text-xs">
          {LEGEND_ITEMS.map(item => <button
            key={item.id}
            type="button"
            onClick={() => toggleSeries(item.id)}
            aria-pressed={visibleSeries[item.id]}
            className="cursor-pointer select-none"
            style={{ color: colors[item.id], filter: visibleSeries[item.id] ? undefined : 'grayscale(100%)' }}
          >
            — {item.label}
          </button>)}
      </div>}
    </div>
    <div className="flex-1 min-h-0 relative">
      {data.length === 0
        ? <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data — start driving to see telemetry</div>
        : view === 'table'
          ? <GraphTable columns={tableColumns} data={tableData} liveRows={data} getLiveValues={getTableValues} allLapsDataMask={HISTORY_ROW.telemetry | HISTORY_ROW.status} />
          : <SpeedRpmTimeChart isDark={isDark} telemetry={data} statuses={statusHistory}
              comparisonTelemetry={coordinates.comparisonMode ? coordinates.lapData?.telemetry : undefined}
              comparisonStatuses={coordinates.comparisonMode ? coordinates.lapData?.statusHistory : undefined}
              colors={colors}
              visibleSeries={visibleSeries}
              windowSeconds={scopedWindowSeconds} xTickFormat={fmtTime} tooltipFormat={tooltipFormat} />}
    </div>
  </div>
}

function SpeedRpmChart(props: Props) {
  return <ChartWindowScope section="overviewTelemetry"><SpeedRpmChartContent {...props} /></ChartWindowScope>
}

export default memo(SpeedRpmChart)
