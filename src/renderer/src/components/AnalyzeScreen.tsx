import { memo, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState, type CSSProperties } from 'react'
import { createPortal } from 'react-dom'
import Select, { type SingleValue } from 'react-select'
import { Chrome, ChromeInputType } from '@uiw/react-color'
import { ChevronLeft, ChevronRight, GripVertical, PanelLeftClose, PanelLeftOpen, RotateCcw, Trash2 } from 'lucide-react'
import { useAppConfig } from '../hooks/useAppConfig'
import {
  ANALYZE_METRICS, ANALYZE_METRIC_BY_ID, DEFAULT_ANALYZE_CONFIG, sanitizeAnalyzeConfig,
  type AnalyzeConfig, type AnalyzeSeriesConfig,
} from '../lib/analyzeMetrics'
import { buildSelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import { useTelemetryStore } from '../stores/telemetryStore'
import type { AnalyzeLapData } from '../types'
import AnalyzeTimeChart from './charts/AnalyzeTimeChart'

interface Props {
  isDark: boolean
  playbackFilename: string | null
  currentLapNum: number | null
  compareLapNum: number | null
  onCompareLapChange: (lapNum: number | null) => void
}

interface LapBlock { lapNum: number; startSessionTime: number; endSessionTime: number }
interface SelectOption { value: string; label: string }

function AnalyzeColorPicker({ label, color, onChange }: { label: string; color: string; onChange: (color: string) => void }) {
  const [open, setOpen] = useState(false)
  const [position, setPosition] = useState({ left: 8, top: 8 })
  const buttonRef = useRef<HTMLButtonElement>(null)
  const pickerRef = useRef<HTMLDivElement>(null)

  useLayoutEffect(() => {
    if (!open) return
    const place = () => {
      const rect = buttonRef.current?.getBoundingClientRect()
      if (!rect) return
      const pickerWidth = 230
      const pickerHeight = 260
      const left = Math.max(8, Math.min(rect.left, window.innerWidth - pickerWidth - 8))
      const below = rect.bottom + 6
      const top = below + pickerHeight <= window.innerHeight
        ? below
        : Math.max(8, rect.top - pickerHeight - 6)
      setPosition({ left, top })
    }
    place()
    window.addEventListener('resize', place)
    return () => window.removeEventListener('resize', place)
  }, [open])

  useEffect(() => {
    if (!open) return
    const closeOutside = (event: PointerEvent) => {
      const target = event.target as Node
      if (!buttonRef.current?.contains(target) && !pickerRef.current?.contains(target)) setOpen(false)
    }
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setOpen(false)
    }
    document.addEventListener('pointerdown', closeOutside)
    document.addEventListener('keydown', closeOnEscape)
    return () => {
      document.removeEventListener('pointerdown', closeOutside)
      document.removeEventListener('keydown', closeOnEscape)
    }
  }, [open])

  const chromeStyle = {
    '--github-background-color': 'var(--bg-menu)',
    '--github-border': '1px solid var(--border)',
    '--github-box-shadow': '0 14px 36px rgba(0, 0, 0, 0.38)',
    '--github-arrow-border-color': 'var(--border)',
    '--editable-input-label-color': 'var(--text-secondary)',
    '--editable-input-box-shadow': 'var(--border) 0 0 0 1px inset',
    '--editable-input-color': 'var(--text-primary)',
    '--chrome-arrow-fill': 'var(--text-secondary)',
    '--chrome-arrow-background-color': 'var(--bg-hover)',
    width: 230,
    borderRadius: 6,
    fontFamily: '"Cascadia Code", ui-monospace, monospace',
  } as CSSProperties

  return <>
    <button
      ref={buttonRef}
      type="button"
      draggable={false}
      aria-label={`${label} color`}
      aria-haspopup="dialog"
      aria-expanded={open}
      onClick={() => setOpen(value => !value)}
      className="w-5 h-5 rounded border border-[var(--border)] cursor-pointer shrink-0 shadow-inner"
      style={{ backgroundColor: color }}
    />
    {open && createPortal(
      <div ref={pickerRef} role="dialog" aria-label={`${label} color picker`} className="fixed z-[10000]" style={position}>
        <Chrome
          color={color}
          inputType={ChromeInputType.HEXA}
          showAlpha={false}
          showTriangle={false}
          style={chromeStyle}
          onChange={result => onChange(result.hex)}
        />
      </div>,
      document.body,
    )}
  </>
}

const AnalyzeChartSubscriber = memo(function AnalyzeChartSubscriber({
  isDark, selected, showYAxis, currentLapNum, comparison,
}: {
  isDark: boolean
  selected: AnalyzeSeriesConfig[]
  showYAxis: boolean
  currentLapNum: number | null
  comparison: AnalyzeLapData | null
}) {
  const telemetry = useTelemetryStore(s => s.analyzeLapTelemetry)
  const motion = useTelemetryStore(s => s.analyzeLapMotion)
  const motionEx = useTelemetryStore(s => s.analyzeLapMotionEx)
  const statusHistory = useTelemetryStore(s => s.analyzeLapStatusHistory)
  const damageHistory = useTelemetryStore(s => s.analyzeLapDamageHistory)
  const startSessionTime = useTelemetryStore(s => s.analyzeLapStartTime)
  const currentRevision = useTelemetryStore(s => s.analyzeLapRevision)

  const current = useMemo<AnalyzeLapData>(() => {
    const ends = [telemetry, motion, motionEx, statusHistory, damageHistory]
      .flatMap(rows => rows.length ? [rows[rows.length - 1].session_time] : [])
    return {
      lapNum: currentLapNum ?? 0,
      startSessionTime,
      endSessionTime: ends.length ? Math.max(...ends) : startSessionTime,
      telemetry, motion, motionEx, statusHistory, damageHistory,
    }
  }, [currentLapNum, damageHistory, motion, motionEx, startSessionTime, statusHistory, telemetry])

  return <div className="absolute inset-0">
    <AnalyzeTimeChart isDark={isDark} current={current} currentRevision={currentRevision} comparison={comparison} selected={selected} showYAxis={showYAxis} />
    {selected.length === 0 && <div className="absolute inset-0 flex items-center justify-center text-sm text-[var(--text-secondary)] pointer-events-none">Add a metric from the panel to begin analyzing</div>}
    {selected.length > 0 && telemetry.length === 0 && <div className="absolute inset-0 flex items-center justify-center text-sm text-[var(--text-secondary)] pointer-events-none">No lap data — start driving or load a recording</div>}
  </div>
})

export default function AnalyzeScreen({ isDark, playbackFilename, currentLapNum, compareLapNum, onCompareLapChange }: Props) {
  const [rawConfig, setRawConfig] = useAppConfig<AnalyzeConfig>('analyze', DEFAULT_ANALYZE_CONFIG)
  const config = useMemo(() => sanitizeAnalyzeConfig(rawConfig), [rawConfig])
  const blocks = useTelemetryStore(s => s.speedRpmBlocks) as LapBlock[] | null
  const lapCache = useTelemetryStore(s => s.playbackLapDataCache)
  const liveLapNum = useTelemetryStore(s => s.lap?.lap_num ?? null)
  const effectiveCurrentLapNum = currentLapNum ?? liveLapNum
  const [draggedMetric, setDraggedMetric] = useState<string | null>(null)
  const requestedRef = useRef<number | null>(null)
  const selectStyles = useMemo(() => buildSelectStyles(isDark, { solidBg: true, controlHeight: 32 }), [isDark])

  const save = useCallback((next: AnalyzeConfig) => setRawConfig(next), [setRawConfig])
  const updateSeries = useCallback((series: AnalyzeSeriesConfig[]) => save({ ...config, series }), [config, save])

  const selectedIds = useMemo(() => new Set(config.series.map(item => item.metricId)), [config.series])
  const metricOptions = useMemo(() => ['Driving', 'Motion', 'Power', 'Tyres'].map(group => ({
    label: group,
    options: ANALYZE_METRICS.filter(metric => metric.group === group && !selectedIds.has(metric.id)).map(metric => ({ value: metric.id, label: metric.label })),
  })).filter(group => group.options.length), [selectedIds])

  const compareOptions = useMemo(() => [
    { value: 0, label: 'None' },
    ...(blocks ?? []).map(block => ({ value: block.lapNum, label: `Lap ${block.lapNum}` })),
  ], [blocks])
  const compareValue = compareOptions.find(option => option.value === (compareLapNum ?? 0)) ?? compareOptions[0]
  const comparison = compareLapNum !== null ? lapCache[compareLapNum] ?? null : null

  useEffect(() => {
    if (!playbackFilename || compareLapNum === null || lapCache[compareLapNum]) {
      requestedRef.current = null
      return
    }
    if (requestedRef.current === compareLapNum) return
    requestedRef.current = compareLapNum
    window.playerBridge.getLapData(compareLapNum)
  }, [compareLapNum, lapCache, playbackFilename])

  const addMetric = useCallback((option: SingleValue<SelectOption>) => {
    if (!option) return
    const def = ANALYZE_METRIC_BY_ID.get(option.value)
    if (!def || config.series.some(item => item.metricId === def.id)) return
    updateSeries([...config.series, { metricId: def.id, color: def.defaultColor }])
  }, [config.series, updateSeries])

  const moveMetric = useCallback((metricId: string, delta: number) => {
    const from = config.series.findIndex(item => item.metricId === metricId)
    const to = from + delta
    if (from < 0 || to < 0 || to >= config.series.length) return
    const next = [...config.series]
    const [item] = next.splice(from, 1)
    next.splice(to, 0, item)
    updateSeries(next)
  }, [config.series, updateSeries])

  const dropMetric = useCallback((targetId: string) => {
    if (!draggedMetric || draggedMetric === targetId) return
    const from = config.series.findIndex(item => item.metricId === draggedMetric)
    const to = config.series.findIndex(item => item.metricId === targetId)
    if (from < 0 || to < 0) return
    const next = [...config.series]
    const [item] = next.splice(from, 1)
    next.splice(to, 0, item)
    updateSeries(next)
    setDraggedMetric(null)
  }, [config.series, draggedMetric, updateSeries])

  return (
    <div className="h-full flex overflow-hidden border-t border-[var(--border)] bg-[var(--bg-panel)]">
      <aside className={`${config.collapsed ? 'w-0 border-r-0' : 'w-72 border-r'} shrink-0 border-[var(--border)] overflow-hidden bg-[var(--bg-panel)] transition-[width] duration-200 ease-out`}>
          <div className={`w-72 h-full flex flex-col transition-[visibility] duration-0 ${config.collapsed ? 'invisible delay-200' : 'visible delay-0'}`}>
            <div className="h-11 px-3 flex items-center border-b border-[var(--border)] shrink-0">
              <div>
                <div className="text-[11px] font-bold uppercase tracking-widest text-[var(--text-primary)]">Analyze</div>
                <div className="text-[9px] uppercase tracking-wider text-[var(--text-secondary)]">Overlay configuration</div>
              </div>
            </div>

            <div className="p-3 border-b border-[var(--border)] space-y-3 shrink-0">
              <div>
                <label className="block text-[9px] uppercase tracking-widest text-[var(--text-secondary)] mb-1.5">Add metric</label>
                <Select<SelectOption, false>
                  value={null} options={metricOptions} onChange={addMetric} placeholder="Choose a value…"
                  styles={selectStyles} components={selectComponents} isSearchable menuPortalTarget={document.body}
                />
              </div>
              <div>
                <label className="block text-[9px] uppercase tracking-widest text-[var(--text-secondary)] mb-1.5">Compare lap</label>
                <Select
                  value={compareValue} options={compareOptions}
                  onChange={option => onCompareLapChange(option && option.value !== 0 ? option.value : null)}
                  styles={selectStyles} components={selectComponents} isSearchable={false}
                  isDisabled={!playbackFilename || !blocks} menuPortalTarget={document.body}
                />
                {!playbackFilename && <div className="text-[9px] text-[var(--text-secondary)] mt-1">Load a recording to compare laps</div>}
                {playbackFilename && compareLapNum !== null && !comparison && <div className="text-[9px] text-[var(--text-secondary)] mt-1">Loading comparison lap…</div>}
              </div>
              <button
                role="switch" aria-checked={config.showYAxis}
                onClick={() => save({ ...config, showYAxis: !config.showYAxis })}
                className="w-full flex items-center justify-between text-[10px] uppercase tracking-wider text-[var(--text-secondary)]"
              >
                <span>Y-axis values</span>
                <span className={`w-8 h-4 rounded-full p-0.5 transition-colors ${config.showYAxis ? 'bg-[var(--border-focus)]' : 'bg-[var(--border)]'}`}><span className={`block w-3 h-3 rounded-full bg-white transition-transform ${config.showYAxis ? 'translate-x-4' : ''}`} /></span>
              </button>
            </div>

            <div className="flex-1 min-h-0 overflow-y-auto p-2 space-y-1">
              {config.series.length === 0 && <div className="p-4 text-center text-[10px] text-[var(--text-secondary)]">No metrics selected</div>}
              {config.series.map((item, index) => {
                const def = ANALYZE_METRIC_BY_ID.get(item.metricId)
                if (!def) return null
                return (
                  <div
                    key={item.metricId} draggable onDragStart={() => setDraggedMetric(item.metricId)} onDragEnd={() => setDraggedMetric(null)}
                    onDragOver={event => event.preventDefault()} onDrop={() => dropMetric(item.metricId)}
                    className={`flex items-center gap-1.5 px-1.5 py-1.5 rounded border border-transparent hover:border-[var(--border)] hover:bg-[var(--bg-hover)] ${draggedMetric === item.metricId ? 'opacity-40' : ''}`}
                  >
                    <GripVertical size={13} className="text-[var(--text-secondary)] cursor-grab shrink-0" />
                    <AnalyzeColorPicker
                      label={def.label}
                      color={item.color}
                      onChange={color => updateSeries(config.series.map(entry => entry.metricId === item.metricId ? { ...entry, color } : entry))}
                    />
                    <div className="flex-1 min-w-0">
                      <div className="text-[10px] text-[var(--text-primary)] truncate">{def.label}</div>
                      <div className="text-[8px] uppercase tracking-wider text-[var(--text-secondary)]">{def.group}{def.unit ? ` · ${def.unit}` : ''}</div>
                    </div>
                    <div className="flex items-center shrink-0">
                      <button disabled={index === 0} title="Move up" onClick={() => moveMetric(item.metricId, -1)} className="p-1 text-[var(--text-secondary)] hover:text-[var(--text-primary)] disabled:opacity-20"><ChevronLeft size={12} className="rotate-90" /></button>
                      <button disabled={index === config.series.length - 1} title="Move down" onClick={() => moveMetric(item.metricId, 1)} className="p-1 text-[var(--text-secondary)] hover:text-[var(--text-primary)] disabled:opacity-20"><ChevronRight size={12} className="rotate-90" /></button>
                      <button title="Reset color" onClick={() => updateSeries(config.series.map(entry => entry.metricId === item.metricId ? { ...entry, color: def.defaultColor } : entry))} className="p-1 text-[var(--text-secondary)] hover:text-[var(--text-primary)]"><RotateCcw size={11} /></button>
                      <button title="Remove metric" onClick={() => updateSeries(config.series.filter(entry => entry.metricId !== item.metricId))} className="p-1 text-[var(--text-secondary)] hover:text-[#e10600]"><Trash2 size={11} /></button>
                    </div>
                  </div>
                )
              })}
            </div>
          </div>
      </aside>

      <section className="flex-1 min-w-0 flex flex-col">
        <div className="h-11 px-2 border-b border-[var(--border)] flex items-center gap-x-3 shrink-0 overflow-hidden">
          <button
            title={config.collapsed ? 'Open Analyze controls' : 'Collapse Analyze controls'}
            aria-label={config.collapsed ? 'Open Analyze controls' : 'Collapse Analyze controls'}
            onClick={() => save({ ...config, collapsed: !config.collapsed })}
            className="w-7 h-7 rounded flex items-center justify-center shrink-0 text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]"
          >
            {config.collapsed ? <PanelLeftOpen size={15} /> : <PanelLeftClose size={15} />}
          </button>
          <span className="text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">Current {effectiveCurrentLapNum ? `L${effectiveCurrentLapNum}` : 'lap'}</span>
          {config.series.map(item => {
            const def = ANALYZE_METRIC_BY_ID.get(item.metricId)
            return def ? <span key={item.metricId} className="text-[10px] whitespace-nowrap" style={{ color: item.color }}>— {def.label}</span> : null
          })}
          {compareLapNum !== null && <span className="ml-auto text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">Muted lines · Compare L{compareLapNum}</span>}
        </div>
        <div className="flex-1 min-h-0 relative">
          <AnalyzeChartSubscriber isDark={isDark} selected={config.series} showYAxis={config.showYAxis} currentLapNum={effectiveCurrentLapNum} comparison={comparison} />
        </div>
      </section>
    </div>
  )
}
