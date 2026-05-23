import { useMemo, useRef, useCallback } from 'react'
import UPlotReact from 'uplot-react'
import uPlot from 'uplot'
import type { StatusRow } from '../types'
import { useSize } from '../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../hooks/useChartTooltip'

interface CP { data: StatusRow[]; isDark: boolean }

const C_ICE    = '#5794F2'
const C_MGUK   = '#FADE2A'
const C_HARV_K = '#37872D'
const C_HARV_H = '#C4162A'
const C_FUEL   = '#F0A500'

function fmtTime(s: number) {
  return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`
}

function colors(isDark: boolean) {
  return {
    ac: isDark ? '#7c8098' : '#6b7280',
    gc: isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)',
    bc: isDark ? '#1e2136' : '#d0d5e0',
  }
}

function xAxis(ac: string, gc: string, bc: string): uPlot.Axis {
  return {
    stroke: ac, font: '11px ui-monospace, monospace',
    ticks: { show: false }, grid: { stroke: gc, width: 1 },
    gap: 2, size: 22, values: (_u, s) => s.map(fmtTime), space: 80,
    border: { stroke: bc, width: 1 },
  }
}

function yAxis(ac: string, fmt: (v: number) => string): uPlot.Axis {
  return {
    stroke: ac, font: '11px ui-monospace, monospace',
    ticks: { show: false }, grid: { show: false }, gap: 4, size: 52,
    values: (_u, s) => s.map(fmt),
  }
}

// ── Power Split (ICE vs MGU-K) ─────────────────────────────────────────────
function PowerSplitChart({ data, isDark }: CP) {
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const mountedRef = useRef(false)
  const visible = width > 0 && height > 0
  if (visible) mountedRef.current = true

  const uData = useMemo((): uPlot.AlignedData => {
    const ts   = new Float64Array(data.length)
    const ice  = new Float64Array(data.length)
    const mguk = new Float64Array(data.length)
    data.forEach((d, i) => {
      ts[i]   = d.session_time
      ice[i]  = d.engine_power_ice_kw  ?? 0
      mguk[i] = d.engine_power_mguk_kw ?? 0
    })
    return [ts, ice, mguk]
  }, [data])

  const opts = useMemo((): uPlot.Options => {
    const { ac, gc, bc } = colors(isDark)
    return {
      width, height, padding: [4, 16, 0, 4],
      legend: { show: false }, cursor: { drag: { setScale: false } },
      scales: { y: { range: [0, 1000] } },
      axes: [xAxis(ac, gc, bc), yAxis(ac, v => `${v}kW`)],
      series: [
        {},
        { label: 'ICE',   stroke: C_ICE,  width: 1.5, points: { show: false } },
        { label: 'MGU-K', stroke: C_MGUK, width: 1.5, points: { show: false } },
      ],
      plugins: [{
        hooks: {
          setCursor: (u) => {
            const i = u.cursor.idx
            if (i == null) { hide(); return }
            const ts   = (u.data[0] as Float64Array)[i]
            const ice  = (u.data[1] as Float64Array)[i]
            const mguk = (u.data[2] as Float64Array)[i]
            show([
              `<div style="color:${ac};margin-bottom:4px">${fmtTime(ts)}</div>`,
              `<div><span style="color:${C_ICE}">ICE</span>: ${ice.toFixed(1)} kW</div>`,
              `<div><span style="color:${C_MGUK}">MGU-K</span>: ${mguk.toFixed(1)} kW</div>`,
              `<div style="color:${ac}">Total: ${(ice + mguk).toFixed(1)} kW</div>`,
            ].join(''), u.cursor.left ?? 0, u.cursor.top ?? 0, width, height)
          },
        },
      }],
    }
  }, [width, height, isDark])

  const onCreate = useCallback((u: uPlot) => { u.over.addEventListener('mouseleave', hide) }, [hide])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Power Split</h2>
        <div className="flex gap-4 text-xs">
          <span style={{ color: C_ICE }}>— ICE</span>
          <span style={{ color: C_MGUK }}>— MGU-K</span>
        </div>
      </div>
      <div className="flex-1 min-h-0 relative" ref={sizeRef}>
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : (
          <>
            <div style={{ position: 'absolute', inset: 0, display: visible ? undefined : 'none' }}>
              {mountedRef.current && <UPlotReact options={opts} data={uData} onCreate={onCreate} />}
            </div>
            <div ref={tooltipRef} style={TOOLTIP_STYLE} />
          </>
        )}
      </div>
    </div>
  )
}

// ── ERS Harvest This Lap ───────────────────────────────────────────────────
function ERSHarvestChart({ data, isDark }: CP) {
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const mountedRef = useRef(false)
  const visible = width > 0 && height > 0
  if (visible) mountedRef.current = true

  const uData = useMemo((): uPlot.AlignedData => {
    const ts = new Float64Array(data.length)
    const hk = new Float64Array(data.length)
    const hh = new Float64Array(data.length)
    data.forEach((d, i) => {
      ts[i] = d.session_time
      hk[i] = (d.ers_harvested_mguk_j ?? 0) / 1000
      hh[i] = (d.ers_harvested_mguh_j ?? 0) / 1000
    })
    return [ts, hk, hh]
  }, [data])

  const opts = useMemo((): uPlot.Options => {
    const { ac, gc, bc } = colors(isDark)
    return {
      width, height, padding: [4, 16, 0, 4],
      legend: { show: false }, cursor: { drag: { setScale: false } },
      axes: [xAxis(ac, gc, bc), yAxis(ac, v => `${v}kJ`)],
      series: [
        {},
        { label: 'MGU-K', stroke: C_HARV_K, width: 1.5, points: { show: false } },
        { label: 'MGU-H', stroke: C_HARV_H, width: 1.5, points: { show: false } },
      ],
      plugins: [{
        hooks: {
          setCursor: (u) => {
            const i = u.cursor.idx
            if (i == null) { hide(); return }
            const ts = (u.data[0] as Float64Array)[i]
            const hk = (u.data[1] as Float64Array)[i]
            const hh = (u.data[2] as Float64Array)[i]
            show([
              `<div style="color:${ac};margin-bottom:4px">${fmtTime(ts)}</div>`,
              `<div><span style="color:${C_HARV_K}">MGU-K</span>: ${hk.toFixed(1)} kJ</div>`,
              `<div><span style="color:${C_HARV_H}">MGU-H</span>: ${hh.toFixed(1)} kJ</div>`,
              `<div style="color:${ac}">Total: ${(hk + hh).toFixed(1)} kJ</div>`,
            ].join(''), u.cursor.left ?? 0, u.cursor.top ?? 0, width, height)
          },
        },
      }],
    }
  }, [width, height, isDark])

  const onCreate = useCallback((u: uPlot) => { u.over.addEventListener('mouseleave', hide) }, [hide])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">ERS Harvest</h2>
        <div className="flex gap-4 text-xs">
          <span style={{ color: C_HARV_K }}>— MGU-K</span>
          <span style={{ color: C_HARV_H }}>— MGU-H</span>
          <span className="text-[var(--text-secondary)]">resets each lap</span>
        </div>
      </div>
      <div className="flex-1 min-h-0 relative" ref={sizeRef}>
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : (
          <>
            <div style={{ position: 'absolute', inset: 0, display: visible ? undefined : 'none' }}>
              {mountedRef.current && <UPlotReact options={opts} data={uData} onCreate={onCreate} />}
            </div>
            <div ref={tooltipRef} style={TOOLTIP_STYLE} />
          </>
        )}
      </div>
    </div>
  )
}

// ── ERS Store History ──────────────────────────────────────────────────────
function ERSStoreChart({ data, isDark }: CP) {
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const mountedRef = useRef(false)
  const visible = width > 0 && height > 0
  if (visible) mountedRef.current = true

  const uData = useMemo((): uPlot.AlignedData => {
    const ts  = new Float64Array(data.length)
    const pct = new Float64Array(data.length)
    data.forEach((d, i) => { ts[i] = d.session_time; pct[i] = d.ers_pct })
    return [ts, pct]
  }, [data])

  const opts = useMemo((): uPlot.Options => {
    const { ac, gc, bc } = colors(isDark)
    return {
      width, height, padding: [4, 16, 0, 4],
      legend: { show: false }, cursor: { drag: { setScale: false } },
      scales: { y: { range: [0, 100] } },
      axes: [xAxis(ac, gc, bc), yAxis(ac, v => `${v}%`)],
      series: [
        {},
        { label: 'ERS', stroke: C_ICE, width: 1.5, points: { show: false } },
      ],
      plugins: [{
        hooks: {
          setCursor: (u) => {
            const i = u.cursor.idx
            if (i == null) { hide(); return }
            const ts  = (u.data[0] as Float64Array)[i]
            const pct = (u.data[1] as Float64Array)[i]
            show([
              `<div style="color:${ac};margin-bottom:4px">${fmtTime(ts)}</div>`,
              `<div><span style="color:${C_ICE}">ERS Store</span>: ${pct.toFixed(1)}%</div>`,
              `<div style="color:${ac}">${(pct / 100 * 4).toFixed(2)} / 4.00 MJ</div>`,
            ].join(''), u.cursor.left ?? 0, u.cursor.top ?? 0, width, height)
          },
        },
      }],
    }
  }, [width, height, isDark])

  const onCreate = useCallback((u: uPlot) => { u.over.addEventListener('mouseleave', hide) }, [hide])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">ERS Store</h2>
        <div className="flex gap-4 text-xs">
          <span style={{ color: C_ICE }}>— Store %</span>
          <span className="text-[var(--text-secondary)]">max 4.0 MJ</span>
        </div>
      </div>
      <div className="flex-1 min-h-0 relative" ref={sizeRef}>
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : (
          <>
            <div style={{ position: 'absolute', inset: 0, display: visible ? undefined : 'none' }}>
              {mountedRef.current && <UPlotReact options={opts} data={uData} onCreate={onCreate} />}
            </div>
            <div ref={tooltipRef} style={TOOLTIP_STYLE} />
          </>
        )}
      </div>
    </div>
  )
}

// ── Fuel History ───────────────────────────────────────────────────────────
function FuelHistoryChart({ data, isDark }: CP) {
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const mountedRef = useRef(false)
  const visible = width > 0 && height > 0
  if (visible) mountedRef.current = true

  const uData = useMemo((): uPlot.AlignedData => {
    const ts = new Float64Array(data.length)
    const kg = new Float64Array(data.length)
    data.forEach((d, i) => { ts[i] = d.session_time; kg[i] = d.fuel_kg })
    return [ts, kg]
  }, [data])

  const opts = useMemo((): uPlot.Options => {
    const { ac, gc, bc } = colors(isDark)
    return {
      width, height, padding: [4, 16, 0, 4],
      legend: { show: false }, cursor: { drag: { setScale: false } },
      axes: [xAxis(ac, gc, bc), yAxis(ac, v => `${v}kg`)],
      series: [
        {},
        { label: 'Fuel', stroke: C_FUEL, width: 1.5, points: { show: false } },
      ],
      plugins: [{
        hooks: {
          setCursor: (u) => {
            const i = u.cursor.idx
            if (i == null) { hide(); return }
            const ts = (u.data[0] as Float64Array)[i]
            const kg = (u.data[1] as Float64Array)[i]
            show([
              `<div style="color:${ac};margin-bottom:4px">${fmtTime(ts)}</div>`,
              `<div><span style="color:${C_FUEL}">Fuel</span>: ${kg.toFixed(2)} kg</div>`,
            ].join(''), u.cursor.left ?? 0, u.cursor.top ?? 0, width, height)
          },
        },
      }],
    }
  }, [width, height, isDark])

  const onCreate = useCallback((u: uPlot) => { u.over.addEventListener('mouseleave', hide) }, [hide])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Fuel History</h2>
        <div className="flex gap-4 text-xs">
          <span style={{ color: C_FUEL }}>— Fuel kg</span>
        </div>
      </div>
      <div className="flex-1 min-h-0 relative" ref={sizeRef}>
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : (
          <>
            <div style={{ position: 'absolute', inset: 0, display: visible ? undefined : 'none' }}>
              {mountedRef.current && <UPlotReact options={opts} data={uData} onCreate={onCreate} />}
            </div>
            <div ref={tooltipRef} style={TOOLTIP_STYLE} />
          </>
        )}
      </div>
    </div>
  )
}

interface VisibleCharts {
  powerSplit: boolean; ersHarvest: boolean; ersStore: boolean; fuelHistory: boolean
}

// ── Main export ────────────────────────────────────────────────────────────
export default function PowerBreakdownChart({ data, isDark, visibleCharts }: { data: StatusRow[]; isDark: boolean; visibleCharts: VisibleCharts }) {
  const items = [
    { key: 'powerSplit',  el: <PowerSplitChart  data={data} isDark={isDark} /> },
    { key: 'ersHarvest',  el: <ERSHarvestChart  data={data} isDark={isDark} /> },
    { key: 'ersStore',    el: <ERSStoreChart    data={data} isDark={isDark} /> },
    { key: 'fuelHistory', el: <FuelHistoryChart data={data} isDark={isDark} /> },
  ].filter(({ key }) => visibleCharts[key as keyof VisibleCharts])

  const odd = items.length % 2 !== 0

  return (
    <div
      className="h-full grid grid-cols-2 gap-[1px] bg-[var(--border)] overflow-hidden"
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
