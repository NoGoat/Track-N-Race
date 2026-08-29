import { useCallback, useMemo, useState } from 'react'
import { useTelemetryStore } from '../stores/telemetryStore'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef } from './charts/TimeChartView'
import type { AlignedTable, MotionRow } from '../types'
import { useChartCoordinates } from '../lib/chartCoordinates'
import { formatChartComparisonTooltip } from '../lib/chartComparisonTooltip'
import { ChartWindowOverrideSelect, ChartWindowScope, useChartWindowSeconds } from '../lib/chartWindowOverrides'
import { themeSeriesColor } from '../lib/themeColors'
import { HISTORY_ROW } from '../lib/historyDependencies'
import { MISC_CHART_Y_AXIS_SIZE } from '../lib/miscChartLayout'
import type { GraphSection } from '../lib/graphSections'

export type GForceChartMode = 'combined' | 'lateral' | 'longitudinal'
interface Props {
  isDark: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
  mode?: GForceChartMode
}

const COLOR_LAT  = '#F0A500'
const COLOR_LONG = '#5794F2'

const Y_TICKS = [-6, -4, -2, 0, 2, 4, 6]

const SERIES: SeriesDef<MotionRow>[] = [
  { label: 'Lateral',      color: COLOR_LAT,  getY: d => d.g_lat },
  { label: 'Longitudinal', color: COLOR_LONG, getY: d => d.g_long },
]

const MODE_CONFIG: Record<GForceChartMode, { title: string; section: GraphSection; order: number; directionHint: string }> = {
  combined: { title: 'G-Force', section: 'miscGForce', order: 10, directionHint: '+ve = right / accel' },
  lateral: { title: 'Lateral G-Force', section: 'miscGLateral', order: 10, directionHint: '+ve = right' },
  longitudinal: { title: 'Longitudinal G-Force', section: 'miscGLongitudinal', order: 20, directionHint: '+ve = accel' },
}

function fmtTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return `${m}:${String(sec).padStart(2, '0')}`
}

function GForceChartContent({ isDark, view = 'chart', windowSeconds = 30, mode = 'combined' }: Props) {
  const coordinates = useChartCoordinates()
  const scopedWindowSeconds = useChartWindowSeconds(windowSeconds)
  const colorLat = themeSeriesColor(COLOR_LAT, isDark)
  const colorLong = themeSeriesColor(COLOR_LONG, isDark)
  const modeConfig = MODE_CONFIG[mode]
  const selectedSeries = useMemo(() => mode === 'combined'
    ? SERIES
    : SERIES.filter(series => series.label === (mode === 'lateral' ? 'Lateral' : 'Longitudinal')),
  [mode])
  const themedSeries = useMemo(() => selectedSeries.map(series => ({
    ...series,
    color: series.label === 'Lateral' ? colorLat : colorLong,
  })), [colorLat, colorLong, selectedSeries])
  const [hiddenSeries, setHiddenSeries] = useState<Record<string, boolean>>({})
  const toggleSeries = useCallback((label: string) => {
    setHiddenSeries(prev => ({ ...prev, [label]: !prev[label] }))
  }, [])
  const tableColumns = useMemo<GraphTableColumn[]>(() => themedSeries.map(series => ({
    header: series.label,
    color: series.color,
    format: value => `${value.toFixed(2)}g`,
  })), [themedSeries])
  const chartSeries = useMemo(() => themedSeries.map(series => ({
    ...series,
    visible: !hiddenSeries[series.label],
  })), [hiddenSeries, themedSeries])
  const data = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapMotion : s.motion)
  const getTableValues = useCallback((row: MotionRow) => selectedSeries.map(series => series.getY(row)), [selectedSeries])
  const emptyTableData = useMemo((): AlignedTable => [
    new Float64Array(0),
    ...selectedSeries.map(() => new Float64Array(0)),
  ], [selectedSeries])

  // Only the table view needs the columnar AlignedData; the chart feeds rows
  // straight into TimeChartView.
  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table' || coordinates.allLapsMode) return emptyTableData
    const ts   = new Float64Array(data.length)
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
      : [`<div><span style="color:${series.color}">${series.label}</span>: ${values[index].toFixed(2)} g</div>`]).join('')
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
    formatRow: (row: MotionRow) => [
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
            <span className="text-[var(--text-secondary)]">{modeConfig.directionHint}</span>
        </div>}
      </div>
      <div className="flex-1 min-h-0 relative">
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : view === 'table' ? (
          <GraphTable columns={tableColumns} data={tableData} liveRows={data} getLiveValues={getTableValues} allLapsDataMask={HISTORY_ROW.motion} />
        ) : (
          <TimeChartView<MotionRow>
            key={`${mode}:${coordinates.mode ?? (coordinates.allLapsMode ? 'AL' : 'time')}`}
            isDark={isDark}
            rows={data}
            allLapsDataMask={HISTORY_ROW.motion}
            comparisonRows={coordinates.comparisonMode ? coordinates.lapData?.motion : undefined}
            getX={d => d.session_time}
            series={chartSeries}
            windowSeconds={scopedWindowSeconds}
            yRange={{ kind: 'fixed', min: -6, max: 6 }}
            yAxisSize={MISC_CHART_Y_AXIS_SIZE}
            yTickValues={() => Y_TICKS}
            yTickFormat={v => `${v}g`}
            xTickFormat={fmtTime}
            refLines={[{ y: 0, dashed: false }, { y: 4, dashed: true }, { y: -4, dashed: true }]}
            tooltipFormat={tooltipFormat}
            cursorSync={cursorSync}
          />
        )}
      </div>
    </div>
  )
}

export default function GForceChart(props: Props) {
  const section = MODE_CONFIG[props.mode ?? 'combined'].section
  return <ChartWindowScope section={section}><GForceChartContent {...props} /></ChartWindowScope>
}
