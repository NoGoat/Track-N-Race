import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import Select, { type SingleValue } from 'react-select'
import type { AlignedTable, TelemetryRow, StatusRow, LapData } from '../types'
import type { DataPoint } from '../lib/timechart/dataBridge'
import { buildSelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import SpeedRpmTimeChart, { type SpeedRpmSeriesSet } from './charts/SpeedRpmTimeChart'

type Mode = 'default' | 'CL' | 'PL' | 'FL' | 'compare'
type LapOption = { value: number; label: string }
type LapBlock = { telemetry: TelemetryRow[]; statusHistory: StatusRow[]; startSessionTime: number; lapNum?: number }

interface Props {
  data: TelemetryRow[]; statusHistory: StatusRow[]; lapData: TelemetryRow[]; lapStatusHistory: StatusRow[]
  lapHistory: LapData[]; fastestLap: LapData | null; speedRpmBlocks: LapBlock[] | null
  mode: Mode; onModeChange: (mode: Mode) => void; isDark: boolean; view?: 'chart' | 'table'
  currentLapNum?: number | null; windowSeconds?: number
}

const COLOR_SPEED = '#37872D', COLOR_RPM = '#C4162A', COLOR_ERS = '#FADE2A'
const COLOR_SPEED_MUTED = 'rgba(55,135,45,0.35)', COLOR_RPM_MUTED = 'rgba(196,22,42,0.35)', COLOR_ERS_MUTED = 'rgba(250,222,42,0.35)'
const TABLE_COLS: GraphTableColumn[] = [
  { header: 'Speed', color: COLOR_SPEED, format: v => `${Math.round(v)}` },
  { header: 'RPM', color: COLOR_RPM, format: v => Math.round(v).toLocaleString() },
  { header: 'ERS', color: COLOR_ERS, format: v => `${Math.round(v)}%` },
]
const EMPTY_TABLE: AlignedTable = [new Float64Array(0)]
const EMPTY_TRIPLE: [DataPoint[], DataPoint[], DataPoint[]] = [[], [], []]

function fmtTime(s: number) { return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}` }
function fmtLapTime(s: number) { return `${Math.floor(s / 60)}:${(s % 60).toFixed(1).padStart(4, '0')}` }

function useNormalizedSeries(telemetry: readonly TelemetryRow[], statuses: readonly StatusRow[], origin: number, revision: string) {
  const cache = useRef({ revision: '', lastX: -Infinity, version: 0, series: [[], [], []] as [DataPoint[], DataPoint[], DataPoint[]] })
  const c = cache.current
  const lastSourceX = telemetry.length ? telemetry[telemetry.length - 1].session_time - origin : -Infinity
  if (c.revision !== revision || lastSourceX < c.lastX) {
    c.revision = revision; c.lastX = -Infinity; c.version++
    c.series.forEach(s => s.splice(0, s.length))
  }
  let lo = 0, hi = telemetry.length
  while (lo < hi) { const mid = (lo + hi) >> 1; if (telemetry[mid].session_time - origin <= c.lastX) lo = mid + 1; else hi = mid }
  if (lo < telemetry.length) {
    let si = 0
    if (statuses.length) {
      let a = 0, b = statuses.length
      while (a < b) { const mid = (a + b) >> 1; if (statuses[mid].session_time <= telemetry[lo].session_time) a = mid + 1; else b = mid }
      si = Math.max(0, a - 1)
    }
    for (let i = lo; i < telemetry.length; i++) {
      const row = telemetry[i]
      while (si + 1 < statuses.length && statuses[si + 1].session_time <= row.session_time) si++
      const x = row.session_time - origin
      c.series[0].push({ x, y: row.speed_kph / 380 })
      c.series[1].push({ x, y: row.rpm / 16000 })
      c.series[2].push({ x, y: (statuses[si]?.ers_pct ?? 0) / 100 })
      c.lastX = x
    }
    c.version++
  }
  if (telemetry.length && c.series[0].length > telemetry.length + 2048) {
    const firstX = telemetry[0].session_time - origin
    let trim = 0
    while (trim < c.series[0].length && c.series[0][trim].x < firstX) trim++
    if (trim) { c.series.forEach(s => s.splice(0, trim)); c.version++ }
  }
  return { series: c.series, version: c.version }
}

function raw(v: number, max: number): number { return Number.isNaN(v) ? NaN : Math.round(v * max) }

export default function SpeedRpmChart({ data, statusHistory, lapData, lapStatusHistory, lapHistory, fastestLap, speedRpmBlocks, mode, onModeChange, isDark, view = 'chart', currentLapNum = null, windowSeconds = 30 }: Props) {
  const [compareLapNum, setCompareLapNum] = useState<number | null>(null)
  useEffect(() => {
    if (mode === 'compare' && compareLapNum === null && speedRpmBlocks?.length) setCompareLapNum(speedRpmBlocks[0].lapNum ?? null)
  }, [mode, speedRpmBlocks, compareLapNum])

  const clBlock = useMemo(() => speedRpmBlocks?.find(b => b.lapNum === currentLapNum) ?? null, [speedRpmBlocks, currentLapNum])
  const compareBlock = useMemo(() => speedRpmBlocks?.find(b => b.lapNum === compareLapNum) ?? null, [speedRpmBlocks, compareLapNum])
  const referenceBlock = useMemo((): LapBlock | null => {
    if (mode === 'compare') return compareBlock
    if (mode === 'FL') return fastestLap as unknown as LapBlock | null
    if (mode === 'PL') return (lapHistory[lapHistory.length - 1] as unknown as LapBlock) ?? null
    if (mode === 'CL') return clBlock
    return null
  }, [mode, compareBlock, fastestLap, lapHistory, clBlock])

  const overlay = mode === 'PL' || mode === 'FL' || mode === 'compare' || (mode === 'CL' && !!clBlock)
  const scrolling = mode === 'default'
  const currentRows = scrolling ? data : lapData
  const currentStatuses = scrolling ? statusHistory : lapStatusHistory
  const currentOrigin = scrolling ? 0 : (lapData[0]?.session_time ?? 0)
  const currentNormalized = useNormalizedSeries(currentRows, currentStatuses, currentOrigin, scrolling ? 'session' : `lap:${currentLapNum ?? currentOrigin}`)
  const referenceNormalized = useNormalizedSeries(
    referenceBlock?.telemetry ?? [], referenceBlock?.statusHistory ?? [], referenceBlock?.startSessionTime ?? 0,
    referenceBlock ? `ref:${mode}:${referenceBlock.lapNum ?? compareLapNum ?? referenceBlock.startSessionTime}` : 'none',
  )
  const seriesData = useMemo((): SpeedRpmSeriesSet => ({
    reference: referenceBlock ? referenceNormalized.series : EMPTY_TRIPLE,
    current: currentNormalized.series,
  }), [referenceBlock, referenceNormalized.version, currentNormalized.version])

  const revision = `${mode}:${compareLapNum ?? ''}:${referenceBlock?.lapNum ?? ''}:${currentLapNum ?? ''}`
  const compLabel = compareLapNum !== null ? `L${compareLapNum}` : 'CMP'
  const refLabel = mode === 'FL' ? 'FL' : mode === 'compare' ? compLabel : mode === 'CL' ? 'LAP' : 'PL'
  const tooltipFormat = useCallback((x: number, ref: number[], cur: number[]) => {
    const curVals = [raw(cur[0], 380), raw(cur[1], 16000), raw(cur[2], 100)]
    if (!overlay) return [
      `<div style="color:var(--text-secondary);margin-bottom:4px">${scrolling ? fmtTime(x) : fmtLapTime(x)}</div>`,
      `<div><span style="color:${COLOR_SPEED}">Speed</span>: ${curVals[0]} kph</div>`,
      `<div><span style="color:${COLOR_RPM}">RPM</span>: ${curVals[1].toLocaleString()}</div>`,
      `<div><span style="color:${COLOR_ERS}">ERS</span>: ${curVals[2]}%</div>`,
    ].join('')
    const rv = [raw(ref[0], 380), raw(ref[1], 16000), raw(ref[2], 100)]
    const showValue = (v: number, suffix = '') => Number.isNaN(v) ? '—' : `${v.toLocaleString()}${suffix}`
    return [
      `<div style="color:var(--text-secondary);margin-bottom:4px">${fmtLapTime(x)}</div>`,
      `<div style="color:var(--text-secondary);font-size:10px;margin-bottom:2px">${refLabel}</div>`,
      `<div><span style="color:${COLOR_SPEED_MUTED}">Speed</span>: ${showValue(rv[0])} &nbsp;<span style="color:${COLOR_RPM_MUTED}">RPM</span>: ${showValue(rv[1])} &nbsp;<span style="color:${COLOR_ERS_MUTED}">ERS</span>: ${showValue(rv[2], '%')}</div>`,
      `<div style="color:var(--text-secondary);font-size:10px;margin-top:4px;margin-bottom:2px">CURR</div>`,
      `<div><span style="color:${COLOR_SPEED}">Speed</span>: ${showValue(curVals[0])} &nbsp;<span style="color:${COLOR_RPM}">RPM</span>: ${showValue(curVals[1])} &nbsp;<span style="color:${COLOR_ERS}">ERS</span>: ${showValue(curVals[2], '%')}</div>`,
    ].join('')
  }, [overlay, scrolling, refLabel])

  const tableData = useMemo((): AlignedTable => {
    if (overlay || data.length === 0) return EMPTY_TABLE
    const source = mode === 'CL' ? lapData : data
    const statuses = mode === 'CL' ? lapStatusHistory : statusHistory
    const ts = new Float64Array(source.length), speed = new Float64Array(source.length), rpm = new Float64Array(source.length), ers = new Float64Array(source.length)
    let si = 0
    source.forEach((row, i) => {
      while (si + 1 < statuses.length && statuses[si + 1].session_time <= row.session_time) si++
      ts[i] = row.session_time; speed[i] = row.speed_kph; rpm[i] = row.rpm; ers[i] = statuses[si]?.ers_pct ?? 0
    })
    return [ts, speed, rpm, ers]
  }, [overlay, mode, data, lapData, statusHistory, lapStatusHistory])

  const showTable = view === 'table' && !overlay
  const noData = mode === 'compare' ? !compareBlock : mode === 'PL' ? lapHistory.length === 0 : mode === 'FL' ? fastestLap === null : (mode === 'CL' ? lapData.length === 0 : data.length === 0)
  const emptyMsg = mode === 'compare' ? 'Load a file to compare laps' : mode === 'FL' ? 'Complete a lap to record fastest' : mode === 'PL' ? 'Complete a lap to see comparison' : 'No data — start driving to see telemetry'
  const compareSelectStyles = useMemo(() => buildSelectStyles(isDark, { controlHeight: 20 }), [isDark])
  const lapOptions = useMemo(() => speedRpmBlocks?.map(b => ({ value: b.lapNum ?? 0, label: String(b.lapNum) })) ?? [], [speedRpmBlocks])
  const compareValue = compareLapNum !== null ? { value: compareLapNum, label: String(compareLapNum) } : null
  const handleCompareLapChange = useCallback((opt: SingleValue<LapOption>) => { if (opt) setCompareLapNum(opt.value) }, [])

  return <div className="bg-[var(--bg-panel)] p-4 flex flex-col h-full">
    <div className="flex items-center justify-between mb-3 shrink-0">
      <div className="flex items-center gap-3">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Speed + RPM + ERS</h2>
        {view !== 'table' && <div className="flex gap-1">
          {(['default', 'CL', 'PL', 'FL'] as const).map(m => <button key={m} onClick={() => onModeChange(m)} className={`px-2 py-0.5 text-xs transition-colors border-b-2 ${mode === m ? 'border-[var(--border-focus)] text-[var(--text-primary)]' : 'border-transparent text-[var(--text-secondary)] hover:text-[var(--text-primary)]'}`}>{m === 'default' ? 'Default' : m === 'CL' ? 'Current Lap' : m === 'PL' ? 'Previous Lap' : 'Fastest Lap'}</button>)}
          {!!speedRpmBlocks?.length && <button onClick={() => onModeChange('compare')} className={`px-2 py-0.5 text-xs transition-colors border-b-2 ${mode === 'compare' ? 'border-[var(--border-focus)] text-[var(--text-primary)]' : 'border-transparent text-[var(--text-secondary)] hover:text-[var(--text-primary)]'}`}>Compare Laps</button>}
        </div>}
      </div>
      {view !== 'table' && <div className="flex items-center gap-3">
        {mode === 'compare' && speedRpmBlocks && <div className="w-16 shrink-0"><Select<LapOption> value={compareValue} options={lapOptions} onChange={handleCompareLapChange} isSearchable={false} maxMenuHeight={150} styles={compareSelectStyles} components={selectComponents} placeholder="Lap…" /></div>}
        <div className="flex gap-4 text-xs">{overlay ? <>
          <span style={{ color: COLOR_SPEED_MUTED }}>— {refLabel} Speed</span><span style={{ color: COLOR_RPM_MUTED }}>— {refLabel} RPM</span><span style={{ color: COLOR_ERS_MUTED }}>— {refLabel} ERS</span>
          <span style={{ color: COLOR_SPEED }}>— Speed</span><span style={{ color: COLOR_RPM }}>— RPM</span><span style={{ color: COLOR_ERS }}>— ERS</span>
        </> : <><span style={{ color: COLOR_SPEED }}>— Speed (kph)</span><span style={{ color: COLOR_RPM }}>— RPM</span><span style={{ color: COLOR_ERS }}>— ERS (%)</span></>}</div>
      </div>}
    </div>
    <div className="flex-1 min-h-0 relative">
      {noData ? <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">{emptyMsg}</div>
        : showTable ? <GraphTable columns={TABLE_COLS} data={tableData} />
          : <SpeedRpmTimeChart key={isDark ? 'dark' : 'light'} isDark={isDark} data={seriesData} revision={revision} scrolling={scrolling} windowSeconds={windowSeconds} xTickFormat={scrolling ? fmtTime : fmtLapTime} tooltipFormat={tooltipFormat} />}
    </div>
  </div>
}
