import { useMemo, useRef, useCallback } from 'react'
import UPlotReact from 'uplot-react'
import uPlot from 'uplot'
import type { TelemetryRow, DamageRow } from '../types'
import { useSize } from '../hooks/useSize'
import { useChartTooltip } from '../hooks/useChartTooltip'
import { useScrollScale } from '../hooks/useScrollScale'
import GraphTable, { type GraphTableColumn } from './GraphTable'

const FL = '#e10600'
const FR = '#4488ff'
const RL = '#37872D'
const RR = '#ffd700'

const LEGEND = [
  { label: 'FL', color: FL,  dash: undefined    },
  { label: 'FR', color: FR,  dash: '6 3'        },
  { label: 'RL', color: RL,  dash: '12 4'       },
  { label: 'RR', color: RR,  dash: '10 3 2 3'   },
]

const TOOLTIP_STYLE: React.CSSProperties = {
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

function makeTtPlugin(
  show: (html: string, left: number, top: number, w: number, h: number) => void,
  hide: () => void,
  unit: string,
  colors: string[],
  labels: string[],
  width: number,
  height: number,
  axisColor: string,
) {
  const plugin: uPlot.Plugin = {
    hooks: {
      setCursor: (u) => {
        const idx = u.cursor.idx
        if (idx == null) { hide(); return }
        const ts = (u.data[0] as Float64Array)[idx]
        let html = `<div style="color:${axisColor};margin-bottom:3px">${fmtTime(ts)}</div>`
        for (let i = 1; i <= 4; i++) {
          const val = (u.data[i] as Float64Array)[idx]
          html += `<div><span style="color:${colors[i - 1]}">${labels[i - 1]}</span>: ${val.toFixed(1)}${unit}</div>`
        }
        show(html, u.cursor.left ?? 0, u.cursor.top ?? 0, width, height)
      },
    },
  }
  return plugin
}

function makeOpts(unit: string, w: number, h: number, ttPlugin: uPlot.Plugin, isDark: boolean, solid = false): uPlot.Options {
  const axisColor = isDark ? '#7c8098' : '#6b7280'
  const gridColor = isDark ? '#1e2136' : '#d0d5e0'
  const tickColor = isDark ? '#555'    : '#b0b8cc'

  const flColor = FL
  const frColor = isDark ? FR : '#0B57D0'
  const rlColor = isDark ? RL : '#137333'
  const rrColor = isDark ? RR : '#B38F00'

  return {
    width: w,
    height: h,
    padding: [4, 4, 0, 0],
    legend: { show: false },
    cursor: { drag: { setScale: false } },
    axes: [
      {
        stroke: axisColor,
        font: '9px "Cascadia Code", ui-monospace, monospace',
        ticks: { stroke: tickColor, size: 3 },
        grid:  { stroke: gridColor, width: 1, dash: [3, 3] },
        gap: 2,
        size: 18,
        values: (_u, splits) => splits.map(fmtTime),
        space: 60,
      },
      {
        stroke: axisColor,
        font: '9px "Cascadia Code", ui-monospace, monospace',
        size: 42,
        ticks: { show: false },
        grid:  { stroke: gridColor, width: 1, dash: [3, 3] },
        gap: 3,
        values: (_u, splits) => splits.map(v => `${v}${unit}`),
      },
    ],
    series: [
      {},
      { label: 'FL', stroke: flColor, width: 2, points: { show: false } },
      { label: 'FR', stroke: frColor, width: 2, dash: solid ? undefined : [6,  3],       points: { show: false } },
      { label: 'RL', stroke: rlColor, width: 2, dash: solid ? undefined : [12, 4],       points: { show: false } },
      { label: 'RR', stroke: rrColor, width: 2, dash: solid ? undefined : [10, 3, 2, 3], points: { show: false } },
    ],
    plugins: [ttPlugin],
  }
}

interface ChartProps {
  title: string
  unit: string
  data: uPlot.AlignedData
  isDark: boolean
  solid?: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
  // Wear comes from the sparse ~1-2 Hz damage rows; the scroll clock needs a
  // longer extrapolation window there than for the 60 Hz telemetry streams.
  sparse?: boolean
}

const SPARSE_SCROLL = { stallS: 1.5, snapS: 2 }

function TyreLineChart({ title, unit, data, isDark, solid, view = 'chart', windowSeconds = 30, sparse = false }: ChartProps) {
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const mountedRef = useRef(false)
  const visible = width > 0 && height > 0
  if (visible) mountedRef.current = true

  const ts = data[0]
  const { attach, detach } = useScrollScale(
    view !== 'table',
    ts.length > 0 ? (ts[ts.length - 1] as number) : null,
    ts.length > 0 ? (ts[0] as number) : null,
    windowSeconds,
    sparse ? SPARSE_SCROLL : undefined,
  )

  const axisColor = isDark ? '#7c8098' : '#6b7280'

  const flColor = FL
  const frColor = isDark ? FR : '#0B57D0'
  const rlColor = isDark ? RL : '#137333'
  const rrColor = isDark ? RR : '#B38F00'

  const ttPlugin = useMemo(
    () => makeTtPlugin(show, hide, unit, [flColor, frColor, rlColor, rrColor], ['FL', 'FR', 'RL', 'RR'], width, height, axisColor),
    [show, hide, unit, width, height, axisColor, flColor, frColor, rlColor, rrColor],
  )

  const opts = useMemo(
    () => makeOpts(unit, width, height, ttPlugin, isDark, solid),
    [unit, width, height, ttPlugin, isDark, solid],
  )

  const onCreate = useCallback((u: uPlot) => {
    attach(u)
    u.over.addEventListener('mouseleave', hide)
  }, [hide, attach])

  const tableCols = useMemo((): GraphTableColumn[] => [
    { header: 'FL', color: flColor, format: v => `${v.toFixed(1)}${unit}` },
    { header: 'FR', color: frColor, format: v => `${v.toFixed(1)}${unit}` },
    { header: 'RL', color: rlColor, format: v => `${v.toFixed(1)}${unit}` },
    { header: 'RR', color: rrColor, format: v => `${v.toFixed(1)}${unit}` },
  ], [flColor, frColor, rlColor, rrColor, unit])

  const legend = [
    { label: 'FL', color: flColor,  dash: undefined    },
    { label: 'FR', color: frColor,  dash: '6 3'        },
    { label: 'RL', color: rlColor,  dash: '12 4'       },
    { label: 'RR', color: rrColor,  dash: '10 3 2 3'   },
  ]

  return (
    <div className="flex-1 min-w-0 bg-[var(--bg-panel)] p-3 flex flex-col">
      <div className="flex items-center justify-between mb-2 shrink-0">
        <span className="text-[10px] text-[var(--text-secondary)] uppercase tracking-widest">{title}</span>
        {view !== 'table' && (
          <div className="flex gap-3">
            {legend.map(({ label, color, dash }) => (
              <div key={label} className="flex items-center gap-1">
                <svg width="16" height="4">
                  <line x1="0" y1="2" x2="16" y2="2" stroke={color} strokeWidth="2" strokeDasharray={solid ? undefined : dash} />
                </svg>
                <span className="text-[9px] text-[var(--text-secondary)]">{label}</span>
              </div>
            ))}
          </div>
        )}
      </div>
      <div className="flex-1 min-h-0 relative" ref={sizeRef}>
        {view === 'table' ? (
          <GraphTable columns={tableCols} data={data} edgePadRem={0.75} />
        ) : (
          <>
            <div style={{ position: 'absolute', inset: 0, display: visible ? undefined : 'none' }}>
              {mountedRef.current && <UPlotReact options={opts} data={data} onCreate={onCreate} onDelete={detach} resetScales={false} />}
            </div>
            <div ref={tooltipRef} style={TOOLTIP_STYLE} />
          </>
        )}
      </div>
    </div>
  )
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
}

export default function TyreTrendCharts({ telemetry, damageHistory, tyreWearMode, visibleGraphs, isDark, layout = 'row', graphViews, windowSeconds = 30 }: Props) {
  const surface = useMemo((): uPlot.AlignedData => {
    const ts = new Float64Array(telemetry.length)
    const fl = new Float64Array(telemetry.length), fr = new Float64Array(telemetry.length)
    const rl = new Float64Array(telemetry.length), rr = new Float64Array(telemetry.length)
    telemetry.forEach((d, i) => {
      ts[i] = d.session_time
      fl[i] = d.tyre_temp_surface_fl; fr[i] = d.tyre_temp_surface_fr
      rl[i] = d.tyre_temp_surface_rl; rr[i] = d.tyre_temp_surface_rr
    })
    return [ts, fl, fr, rl, rr]
  }, [telemetry])

  const inner = useMemo((): uPlot.AlignedData => {
    const ts = new Float64Array(telemetry.length)
    const fl = new Float64Array(telemetry.length), fr = new Float64Array(telemetry.length)
    const rl = new Float64Array(telemetry.length), rr = new Float64Array(telemetry.length)
    telemetry.forEach((d, i) => {
      ts[i] = d.session_time
      fl[i] = d.tyre_temp_inner_fl; fr[i] = d.tyre_temp_inner_fr
      rl[i] = d.tyre_temp_inner_rl; rr[i] = d.tyre_temp_inner_rr
    })
    return [ts, fl, fr, rl, rr]
  }, [telemetry])

  const brakes = useMemo((): uPlot.AlignedData => {
    const ts = new Float64Array(telemetry.length)
    const fl = new Float64Array(telemetry.length), fr = new Float64Array(telemetry.length)
    const rl = new Float64Array(telemetry.length), rr = new Float64Array(telemetry.length)
    telemetry.forEach((d, i) => {
      ts[i] = d.session_time
      fl[i] = d.brake_temp_fl; fr[i] = d.brake_temp_fr
      rl[i] = d.brake_temp_rl; rr[i] = d.brake_temp_rr
    })
    return [ts, fl, fr, rl, rr]
  }, [telemetry])

  const wear = useMemo((): uPlot.AlignedData => {
    const ts = new Float64Array(damageHistory.length)
    const fl = new Float64Array(damageHistory.length), fr = new Float64Array(damageHistory.length)
    const rl = new Float64Array(damageHistory.length), rr = new Float64Array(damageHistory.length)
    damageHistory.forEach((d, i) => {
      ts[i] = d.session_time
      fl[i] = tyreWearMode === 'life' ? 100 - d.tyre_wear_fl : d.tyre_wear_fl
      fr[i] = tyreWearMode === 'life' ? 100 - d.tyre_wear_fr : d.tyre_wear_fr
      rl[i] = tyreWearMode === 'life' ? 100 - d.tyre_wear_rl : d.tyre_wear_rl
      rr[i] = tyreWearMode === 'life' ? 100 - d.tyre_wear_rr : d.tyre_wear_rr
    })
    return [ts, fl, fr, rl, rr]
  }, [damageHistory, tyreWearMode])

  const wearTitle = tyreWearMode === 'life' ? 'Tyre Life' : 'Tyre Wear'

  if (layout === 'grid') {
    const items = [
      { key: 'surfaceTemp', el: <TyreLineChart title="Surface Temp" unit="°C" data={surface} isDark={isDark} solid view={graphViews?.surfaceTemp} windowSeconds={windowSeconds} /> },
      { key: 'innerTemp',   el: <TyreLineChart title="Inner Temp"   unit="°C" data={inner}   isDark={isDark} solid view={graphViews?.innerTemp} windowSeconds={windowSeconds} /> },
      { key: 'brakeTemp',   el: <TyreLineChart title="Brake Temp"   unit="°C" data={brakes}  isDark={isDark} solid view={graphViews?.brakeTemp} windowSeconds={windowSeconds} /> },
      { key: 'tyreLife',    el: <TyreLineChart title={wearTitle}    unit="%"  data={wear}     isDark={isDark} solid view={graphViews?.tyreLife} windowSeconds={windowSeconds} sparse /> },
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
      {visibleGraphs.surfaceTemp && <TyreLineChart title="Surface Temp" unit="°C" data={surface} isDark={isDark} view={graphViews?.surfaceTemp} windowSeconds={windowSeconds} />}
      {visibleGraphs.innerTemp   && <TyreLineChart title="Inner Temp"   unit="°C" data={inner}   isDark={isDark} view={graphViews?.innerTemp} windowSeconds={windowSeconds} />}
      {visibleGraphs.brakeTemp   && <TyreLineChart title="Brake Temp"   unit="°C" data={brakes}  isDark={isDark} view={graphViews?.brakeTemp} windowSeconds={windowSeconds} />}
      {visibleGraphs.tyreLife    && <TyreLineChart title={wearTitle}    unit="%"  data={wear}     isDark={isDark} view={graphViews?.tyreLife} windowSeconds={windowSeconds} sparse />}
    </div>
  )
}
