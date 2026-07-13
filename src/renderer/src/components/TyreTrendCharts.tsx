import { memo, useCallback, useMemo } from 'react'
import type { CSSProperties } from 'react'
import type uPlot from 'uplot'
import type { TelemetryRow, DamageRow } from '../types'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef, type ChartColors, type AxisLook } from './charts/TimeChartView'

const FL = '#e10600'
const FR = '#4488ff'
const RL = '#37872D'
const RR = '#ffd700'

// Corner colours differ slightly in light mode for contrast. FL is shared.
function cornerColors(isDark: boolean) {
  return {
    fl: FL,
    fr: isDark ? FR : '#0B57D0',
    rl: isDark ? RL : '#137333',
    rr: isDark ? RR : '#B38F00',
  }
}

// Tyre charts are smaller/denser than the Misc charts: 9px axis font, dashed
// grid on both axes, x tick marks, tighter padding.
const TYRE_AXIS_LOOK: AxisLook = {
  font: '9px "Cascadia Code", ui-monospace, monospace',
  xAxisSize: 18,
  paddingRight: 4,
  paddingLeftExtra: 0,
  xGap: 2,
  yGap: 3,
  xTickSpacePx: 60,
  gridDash: [3, 3],
  showYGrid: true,
  xTickSize: 3,
}

function tyreColorsFor(isDark: boolean): ChartColors {
  return {
    axis: isDark ? '#7c8098' : '#6b7280',
    grid: isDark ? '#1e2136' : '#d0d5e0',
    border: isDark ? '#1e2136' : '#d0d5e0',
    tickMark: isDark ? '#555' : '#b0b8cc',
  }
}

const TYRE_TOOLTIP_STYLE: CSSProperties = {
  position: 'absolute',
  display: 'none',
  background: 'var(--bg-panel)',
  border: '1px solid var(--border)',
  borderRadius: 4,
  fontSize: 10,
  padding: '5px 8px',
  pointerEvents: 'none',
  zIndex: 10,
  color: 'var(--text-primary)',
  whiteSpace: 'nowrap',
  boxShadow: '0 4px 16px rgba(0,0,0,0.3)',
}

function fmtTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return `${m}:${String(sec).padStart(2, '0')}`
}

const EMPTY_ALIGNED: uPlot.AlignedData = [new Float64Array(0)]

interface ChartProps<T extends { session_time: number }> {
  title: string
  unit: string
  rows: readonly T[]
  series: SeriesDef<T>[]
  isDark: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
  fastScroll?: boolean
  followSessionClock?: boolean
  minScrollStallS?: number
}

function TyreLineChartImpl<T extends { session_time: number }>({
  title, unit, rows, series, isDark, view = 'chart', windowSeconds = 30,
  fastScroll, followSessionClock, minScrollStallS,
}: ChartProps<T>) {
  const axisColor = isDark ? '#7c8098' : '#6b7280'

  // A single sample can't draw a line, so it counts as no data — otherwise the
  // one damage row delivered by a playback load/seek shows a bare axis frame.
  const hasData = rows.length > 1

  // AlignedData only for the table view.
  const uData = useMemo((): uPlot.AlignedData => {
    if (view !== 'table') return EMPTY_ALIGNED
    const ts = new Float64Array(rows.length)
    const cols = series.map(() => new Float64Array(rows.length))
    rows.forEach((d, i) => {
      ts[i] = d.session_time
      series.forEach((s, k) => { cols[k][i] = s.getY(d) })
    })
    return [ts, ...cols]
  }, [rows, series, view])

  const tableCols = useMemo((): GraphTableColumn[] =>
    series.map((s) => ({ header: s.label, color: s.color, format: (v: number) => `${v.toFixed(1)}${unit}` })),
    [series, unit],
  )

  const tooltipFormat = useCallback((x: number, values: number[]) => {
    let html = `<div style="color:${axisColor};margin-bottom:3px">${fmtTime(x)}</div>`
    for (let i = 0; i < series.length; i++) {
      html += `<div><span style="color:${series[i].color}">${series[i].label}</span>: ${values[i].toFixed(1)}${unit}</div>`
    }
    return html
  }, [series, unit, axisColor])

  return (
    <div className="flex-1 min-w-0 bg-[var(--bg-panel)] p-3 flex flex-col">
      <div className="flex items-center justify-between mb-2 shrink-0">
        <span className="text-[10px] text-[var(--text-secondary)] uppercase tracking-widest">{title}</span>
        {view !== 'table' && (
          <div className="flex gap-3">
            {series.map((s) => (
              <div key={s.label} className="flex items-center gap-1">
                <svg width="16" height="4">
                  <line x1="0" y1="2" x2="16" y2="2" stroke={s.color} strokeWidth="2" />
                </svg>
                <span className="text-[9px] text-[var(--text-secondary)]">{s.label}</span>
              </div>
            ))}
          </div>
        )}
      </div>
      <div className="flex-1 min-h-0 relative">
        {!hasData ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : view === 'table' ? (
          <GraphTable columns={tableCols} data={uData} edgePadRem={0.75} />
        ) : (
          <TimeChartView<T>
            isDark={isDark}
            rows={rows}
            getX={(d) => d.session_time}
            series={series}
            windowSeconds={windowSeconds}
            yRange={{ kind: 'auto' }}
            yAxisSize={42}
            yTickFormat={(v) => `${v}${unit}`}
            xTickFormat={fmtTime}
            tooltipFormat={tooltipFormat}
            colorsFor={tyreColorsFor}
            axisLook={TYRE_AXIS_LOOK}
            tooltipStyle={TYRE_TOOLTIP_STYLE}
            fastScroll={fastScroll}
            followSessionClock={followSessionClock}
            minScrollStallS={minScrollStallS}
          />
        )}
      </div>
    </div>
  )
}

// In particular, keep the sparse Tyre Life leaf cold during 60 Hz telemetry
// publications when its damage rows and configuration have not changed.
const TyreLineChart = memo(TyreLineChartImpl) as typeof TyreLineChartImpl

type TyreGraphViews = { surfaceTemp?: 'chart' | 'table'; innerTemp?: 'chart' | 'table'; brakeTemp?: 'chart' | 'table'; tyreLife?: 'chart' | 'table' }

interface Props {
  telemetry: TelemetryRow[]
  damageHistory: DamageRow[]
  tyreWearMode: 'wear' | 'life'
  visibleGraphs: { surfaceTemp: boolean; innerTemp: boolean; brakeTemp: boolean; tyreLife: boolean }
  isDark: boolean
  layout?: 'row' | 'grid'
  // Per-metric Chart/Table view mode. The same component serves the Overview strip
  // and the Tyres page, which persist independent keys — the caller supplies the map.
  graphViews?: TyreGraphViews
  windowSeconds?: number
  fastScroll?: boolean
}

export default function TyreTrendCharts({ telemetry, damageHistory, tyreWearMode, visibleGraphs, isDark, layout = 'row', graphViews, windowSeconds = 30, fastScroll }: Props) {
  const c = cornerColors(isDark)

  const tempSeries = useCallback((corner: (row: TelemetryRow) => { fl: number; fr: number; rl: number; rr: number }): SeriesDef<TelemetryRow>[] => [
    { label: 'FL', color: c.fl, getY: (d) => corner(d).fl, lineWidth: 2 },
    { label: 'FR', color: c.fr, getY: (d) => corner(d).fr, lineWidth: 2 },
    { label: 'RL', color: c.rl, getY: (d) => corner(d).rl, lineWidth: 2 },
    { label: 'RR', color: c.rr, getY: (d) => corner(d).rr, lineWidth: 2 },
  ], [c.fl, c.fr, c.rl, c.rr])

  const surfaceSeries = useMemo(() => tempSeries((d) => ({ fl: d.tyre_temp_surface_fl, fr: d.tyre_temp_surface_fr, rl: d.tyre_temp_surface_rl, rr: d.tyre_temp_surface_rr })), [tempSeries])
  const innerSeries = useMemo(() => tempSeries((d) => ({ fl: d.tyre_temp_inner_fl, fr: d.tyre_temp_inner_fr, rl: d.tyre_temp_inner_rl, rr: d.tyre_temp_inner_rr })), [tempSeries])
  const brakeSeries = useMemo(() => tempSeries((d) => ({ fl: d.brake_temp_fl, fr: d.brake_temp_fr, rl: d.brake_temp_rl, rr: d.brake_temp_rr })), [tempSeries])

  const life = tyreWearMode === 'life'
  const wearSeries = useMemo((): SeriesDef<DamageRow>[] => [
    { label: 'FL', color: c.fl, getY: (d) => life ? 100 - d.tyre_wear_fl : d.tyre_wear_fl, lineWidth: 2 },
    { label: 'FR', color: c.fr, getY: (d) => life ? 100 - d.tyre_wear_fr : d.tyre_wear_fr, lineWidth: 2 },
    { label: 'RL', color: c.rl, getY: (d) => life ? 100 - d.tyre_wear_rl : d.tyre_wear_rl, lineWidth: 2 },
    { label: 'RR', color: c.rr, getY: (d) => life ? 100 - d.tyre_wear_rr : d.tyre_wear_rr, lineWidth: 2 },
  ], [c.fl, c.fr, c.rl, c.rr, life])

  const wearTitle = life ? 'Tyre Life' : 'Tyre Wear'

  // getY closures for the wear chart depend on tyreWearMode; TimeChartView
  // captures accessors at creation, so remount the wear chart when the mode
  // flips (an occasional user toggle) via a key.
  const surfaceEl = <TyreLineChart<TelemetryRow> title="Surface Temp" unit="°C" rows={telemetry} series={surfaceSeries} isDark={isDark} view={graphViews?.surfaceTemp} windowSeconds={windowSeconds} fastScroll={fastScroll} />
  const innerEl   = <TyreLineChart<TelemetryRow> title="Inner Temp"   unit="°C" rows={telemetry} series={innerSeries}   isDark={isDark} view={graphViews?.innerTemp} windowSeconds={windowSeconds} fastScroll={fastScroll} />
  const brakeEl   = <TyreLineChart<TelemetryRow> title="Brake Temp"   unit="°C" rows={telemetry} series={brakeSeries}   isDark={isDark} view={graphViews?.brakeTemp} windowSeconds={windowSeconds} fastScroll={fastScroll} />
  const wearEl    = <TyreLineChart<DamageRow> key={tyreWearMode} title={wearTitle} unit="%" rows={damageHistory} series={wearSeries} isDark={isDark} view={graphViews?.tyreLife} windowSeconds={windowSeconds} fastScroll followSessionClock minScrollStallS={1} />

  if (layout === 'grid') {
    const items = [
      { key: 'surfaceTemp', el: surfaceEl },
      { key: 'innerTemp',   el: innerEl },
      { key: 'brakeTemp',   el: brakeEl },
      { key: 'tyreLife',    el: wearEl },
    ].filter(({ key }) => visibleGraphs[key as keyof typeof visibleGraphs])

    const odd = items.length % 2 !== 0

    return (
      <div
        className="grid grid-cols-2 h-full gap-[1px] bg-[var(--border)] overflow-hidden"
        style={{ gridAutoRows: '1fr' }}
      >
        {items.map(({ key, el }, i) => (
          <div key={key} className={`h-full flex flex-col overflow-hidden${odd && i === items.length - 1 ? ' col-span-2' : ''}`}>
            {el}
          </div>
        ))}
      </div>
    )
  }

  return (
    <div className="flex h-full divide-x divide-[var(--border)]">
      {visibleGraphs.surfaceTemp && surfaceEl}
      {visibleGraphs.innerTemp   && innerEl}
      {visibleGraphs.brakeTemp   && brakeEl}
      {visibleGraphs.tyreLife    && wearEl}
    </div>
  )
}
