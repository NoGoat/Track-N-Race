import { memo, useCallback, useLayoutEffect, useMemo, useRef, useState } from 'react'
import type { CSSProperties } from 'react'
import type { AlignedTable, TelemetryRow, DamageRow } from '../types'
import type { GraphSection, TyreYAxisGroupState } from '../lib/graphSections'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import TimeChartView, { type SeriesDef, type ChartColors, type AxisLook, type YRangeSpec } from './charts/TimeChartView'
import { niceTicks } from '../lib/timechart/ticks'
import { useChartCoordinates } from '../lib/chartCoordinates'
import { formatChartComparisonTooltip } from '../lib/chartComparisonTooltip'
import { useTelemetryStore } from '../stores/telemetryStore'
import { ChartWindowOverrideSelect, ChartWindowScope, useChartWindowSeconds } from '../lib/chartWindowOverrides'
import { HISTORY_ROW } from '../lib/historyDependencies'

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
    rr: isDark ? RR : '#765900',
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
    axis: isDark ? '#7c8098' : '#596168',
    grid: isDark ? '#1e2136' : '#afb1ae',
    border: isDark ? '#1e2136' : '#afb1ae',
    tickMark: isDark ? '#555' : '#898e8a',
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

const EMPTY_ALIGNED: AlignedTable = [new Float64Array(0)]
const DYNAMIC_Y_RANGE: YRangeSpec = { kind: 'auto' }
const TEMP_FIXED_Y_RANGE: YRangeSpec = { kind: 'expand', initialLower: 0, initialUpper: 125, lowerPad: 0, upperPad: 0, expandLower: false }
const BRAKE_FIXED_Y_RANGE: YRangeSpec = { kind: 'expand', initialLower: 0, initialUpper: 1250, lowerPad: 0, upperPad: 0, expandLower: false }
const WEAR_FIXED_Y_RANGE: YRangeSpec = { kind: 'fixed', min: 0, max: 100 }
const tyreYTicks = (min: number, max: number) => {
  const ticks = niceTicks(min, max, 6)
  if (ticks.length === 0 || ticks[0] !== min) ticks.unshift(min)
  if (ticks[ticks.length - 1] !== max) ticks.push(max)
  return ticks
}
const temperatureYTicks = (min: number, max: number) => {
  const ticks = tyreYTicks(min, max)
  if (ticks.length < 3) return ticks
  // Add one tick halfway through each of the first two intervals. Because the
  // source ticks are derived from the current domain, these labels keep their
  // midpoint positions and values when an expand-range ceiling changes.
  return [ticks[0], (ticks[0] + ticks[1]) / 2, ticks[1], (ticks[1] + ticks[2]) / 2, ...ticks.slice(2)]
}

function useTyreTitleWidth() {
  const controlsRef = useRef<HTMLDivElement>(null)
  const titleRef = useRef<HTMLSpanElement>(null)

  useLayoutEffect(() => {
    const controls = controlsRef.current
    const title = titleRef.current
    if (!controls || !title) return

    let animationFrame = 0
    const measure = () => {
      const siblingWidth = Array.from(controls.children).reduce((width, child) => (
        child === title ? width : width + child.getBoundingClientRect().width
      ), 0)
      const availableTitleWidth = Math.max(0, controls.clientWidth - siblingWidth)

      title.style.whiteSpace = 'nowrap'
      title.style.width = 'max-content'
      const singleLineWidth = title.getBoundingClientRect().width
      title.style.whiteSpace = 'normal'
      title.style.width = availableTitleWidth + 0.5 < singleLineWidth ? 'min-content' : 'max-content'
    }
    const scheduleMeasure = () => {
      cancelAnimationFrame(animationFrame)
      animationFrame = requestAnimationFrame(measure)
    }

    measure()
    const observer = new ResizeObserver(scheduleMeasure)
    observer.observe(controls)
    for (const child of controls.children) {
      if (child !== title) observer.observe(child)
    }
    return () => {
      cancelAnimationFrame(animationFrame)
      observer.disconnect()
    }
  }, [])

  return { controlsRef, titleRef }
}

interface ChartProps<T extends { session_time: number }> {
  section: GraphSection
  source: 'telemetry' | 'damage'
  title: string
  unit: string
  rows: readonly T[]
  comparisonRows?: readonly T[]
  series: SeriesDef<T>[]
  isDark: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
  fastScroll?: boolean
  followSessionClock?: boolean
  minScrollStallS?: number
  yRange: YRangeSpec
  yTickValues?: (min: number, max: number) => number[]
}

function TyreLineChartImpl<T extends { session_time: number }>(props: ChartProps<T>) {
  const {
    title, unit, series, isDark, view = 'chart', windowSeconds = 30, source,
    fastScroll, followSessionClock, minScrollStallS, yRange, yTickValues = tyreYTicks,
  } = props
  const coordinates = useChartCoordinates()
  const scopedWindowSeconds = useChartWindowSeconds(windowSeconds)
  const rows = useTelemetryStore(s => (source === 'telemetry'
    ? coordinates.distanceMode ? s.analyzeLapTelemetry : s.telemetry
    : coordinates.distanceMode ? s.analyzeLapDamageHistory : s.damageHistory)) as unknown as readonly T[]
  const comparisonRows = coordinates.comparisonMode
    ? (source === 'telemetry' ? coordinates.lapData?.telemetry : coordinates.lapData?.damageHistory) as readonly T[] | undefined
    : undefined
  const axisColor = isDark ? '#7c8098' : '#596168'
  const { controlsRef, titleRef } = useTyreTitleWidth()
  const [hiddenSeries, setHiddenSeries] = useState<Record<string, boolean>>({})
  const toggleSeries = useCallback((label: string) => {
    setHiddenSeries(prev => ({ ...prev, [label]: !prev[label] }))
  }, [])

  const chartSeries = useMemo(() => series.map(s => ({
    ...s,
    visible: !hiddenSeries[s.label],
  })), [series, hiddenSeries])

  // A single sample can't draw a line. Comparison modes can still render a
  // complete selected/previous/fastest trace before the sparse current damage
  // stream has advanced, so do not hide that trace behind the empty state.
  const hasData = rows.length > 1 || (
    view !== 'table' && coordinates.comparisonMode && (comparisonRows?.length ?? 0) > 1
  )

  // AlignedData only for the table view.
  const tableData = useMemo((): AlignedTable => {
    if (view !== 'table' || coordinates.allLapsMode) return EMPTY_ALIGNED
    const ts = new Float64Array(rows.length)
    const cols = series.map(() => new Float64Array(rows.length))
    rows.forEach((d, i) => {
      ts[i] = d.session_time
      series.forEach((s, k) => { cols[k][i] = s.getY(d) })
    })
    return [ts, ...cols]
  }, [coordinates.allLapsMode, rows, series, view])

  const tableCols = useMemo((): GraphTableColumn[] =>
    series.map((s) => ({ header: s.label, color: s.color, format: (v: number) => `${v.toFixed(1)}${unit}` })),
    [series, unit],
  )
  const getTableValues = useCallback((row: T) => series.map(item => item.getY(row)), [series])

  const tooltipFormat = useCallback((x: number, values: number[], comparison?: number[]) => {
    const formatValues = (source: number[]) => {
      let html = ''
      for (let i = 0; i < series.length; i++) {
        if (hiddenSeries[series[i].label]) continue
        html += `<div><span style="color:${series[i].color}">${series[i].label}</span>: ${source[i].toFixed(1)}${unit}</div>`
      }
      return html
    }
    let html = `<div style="color:${axisColor};margin-bottom:3px">${coordinates.distanceMode ? coordinates.formatX(x) : fmtTime(x)}</div>`
    html += formatValues(values)
    html += formatChartComparisonTooltip(comparison, coordinates.mode, formatValues)
    return html
  }, [series, hiddenSeries, unit, axisColor, coordinates])

  return (
    <div className="tyre-chart-container flex-1 min-w-0 bg-[var(--bg-panel)] px-4 pb-4 pt-3 flex flex-col">
      <div className="tyre-chart-header flex h-[22px] items-center justify-between mb-2 shrink-0">
        <div ref={controlsRef} className="tyre-chart-controls flex min-w-0 flex-1 items-center gap-0">
          <span ref={titleRef} className="tyre-chart-title min-w-min shrink-0 whitespace-normal pr-[4px] text-[10px] leading-none text-[var(--text-secondary)] uppercase tracking-widest">{title}</span>
          <ChartWindowOverrideSelect />
        </div>
        {view !== 'table' && <div className="tyre-chart-legend flex shrink-0 items-center gap-3">
            {series.map((s) => {
              const isHidden = !!hiddenSeries[s.label]
              return (
                <div
                  key={s.label}
                  onClick={() => toggleSeries(s.label)}
                  className="flex items-center gap-1 cursor-pointer select-none"
                  style={{ filter: isHidden ? 'grayscale(100%)' : undefined }}
                >
                  <svg width="16" height="4">
                    <line x1="0" y1="2" x2="16" y2="2" stroke={s.color} strokeWidth="2" />
                  </svg>
                  <span className="text-[9px] leading-none text-[var(--text-secondary)]">{s.label}</span>
                </div>
              )
            })}
        </div>}
      </div>
      <div className="flex-1 min-h-0 relative">
        {!hasData ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : view === 'table' ? (
          <GraphTable columns={tableCols} data={tableData} liveRows={rows} getLiveValues={getTableValues} allLapsDataMask={source === 'damage' ? HISTORY_ROW.damage : HISTORY_ROW.telemetry} edgePadRem={0.75} />
        ) : (
          <TimeChartView<T>
            key={coordinates.mode ?? (coordinates.allLapsMode ? 'AL' : 'time')}
            isDark={isDark}
            rows={rows}
            allLapsDataMask={source === 'damage' ? HISTORY_ROW.damage : HISTORY_ROW.telemetry}
            comparisonRows={comparisonRows}
            getX={(d) => d.session_time}
            series={chartSeries}
            windowSeconds={scopedWindowSeconds}
            yRange={yRange}
            yAxisSize={42}
            yTickValues={yTickValues}
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

function ScopedTyreLineChart<T extends { session_time: number }>(props: ChartProps<T>) {
  return <ChartWindowScope section={props.section}><TyreLineChart<T> {...props} /></ChartWindowScope>
}

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
  yAxis: TyreYAxisGroupState
  sectionGroup: 'overview' | 'tyres'
}

export default function TyreTrendCharts({ telemetry, damageHistory, tyreWearMode, visibleGraphs, isDark, layout = 'row', graphViews, windowSeconds = 30, fastScroll, yAxis, sectionGroup }: Props) {
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
  const sections = sectionGroup === 'overview'
    ? { surface: 'overviewTyreSurface', inner: 'overviewTyreInner', brake: 'overviewTyreBrake', wear: 'overviewTyreWear' } as const
    : { surface: 'tyreSurface', inner: 'tyreInner', brake: 'tyreBrake', wear: 'tyreWear' } as const
  const surfaceEl = <ScopedTyreLineChart<TelemetryRow> section={sections.surface} source="telemetry" title="Surface Temp" unit="°C" rows={telemetry} series={surfaceSeries} isDark={isDark} view={graphViews?.surfaceTemp} windowSeconds={windowSeconds} fastScroll={fastScroll} yRange={yAxis.surfaceTemp === 'fixed' ? TEMP_FIXED_Y_RANGE : DYNAMIC_Y_RANGE} yTickValues={temperatureYTicks} />
  const innerEl   = <ScopedTyreLineChart<TelemetryRow> section={sections.inner} source="telemetry" title="Inner Temp" unit="°C" rows={telemetry} series={innerSeries} isDark={isDark} view={graphViews?.innerTemp} windowSeconds={windowSeconds} fastScroll={fastScroll} yRange={yAxis.innerTemp === 'fixed' ? TEMP_FIXED_Y_RANGE : DYNAMIC_Y_RANGE} yTickValues={temperatureYTicks} />
  const brakeEl   = <ScopedTyreLineChart<TelemetryRow> section={sections.brake} source="telemetry" title="Brake Temp" unit="°C" rows={telemetry} series={brakeSeries} isDark={isDark} view={graphViews?.brakeTemp} windowSeconds={windowSeconds} fastScroll={fastScroll} yRange={yAxis.brakeTemp === 'fixed' ? BRAKE_FIXED_Y_RANGE : DYNAMIC_Y_RANGE} yTickValues={temperatureYTicks} />
  const wearEl    = <ScopedTyreLineChart<DamageRow> key={tyreWearMode} section={sections.wear} source="damage" title={wearTitle} unit="%" rows={damageHistory} series={wearSeries} isDark={isDark} view={graphViews?.tyreLife} windowSeconds={windowSeconds} fastScroll followSessionClock minScrollStallS={1} yRange={yAxis.tyreLife === 'fixed' ? WEAR_FIXED_Y_RANGE : DYNAMIC_Y_RANGE} />

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
