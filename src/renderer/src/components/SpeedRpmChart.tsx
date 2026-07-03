import { useMemo, useRef, useCallback, useState, useEffect } from 'react'
import UPlotReact from 'uplot-react'
import uPlot from 'uplot'
import Select, { type SingleValue } from 'react-select'
import type { TelemetryRow, StatusRow, LapData } from '../types'
import { useSize } from '../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../hooks/useChartTooltip'
import { buildSelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'

type LapOption = { value: number; label: string }

interface Props {
  data: TelemetryRow[]
  statusHistory: StatusRow[]
  lapData: TelemetryRow[]
  lapStatusHistory: StatusRow[]
  lapHistory: LapData[]
  fastestLap: LapData | null
  speedRpmBlocks: any[] | null
  mode: 'default' | 'CL' | 'PL' | 'FL' | 'compare'
  onModeChange: (mode: 'default' | 'CL' | 'PL' | 'FL' | 'compare') => void
  isDark: boolean
}

const COLOR_SPEED = '#37872D'
const COLOR_RPM   = '#C4162A'
const COLOR_ERS   = '#FADE2A'
const COLOR_SPEED_MUTED = 'rgba(55, 135, 45, 0.35)'
const COLOR_RPM_MUTED   = 'rgba(196, 22, 42, 0.35)'
const COLOR_ERS_MUTED   = 'rgba(250, 222, 42, 0.35)'


function fmtTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return `${m}:${String(sec).padStart(2, '0')}`
}

function fmtLapTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = (s % 60).toFixed(1)
  return `${m}:${sec.padStart(4, '0')}`
}

function advance<T extends { session_time: number }>(arr: T[], idx: number, target: number): number {
  while (idx + 1 < arr.length) {
    const a = Math.abs(arr[idx].session_time - target)
    const b = Math.abs(arr[idx + 1].session_time - target)
    if (b <= a) idx++; else break
  }
  return idx
}

// Hold-last-value advance for the sparse 2 Hz status rows: the value switches when
// the next sample arrives (step-left), not at the midpoint between samples.
function advanceHold<T extends { session_time: number }>(arr: T[], idx: number, target: number): number {
  while (idx + 1 < arr.length && arr[idx + 1].session_time <= target) idx++
  return idx
}

function buildOverlayData(
  prevLap: { telemetry: any[]; statusHistory: any[]; startSessionTime: number; lapNum?: number },
  lapData: TelemetryRow[],
  lapStatusHistory: StatusRow[]
): uPlot.AlignedData {

  if (!prevLap || prevLap.telemetry.length === 0) {
    return [new Float64Array(), new Float64Array(), new Float64Array(), new Float64Array(),
            new Float64Array(), new Float64Array(), new Float64Array()]
  }
  const prevTel  = prevLap.telemetry
  const prevSts  = prevLap.statusHistory
  const curStart = lapData[0]?.session_time ?? 0
  const curEnd   = lapData.length > 0 ? lapData[lapData.length - 1].session_time : -Infinity
  const stsEnd   = lapStatusHistory.length > 0 ? lapStatusHistory[lapStatusHistory.length - 1].session_time : -Infinity

  const prevDuration = prevTel[prevTel.length - 1].session_time - prevLap.startSessionTime
  const curDuration  = lapData.length > 0 ? curEnd - curStart : 0


  let curExtendStart = lapData.length
  if (curDuration > prevDuration) {
    curExtendStart = 0
    while (curExtendStart < lapData.length &&
           lapData[curExtendStart].session_time - curStart <= prevDuration) {
      curExtendStart++
    }
  }
  const extraPoints = lapData.length - curExtendStart
  const n = prevTel.length + extraPoints

  const x       = new Float64Array(n)
  const prevSpd = new Float64Array(n)
  const prevRpm = new Float64Array(n)
  const prevErs = new Float64Array(n)
  const curSpd  = new Float64Array(n)
  const curRpm  = new Float64Array(n)
  const curErs  = new Float64Array(n)

  let ci = 0, siP = 0, siC = 0

  prevTel.forEach((d: any, i: number) => {
    const t = d.session_time - prevLap.startSessionTime
    x[i] = t
    prevSpd[i] = d.speed_kph
    prevRpm[i] = d.rpm
    siP = advanceHold(prevSts, siP, d.session_time)
    prevErs[i] = prevSts[siP]?.ers_pct ?? 0
    const target = curStart + t
    ci = advance(lapData, ci, target)
    curSpd[i] = target <= curEnd ? lapData[ci].speed_kph : NaN
    curRpm[i] = target <= curEnd ? lapData[ci].rpm        : NaN
    siC = advanceHold(lapStatusHistory, siC, target)
    curErs[i] = target <= stsEnd ? lapStatusHistory[siC].ers_pct : NaN
  })

  for (let j = 0; j < extraPoints; j++) {
    const i = prevTel.length + j
    const d = lapData[curExtendStart + j]
    x[i]       = d.session_time - curStart
    prevSpd[i] = NaN
    prevRpm[i] = NaN
    prevErs[i] = NaN
    curSpd[i]  = d.speed_kph
    curRpm[i]  = d.rpm
    siC = advanceHold(lapStatusHistory, siC, d.session_time)
    curErs[i]  = d.session_time <= stsEnd ? lapStatusHistory[siC].ers_pct : NaN
  }


  return [x, prevSpd, prevRpm, prevErs, curSpd, curRpm, curErs]
}

export default function SpeedRpmChart({ data, statusHistory, lapData, lapStatusHistory, lapHistory, fastestLap, speedRpmBlocks, mode, onModeChange, isDark }: Props) {
  const activeData = mode === 'CL' ? lapData   : data
  const activeSts  = mode === 'CL' ? lapStatusHistory : statusHistory

  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const mountedRef = useRef(false)
  const visible = width > 0 && height > 0
  if (visible) mountedRef.current = true

  const [compareLapNum, setCompareLapNum] = useState<number | null>(null)

  useEffect(() => {
    if (mode === 'compare' && compareLapNum === null && speedRpmBlocks && speedRpmBlocks.length > 0) {
      setCompareLapNum(speedRpmBlocks[0].lapNum)
    }

  }, [mode, speedRpmBlocks, compareLapNum])

  const compareSelectStyles = useMemo(() => buildSelectStyles(isDark, { controlHeight: 20 }), [isDark])
  const lapOptions = useMemo(
    () => speedRpmBlocks?.map(b => ({ value: b.lapNum, label: String(b.lapNum) })) ?? [],
    [speedRpmBlocks]
  )
  const compareValue = useMemo(
    () => compareLapNum !== null ? { value: compareLapNum, label: String(compareLapNum) } : null,
    [compareLapNum]
  )
  const handleCompareLapChange = useCallback((opt: SingleValue<LapOption>) => {
    if (opt) setCompareLapNum(opt.value)
  }, [])

  const is2L = mode === 'PL' || mode === 'FL' || mode === 'compare'

  const uData = useMemo((): uPlot.AlignedData => {
    if (mode === 'compare') {
      const compareBlock = speedRpmBlocks?.find(b => b.lapNum === compareLapNum) ?? null
      if (!compareBlock) {
        return [new Float64Array(), new Float64Array(), new Float64Array(), new Float64Array(),
                new Float64Array(), new Float64Array(), new Float64Array()]
      }
      return buildOverlayData(compareBlock, lapData, lapStatusHistory)
    }

    if (mode === 'PL' || mode === 'FL') {
      const prevLap = mode === 'FL' ? fastestLap : lapHistory[lapHistory.length - 1]
      if (!prevLap) {
        return [new Float64Array(), new Float64Array(), new Float64Array(), new Float64Array(),
                new Float64Array(), new Float64Array(), new Float64Array()]
      }
      return buildOverlayData(prevLap as any, lapData, lapStatusHistory)
    }

    if (activeData.length === 0) return [new Float64Array(), new Float64Array(), new Float64Array(), new Float64Array()]
    const ts  = new Float64Array(activeData.length)
    const spd = new Float64Array(activeData.length)
    const rpm = new Float64Array(activeData.length)
    const ers = new Float64Array(activeData.length)
    let si = 0
    activeData.forEach((d, i) => {
      ts[i]  = d.session_time
      spd[i] = d.speed_kph
      rpm[i] = d.rpm
      if (activeSts.length > 0) {
        si = advanceHold(activeSts, si, d.session_time)
        ers[i] = activeSts[si].ers_pct
      }
    })
    return [ts, spd, rpm, ers]
  }, [is2L ? lapHistory : activeData, is2L ? lapStatusHistory : activeSts, mode, lapData, fastestLap, compareLapNum, speedRpmBlocks])

  const compLabel = compareLapNum !== null ? `L${compareLapNum}` : 'CMP'

  const opts = useMemo((): uPlot.Options => {
    const ttPlugin: uPlot.Plugin = {
      hooks: {
        setCursor: (u) => {
          const idx = u.cursor.idx
          if (idx == null) { hide(); return }
          const ts = (u.data[0] as Float64Array)[idx]
          let html: string
          if (is2L) {
            const pSpd = (u.data[1] as Float64Array)[idx]
            const pRpm = (u.data[2] as Float64Array)[idx]
            const pErs = (u.data[3] as Float64Array)[idx]
            const cSpd = (u.data[4] as Float64Array)[idx]
            const cRpm = (u.data[5] as Float64Array)[idx]
            const cErs = (u.data[6] as Float64Array)[idx]
            const refLabel = mode === 'FL' ? 'FL' : mode === 'compare' ? compLabel : 'PL'
            html = [
              `<div style="color:var(--text-secondary);margin-bottom:4px">${fmtLapTime(ts)}</div>`,
              `<div style="color:var(--text-secondary);font-size:10px;margin-bottom:2px">${refLabel}</div>`,
              `<div><span style="color:${COLOR_SPEED_MUTED}">Speed</span>: ${pSpd} &nbsp;<span style="color:${COLOR_RPM_MUTED}">RPM</span>: ${pRpm?.toLocaleString()} &nbsp;<span style="color:${COLOR_ERS_MUTED}">ERS</span>: ${pErs}%</div>`,
              `<div style="color:var(--text-secondary);font-size:10px;margin-top:4px;margin-bottom:2px">CURR</div>`,
              `<div><span style="color:${COLOR_SPEED}">Speed</span>: ${isNaN(cSpd) ? '—' : cSpd} &nbsp;<span style="color:${COLOR_RPM}">RPM</span>: ${isNaN(cRpm) ? '—' : cRpm?.toLocaleString()} &nbsp;<span style="color:${COLOR_ERS}">ERS</span>: ${isNaN(cErs) ? '—' : cErs + '%'}</div>`,
            ].join('')
          } else {
            const spd = (u.data[1] as Float64Array)[idx]
            const rpm = (u.data[2] as Float64Array)[idx]
            const ers = (u.data[3] as Float64Array)[idx]
            html = [
              `<div style="color:var(--text-secondary);margin-bottom:4px">${fmtTime(ts)}</div>`,
              `<div><span style="color:${COLOR_SPEED}">Speed</span>: ${spd} kph</div>`,
              `<div><span style="color:${COLOR_RPM}">RPM</span>: ${rpm.toLocaleString()}</div>`,
              `<div><span style="color:${COLOR_ERS}">ERS</span>: ${ers}%</div>`,
            ].join('')
          }
          show(html, u.cursor.left ?? 0, u.cursor.top ?? 0, width, height)
        },
      },
    }

    const refLabel = mode === 'FL' ? 'FL' : mode === 'compare' ? compLabel : 'PL'
    const series: uPlot.Series[] = is2L ? [
      {},
      { label: `${refLabel} Speed`, stroke: COLOR_SPEED_MUTED, scale: 'spd', width: 1.5, points: { show: false } },
      { label: `${refLabel} RPM`,   stroke: COLOR_RPM_MUTED,   scale: 'rpm', width: 1.5, points: { show: false } },
      { label: `${refLabel} ERS`,   stroke: COLOR_ERS_MUTED,   scale: 'ers', width: 1.5, points: { show: false } },
      { label: 'Speed',             stroke: COLOR_SPEED,        scale: 'spd', width: 1.5, points: { show: false } },
      { label: 'RPM',               stroke: COLOR_RPM,          scale: 'rpm', width: 1.5, points: { show: false } },
      { label: 'ERS',               stroke: COLOR_ERS,          scale: 'ers', width: 1.5, points: { show: false } },
    ] : [
      {},
      { label: 'Speed', stroke: COLOR_SPEED, scale: 'spd', width: 1.5, points: { show: false } },
      { label: 'RPM',   stroke: COLOR_RPM,   scale: 'rpm', width: 1.5, points: { show: false } },
      { label: 'ERS',   stroke: COLOR_ERS,   scale: 'ers', width: 1.5, points: { show: false } },
    ]

    return {
      width,
      height,
      padding: [4, 0, 0, 0],
      legend: { show: false },
      cursor: { drag: { setScale: false } },
      scales: {
        x:   {},
        spd: { range: [0, 380] },
        rpm: { range: [0, 16000] },
        ers: { range: [0, 100] },
      },
      axes: [
        {
          stroke: isDark ? '#7c8098' : '#6b7280',
          font: '11px "Cascadia Code", ui-monospace, monospace',
          ticks: { show: false },
          grid:  { stroke: isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)', width: 1 },
          gap: 2,
          size: 22,
          values: (_u, splits) => splits.map(is2L ? fmtLapTime : fmtTime),
          space: 80,
        },
        {
          scale: 'spd',
          side: 3,
          stroke: COLOR_SPEED,
          font: '11px "Cascadia Code", ui-monospace, monospace',
          size: 40,
          ticks: { show: false },
          grid:  { stroke: isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)', width: 1 },
          gap: 4,
        },
        {
          scale: 'rpm',
          side: 1,
          stroke: COLOR_RPM,
          font: '11px "Cascadia Code", ui-monospace, monospace',
          size: 50,
          ticks: { show: false },
          grid:  { show: false },
          gap: 4,
          values: (_u, splits) => splits.map(v => v >= 1000 ? `${v / 1000}k` : String(v)),
        },
        {
          scale: 'ers',
          side: 1,
          stroke: COLOR_ERS,
          font: '11px "Cascadia Code", ui-monospace, monospace',
          size: 42,
          ticks: { show: false },
          grid:  { show: false },
          gap: 4,
          values: (_u, splits) => splits.map(v => `${v}%`),
        },
      ],
      series,
      plugins: [ttPlugin],
    }
  }, [width, height, mode, isDark, compLabel, is2L])

  const onCreate = useCallback((u: uPlot) => {
    u.over.addEventListener('mouseleave', hide)
  }, [])

  const noData = mode === 'compare'
    ? !speedRpmBlocks || speedRpmBlocks.length === 0 || compareLapNum === null
    : mode === 'PL'
    ? lapHistory.length === 0
    : mode === 'FL'
    ? fastestLap === null
    : activeData.length === 0

  const emptyMsg = mode === 'compare'
    ? 'Load a file to compare laps'
    : mode === 'FL'
    ? 'Complete a lap to record fastest'
    : mode === 'PL'
    ? 'Complete a lap to see comparison'
    : 'No data — start driving to see telemetry'

  const refLabel = mode === 'FL' ? 'FL' : mode === 'compare' ? compLabel : 'PL'

  return (
    <div className="bg-[var(--bg-panel)] p-4 flex flex-col h-full">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <div className="flex items-center gap-3">
          <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Speed + RPM + ERS</h2>
          <div className="flex gap-1">
            {(['default', 'CL', 'PL', 'FL'] as const).map(m => (
              <button
                key={m}
                onClick={() => onModeChange(m)}
                className={`px-2 py-0.5 text-xs transition-colors border-b-2 ${
                  mode === m
                    ? 'border-[var(--border-focus)] text-[var(--text-primary)]'
                    : 'border-transparent text-[var(--text-secondary)] hover:text-[var(--text-primary)]'
                }`}
              >
                {m === 'default' ? 'Default' : m === 'CL' ? 'Current Lap' : m === 'PL' ? 'Previous Lap' : 'Fastest Lap'}
              </button>
            ))}
            {speedRpmBlocks && speedRpmBlocks.length > 0 && (
              <button
                onClick={() => onModeChange('compare')}
                className={`px-2 py-0.5 text-xs transition-colors border-b-2 ${
                  mode === 'compare'
                    ? 'border-[var(--border-focus)] text-[var(--text-primary)]'
                    : 'border-transparent text-[var(--text-secondary)] hover:text-[var(--text-primary)]'
                }`}
              >
                Compare Laps
              </button>
            )}
          </div>
        </div>
        <div className="flex items-center gap-3">
          {mode === 'compare' && speedRpmBlocks && (
            <div className="w-16 shrink-0">
              <Select<LapOption>
                value={compareValue}
                options={lapOptions}
                onChange={handleCompareLapChange}
                isSearchable={false}
                maxMenuHeight={150}
                styles={compareSelectStyles}
                components={selectComponents}
                placeholder="Lap…"
              />
            </div>
          )}
          <div className="flex gap-4 text-xs">
            {is2L ? (
              <>
                <span style={{ color: COLOR_SPEED_MUTED }}>— {refLabel} Speed</span>
                <span style={{ color: COLOR_RPM_MUTED }}>— {refLabel} RPM</span>
                <span style={{ color: COLOR_ERS_MUTED }}>— {refLabel} ERS</span>
                <span style={{ color: COLOR_SPEED }}>— Speed</span>
                <span style={{ color: COLOR_RPM }}>— RPM</span>
                <span style={{ color: COLOR_ERS }}>— ERS</span>
              </>
            ) : (
              <>
                <span style={{ color: COLOR_SPEED }}>— Speed (kph)</span>
                <span style={{ color: COLOR_RPM }}>— RPM</span>
                <span style={{ color: COLOR_ERS }}>— ERS (%)</span>
              </>
            )}
          </div>
        </div>
      </div>

      <div className="flex-1 min-h-0 relative" ref={sizeRef}>
        {noData ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">
            {emptyMsg}
          </div>
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
