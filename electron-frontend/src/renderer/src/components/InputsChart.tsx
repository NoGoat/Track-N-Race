import { useCallback, useMemo, useState } from 'react'
import type { AlignedTable, TelemetryRow } from '../types'
import { useTelemetryStore } from '../stores/telemetryStore'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef } from './charts/TimeChartView'
import { useChartCoordinates } from '../lib/chartCoordinates'
import { formatChartComparisonTooltip } from '../lib/chartComparisonTooltip'
import { ChartWindowOverrideSelect, ChartWindowScope, useChartWindowSeconds } from '../lib/chartWindowOverrides'
import type { GraphSection } from '../lib/graphSections'
import { themeSeriesColor } from '../lib/themeColors'

export type PedalChartMode = 'combined' | 'combined2' | 'accelerator' | 'brake'

interface Props {
  isDark: boolean
  mode?: PedalChartMode
  showAccelerator?: boolean
  showBrake?: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
}

const COLOR_ACCELERATOR = '#37872D'
const COLOR_BRAKE = '#C4162A'
const COMBINED_Y_TICKS = [-1, -0.5, 0, 0.5, 1]
const SINGLE_Y_TICKS = [0, 0.5, 1]
const ACCELERATOR_SERIES: SeriesDef<TelemetryRow> = {
  label: 'Accelerator',
  color: COLOR_ACCELERATOR,
  getY: d => d.throttle,
  lineType: 1,
  stepLocation: 0,
  fill: 'rgba(55,135,45,0.2)',
  fillBaseline: 0,
}
const COMBINED_BRAKE_SERIES: SeriesDef<TelemetryRow> = {
  label: 'Brake',
  color: COLOR_BRAKE,
  getY: d => -d.brake,
  lineType: 1,
  stepLocation: 0,
  fill: 'rgba(196,22,42,0.2)',
  fillBaseline: 0,
}
const BRAKE_SERIES: SeriesDef<TelemetryRow> = {
  ...COMBINED_BRAKE_SERIES,
  getY: d => d.brake,
}
const OVERLAY_ACCELERATOR_SERIES: SeriesDef<TelemetryRow> = {
  label: 'Accelerator',
  color: COLOR_ACCELERATOR,
  getY: d => d.throttle,
  lineWidth: 2.25,
  lineType: 1,
  stepLocation: 0,
}
const OVERLAY_BRAKE_SERIES: SeriesDef<TelemetryRow> = {
  label: 'Brake',
  color: COLOR_BRAKE,
  getY: d => d.brake,
  lineWidth: 2.25,
  lineType: 1,
  stepLocation: 0,
}
const SERIES_BY_MODE: Record<PedalChartMode, SeriesDef<TelemetryRow>[]> = {
  combined: [ACCELERATOR_SERIES, COMBINED_BRAKE_SERIES],
  combined2: [OVERLAY_ACCELERATOR_SERIES, OVERLAY_BRAKE_SERIES],
  accelerator: [ACCELERATOR_SERIES],
  brake: [BRAKE_SERIES],
}
const EMPTY_ALIGNED: AlignedTable = [new Float64Array(0)]

function fmtTime(s: number) { return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}` }
function fmtPercent(v: number) { return `${Math.round(Math.abs(v) * 100)}%` }

function InputsChartContent({
  isDark,
  mode = 'combined',
  showAccelerator = true,
  showBrake = true,
  view = 'chart',
  windowSeconds = 30,
}: Props) {
  const coordinates = useChartCoordinates()
  const scopedWindowSeconds = useChartWindowSeconds(windowSeconds)
  const colorAccelerator = themeSeriesColor(COLOR_ACCELERATOR, isDark)
  const colorBrake = themeSeriesColor(COLOR_BRAKE, isDark)
  const [hiddenSeries, setHiddenSeries] = useState<Record<string, boolean>>({})
  const toggleSeries = useCallback((label: string) => {
    setHiddenSeries(prev => ({ ...prev, [label]: !prev[label] }))
  }, [])

  const baseSeries = SERIES_BY_MODE[mode]
  const chartSeries = useMemo(() => baseSeries.map(series => ({
    ...series,
    color: series.label === 'Accelerator' ? colorAccelerator : colorBrake,
    visible: (series.label === 'Accelerator' ? showAccelerator : showBrake) && !hiddenSeries[series.label],
  })), [baseSeries, colorAccelerator, colorBrake, hiddenSeries, showAccelerator, showBrake])

  const tableSeries = useMemo(() => baseSeries.flatMap(series => {
    const enabled = series.label === 'Accelerator' ? showAccelerator : showBrake
    if (!enabled) return []
    const color = series.label === 'Accelerator' ? colorAccelerator : colorBrake
    const column: GraphTableColumn = { header: series.label, color, format: fmtPercent }
    return [{ column, getValue: series.getY }]
  }), [baseSeries, colorAccelerator, colorBrake, showAccelerator, showBrake])
  const tableColumns = useMemo(() => tableSeries.map(series => series.column), [tableSeries])
  const getTableValues = useCallback(
    (row: TelemetryRow) => tableSeries.map(series => series.getValue(row)),
    [tableSeries],
  )

  const data = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapTelemetry : s.telemetry)
  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table' || coordinates.allLapsMode) return EMPTY_ALIGNED
    const columns = [new Float64Array(data.length), ...tableSeries.map(() => new Float64Array(data.length))]
    data.forEach((row, rowIndex) => {
      columns[0][rowIndex] = row.session_time
      tableSeries.forEach((series, seriesIndex) => {
        columns[seriesIndex + 1][rowIndex] = series.getValue(row)
      })
    })
    return columns
  }, [coordinates.allLapsMode, data, tableSeries, view])

  const axisColor = isDark ? '#7c8098' : '#596168'
  const tooltipFormat = useCallback((x: number, values: number[], comparison?: number[]) => {
    const formatValues = (sample: number[]) => baseSeries.flatMap((series, index) => {
      const enabled = series.label === 'Accelerator' ? showAccelerator : showBrake
      if (!enabled || hiddenSeries[series.label]) return []
      const color = series.label === 'Accelerator' ? colorAccelerator : colorBrake
      return [`<div><span style="color:${color}">${series.label}</span>: ${fmtPercent(sample[index])}</div>`]
    }).join('')
    return [
      `<div style="color:${axisColor};margin-bottom:4px">${coordinates.distanceMode ? coordinates.formatX(x) : fmtTime(x)}</div>`,
      formatValues(values),
      formatChartComparisonTooltip(comparison, coordinates.mode, formatValues),
    ].join('')
  }, [axisColor, baseSeries, colorAccelerator, colorBrake, coordinates, hiddenSeries, showAccelerator, showBrake])

  const title = mode === 'combined' || mode === 'combined2' ? 'Accelerator / Brake' : mode === 'accelerator' ? 'Accelerator' : 'Brake'
  const combined = mode === 'combined'
  const overlaid = mode === 'combined2'
  const cursorSync = useMemo(() => ({
    id: SECTION_BY_MODE[mode],
    order: mode === 'brake' ? 30 : 20,
    formatRow: (row: TelemetryRow) => baseSeries.flatMap(series => {
      const enabled = series.label === 'Accelerator' ? showAccelerator : showBrake
      if (!enabled || hiddenSeries[series.label]) return []
      const color = series.label === 'Accelerator' ? colorAccelerator : colorBrake
      return [`<div><span style="color:${color}">${series.label}</span>: ${fmtPercent(series.getY(row))}</div>`]
    }).join(''),
  }), [baseSeries, colorAccelerator, colorBrake, hiddenSeries, mode, showAccelerator, showBrake])
  return <div className="bg-[var(--bg-panel)] px-4 pb-4 pt-3 h-full flex flex-col">
    <div className="flex h-[22px] items-center justify-between mb-3 shrink-0">
      <div className="flex items-center gap-0">
        <h2 className="pr-[4px] text-[10px] leading-none text-[var(--text-secondary)] uppercase tracking-widest">{title}</h2>
        <ChartWindowOverrideSelect />
      </div>
      {view !== 'table' && <div className="flex items-center gap-4 text-xs">
        {mode !== 'brake' && showAccelerator && <span
          onClick={() => toggleSeries('Accelerator')}
          className="cursor-pointer select-none"
          style={{ color: colorAccelerator, filter: hiddenSeries.Accelerator ? 'grayscale(100%)' : undefined }}
        >
          {overlaid ? '—' : '▲'} Accelerator
        </span>}
        {mode !== 'accelerator' && showBrake && <span
          onClick={() => toggleSeries('Brake')}
          className="cursor-pointer select-none"
          style={{ color: colorBrake, filter: hiddenSeries.Brake ? 'grayscale(100%)' : undefined }}
        >
          {combined ? '▼' : overlaid ? '—' : '▲'} Brake
        </span>}
      </div>}
    </div>
    <div className="flex-1 min-h-0 relative">
      {data.length === 0 ? <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        : view === 'table' ? <GraphTable columns={tableColumns} data={tableData} liveRows={data} getLiveValues={getTableValues} />
          : <TimeChartView<TelemetryRow> key={`${mode}:${coordinates.mode ?? (coordinates.allLapsMode ? 'AL' : 'time')}`} isDark={isDark} rows={data} comparisonRows={coordinates.comparisonMode ? coordinates.lapData?.telemetry : undefined} getX={d => d.session_time} series={chartSeries}
            windowSeconds={scopedWindowSeconds} yRange={{ kind: 'fixed', min: combined ? -1 : 0, max: 1 }} yAxisSize={52}
            yTickValues={() => combined ? COMBINED_Y_TICKS : SINGLE_Y_TICKS} yTickFormat={fmtPercent} xTickFormat={fmtTime}
            refLines={[{ y: 0, dashed: false }]} tooltipFormat={tooltipFormat} cursorSync={cursorSync} />}
    </div>
  </div>
}

const SECTION_BY_MODE: Record<PedalChartMode, GraphSection> = {
  combined: 'inputThrottleBrake',
  combined2: 'inputThrottleBrakeOverlay',
  accelerator: 'inputAccelerator',
  brake: 'inputBrake',
}

export default function InputsChart(props: Props) {
  const mode = props.mode ?? 'combined'
  return <ChartWindowScope section={SECTION_BY_MODE[mode]}><InputsChartContent {...props} mode={mode} /></ChartWindowScope>
}
