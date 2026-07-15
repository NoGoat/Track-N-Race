import { memo, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState, type CSSProperties, type MutableRefObject } from 'react'
import { createPortal } from 'react-dom'
import Select, { type SingleValue } from 'react-select'
import { Chrome, ChromeInputType } from '@uiw/react-color'
import { ArrowDownUp, ArrowLeft, ArrowRight, ChevronLeft, ChevronRight, Eye, EyeOff, GripVertical, PanelLeftClose, PanelLeftOpen, RotateCcw, Trash2, ZoomIn, ZoomOut } from 'lucide-react'
import { useAppConfig } from '../hooks/useAppConfig'
import {
  ANALYZE_METRICS, ANALYZE_METRIC_BY_ID, DEFAULT_ANALYZE_CONFIG, sanitizeAnalyzeConfig,
  type AnalyzeConfig, type AnalyzeSeriesConfig,
} from '../lib/analyzeMetrics'
import { buildSelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import { useLabels } from '../lib/labels'
import { useTelemetryStore } from '../stores/telemetryStore'
import type { AnalyzeLapData } from '../types'
import AnalyzeTimeChart, { type AnalyzeChartControls } from './charts/AnalyzeTimeChart'

interface Props {
  isDark: boolean
  playbackFilename: string | null
  currentLapNum: number | null
  compareLapNum: number | null
  onCompareLapChange: (lapNum: number | null) => void
  fixedLapMode: AnalyzeFixedLapMode
  onFixedLapModeChange: (mode: AnalyzeFixedLapMode) => void
}

export interface AnalyzeFixedLapMode {
  enabled: boolean
  lapA: number | null
  lapB: number | null
}

interface LapBlock {
  lapNum: number
  startSessionTime: number
  endSessionTime: number
  statusHistory: Array<{ tyre_compound: number; visual_compound: number }>
}
interface SelectOption { value: string; label: string }
interface LapOption { value: number; label: string; compound: string | null; compoundColor: string | null; isFastest: boolean }

const COMPOUND_COLORS: Record<number, string> = {
  16: 'var(--compound-soft)',
  17: 'var(--compound-medium)',
  18: 'var(--compound-hard)',
  7: 'var(--compound-inter)',
  8: 'var(--compound-wet)',
}

function lastTyreStatus(block: LapBlock): LapBlock['statusHistory'][number] | null {
  for (let index = block.statusHistory.length - 1; index >= 0; index--) {
    if (block.statusHistory[index].tyre_compound > 0) return block.statusHistory[index]
  }
  return null
}

function FastestLapChip() {
  return <span
    className="ml-2 text-[10px] font-medium uppercase tracking-wide rounded px-2 py-0.5 select-none shrink-0"
    style={{ backgroundColor: '#BF5FFF22', color: '#BF5FFF' }}
  >FL</span>
}

function formatLapOption(option: LapOption) {
  if (!option.compound) return <span className="inline-flex items-center min-w-0">{option.value === 0 ? 'None' : `Lap ${option.value}`}{option.isFastest && <FastestLapChip />}</span>
  return <span className="inline-flex items-center min-w-0">
    <span style={{ color: option.compoundColor ?? 'var(--text-primary)' }}>{option.compound}</span>
    <span>&nbsp;· Lap {option.value}</span>
    {option.isFastest && <FastestLapChip />}
  </span>
}

function AnalyzeLapSelector({
  id, label, value, options, placeholder, onChange, styles, isClearable = true, isDisabled = false,
}: {
  id: string
  label: string
  value: LapOption | null
  options: LapOption[]
  placeholder: string
  onChange: (option: SingleValue<LapOption>) => void
  styles: ReturnType<typeof buildSelectStyles>
  isClearable?: boolean
  isDisabled?: boolean
}) {
  return <div className="flex items-center gap-3">
    <label htmlFor={id} className="w-20 shrink-0 text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">{label}</label>
    <div className="flex-1 min-w-0">
      <Select<LapOption, false>
        inputId={id} value={value} options={options} placeholder={placeholder} onChange={onChange}
        formatOptionLabel={formatLapOption} styles={styles} components={selectComponents}
        isSearchable={false} isClearable={isClearable} isDisabled={isDisabled} menuPortalTarget={document.body}
      />
    </div>
  </div>
}

function AnalyzeColorPicker({ label, color, onChange }: { label: string; color: string; onChange: (color: string) => void }) {
  const [open, setOpen] = useState(false)
  const [position, setPosition] = useState({ left: 8, top: 8 })
  const [formatIconHost, setFormatIconHost] = useState<HTMLElement | null>(null)
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

  useLayoutEffect(() => {
    if (!open) {
      setFormatIconHost(null)
      return
    }
    const nativeIcon = pickerRef.current?.querySelector<SVGElement>('svg[viewBox="0 0 1024 1024"]')
    setFormatIconHost(nativeIcon?.parentElement ?? null)
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
          className="analyze-color-picker"
          color={color}
          inputType={ChromeInputType.HEXA}
          showAlpha={false}
          showTriangle={false}
          style={chromeStyle}
          onChange={result => onChange(result.hex)}
        />
        {formatIconHost?.isConnected && createPortal(
          <span className="w-8 h-8 flex items-center justify-center pointer-events-none text-[var(--text-secondary)]">
            <ArrowDownUp size={16} strokeWidth={1.75} className="analyze-color-format-icon" />
          </span>,
          formatIconHost,
        )}
      </div>,
      document.body,
    )}
  </>
}

const AnalyzeChartSubscriber = memo(function AnalyzeChartSubscriber({
  isDark, selected, showYAxis, currentLapNum, comparison, fixedMode, primaryOverride, controlsRef,
}: {
  isDark: boolean
  selected: AnalyzeSeriesConfig[]
  showYAxis: boolean
  currentLapNum: number | null
  comparison: AnalyzeLapData | null
  fixedMode: boolean
  primaryOverride: AnalyzeLapData | null
  controlsRef: MutableRefObject<AnalyzeChartControls | null>
}) {
  const telemetry = useTelemetryStore(s => fixedMode ? EMPTY_ROWS : s.analyzeLapTelemetry)
  const motion = useTelemetryStore(s => fixedMode ? EMPTY_ROWS : s.analyzeLapMotion)
  const motionEx = useTelemetryStore(s => fixedMode ? EMPTY_ROWS : s.analyzeLapMotionEx)
  const statusHistory = useTelemetryStore(s => fixedMode ? EMPTY_ROWS : s.analyzeLapStatusHistory)
  const damageHistory = useTelemetryStore(s => fixedMode ? EMPTY_ROWS : s.analyzeLapDamageHistory)
  const startSessionTime = useTelemetryStore(s => fixedMode ? 0 : s.analyzeLapStartTime)
  const liveRevision = useTelemetryStore(s => fixedMode ? 0 : s.analyzeLapRevision)

  const liveCurrent = useMemo<AnalyzeLapData>(() => {
    const ends = [telemetry, motion, motionEx, statusHistory, damageHistory]
      .flatMap(rows => rows.length ? [rows[rows.length - 1].session_time] : [])
    return {
      lapNum: currentLapNum ?? 0,
      startSessionTime,
      endSessionTime: ends.length ? Math.max(...ends) : startSessionTime,
      telemetry, motion, motionEx, statusHistory, damageHistory,
    }
  }, [currentLapNum, damageHistory, motion, motionEx, startSessionTime, statusHistory, telemetry])
  const current = fixedMode ? primaryOverride ?? EMPTY_ANALYZE_LAP : liveCurrent
  const currentRevision = fixedMode
    ? `fixed:${current.lapNum}:${current.startSessionTime}:${current.endSessionTime}`
    : liveRevision

  return <div className="absolute inset-0">
    <AnalyzeTimeChart
      isDark={isDark} current={current} currentRevision={currentRevision} comparison={comparison}
      selected={selected} showYAxis={showYAxis}
      primaryLabel={fixedMode ? `LAP A · L${current.lapNum || '—'}` : undefined}
      comparisonLabel={fixedMode && comparison ? `LAP B · L${comparison.lapNum}` : undefined}
      zoomEnabled={fixedMode && primaryOverride !== null} controlsRef={controlsRef}
    />
  </div>
})

const EMPTY_ROWS: never[] = []
const EMPTY_ANALYZE_LAP: AnalyzeLapData = {
  lapNum: 0, startSessionTime: 0, endSessionTime: 0,
  telemetry: [], motion: [], motionEx: [], statusHistory: [], damageHistory: [],
}

export default function AnalyzeScreen({
  isDark, playbackFilename, currentLapNum, compareLapNum, onCompareLapChange,
  fixedLapMode, onFixedLapModeChange,
}: Props) {
  const [rawConfig, setRawConfig] = useAppConfig<AnalyzeConfig>('analyze', DEFAULT_ANALYZE_CONFIG)
  const config = useMemo(() => sanitizeAnalyzeConfig(rawConfig), [rawConfig])
  const blocks = useTelemetryStore(s => s.speedRpmBlocks) as LapBlock[] | null
  const lapCache = useTelemetryStore(s => s.playbackLapDataCache)
  const liveLapNum = useTelemetryStore(s => s.lap?.lap_num ?? null)
  const fastestLapNum = useTelemetryStore(s => s.fastestLap?.lapNum ?? null)
  const { tn } = useLabels()
  const effectiveCurrentLapNum = currentLapNum ?? liveLapNum
  const [draggedMetric, setDraggedMetric] = useState<string | null>(null)
  const requestedRef = useRef(new Set<number>())
  const chartControlsRef = useRef<AnalyzeChartControls | null>(null)
  const selectStyles = useMemo(() => buildSelectStyles(isDark, { solidBg: true, controlHeight: 32 }), [isDark])

  const save = useCallback((next: AnalyzeConfig) => setRawConfig(next), [setRawConfig])
  const updateSeries = useCallback((series: AnalyzeSeriesConfig[]) => save({ ...config, series }), [config, save])

  const selectedIds = useMemo(() => new Set(config.series.map(item => item.metricId)), [config.series])
  const metricOptions = useMemo(() => ['Driving', 'Motion', 'Power', 'Tyres'].map(group => ({
    label: group,
    options: ANALYZE_METRICS.filter(metric => metric.group === group && !selectedIds.has(metric.id)).map(metric => ({ value: metric.id, label: metric.label })),
  })).filter(group => group.options.length), [selectedIds])

  const lapOption = useCallback((block: LapBlock): LapOption => {
    const status = lastTyreStatus(block)
    const isFastest = block.lapNum === fastestLapNum
    if (!status) return { value: block.lapNum, label: `Lap ${block.lapNum}${isFastest ? ' · FL' : ''}`, compound: null, compoundColor: null, isFastest }
    const actual = tn('tyre.actual', status.tyre_compound)
    return {
      value: block.lapNum,
      label: `${actual} · Lap ${block.lapNum}${isFastest ? ' · FL' : ''}`,
      compound: actual,
      compoundColor: COMPOUND_COLORS[status.visual_compound] ?? null,
      isFastest,
    }
  }, [fastestLapNum, tn])

  const compareOptions = useMemo<LapOption[]>(() => [
    { value: 0, label: 'None', compound: null, compoundColor: null, isFastest: false },
    ...(blocks ?? []).map(lapOption),
  ], [blocks, lapOption])
  const compareValue = compareOptions.find(option => option.value === (compareLapNum ?? 0)) ?? compareOptions[0]
  const lapOptions = useMemo<LapOption[]>(() => (blocks ?? []).map(lapOption), [blocks, lapOption])
  const lapAValue = lapOptions.find(option => option.value === fixedLapMode.lapA) ?? null
  const lapBValue = lapOptions.find(option => option.value === fixedLapMode.lapB) ?? null
  const fixedPrimary = fixedLapMode.lapA !== null ? lapCache[fixedLapMode.lapA] ?? null : null
  const fixedComparison = fixedLapMode.lapB !== null ? lapCache[fixedLapMode.lapB] ?? null : null
  const comparison = fixedLapMode.enabled
    ? fixedComparison
    : compareLapNum !== null ? lapCache[compareLapNum] ?? null : null

  useEffect(() => {
    requestedRef.current.clear()
  }, [playbackFilename])

  useEffect(() => {
    if (!playbackFilename) return
    const targets = fixedLapMode.enabled
      ? [fixedLapMode.lapA, fixedLapMode.lapB]
      : [compareLapNum]
    for (const lapNum of targets) {
      if (lapNum === null) continue
      if (lapCache[lapNum]) {
        requestedRef.current.delete(lapNum)
        continue
      }
      if (requestedRef.current.has(lapNum)) continue
      requestedRef.current.add(lapNum)
      window.playerBridge.getLapData(lapNum)
    }
  }, [compareLapNum, fixedLapMode.enabled, fixedLapMode.lapA, fixedLapMode.lapB, lapCache, playbackFilename])

  const addMetric = useCallback((option: SingleValue<SelectOption>) => {
    if (!option) return
    const def = ANALYZE_METRIC_BY_ID.get(option.value)
    if (!def || config.series.some(item => item.metricId === def.id)) return
    updateSeries([...config.series, { metricId: def.id, color: def.defaultColor, visible: true }])
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
              <div className="flex items-center gap-3">
                <label htmlFor="analyze-add-metric" className="w-20 shrink-0 text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">Add metric</label>
                <div className="flex-1 min-w-0">
                  <Select<SelectOption, false>
                    inputId="analyze-add-metric" value={null} options={metricOptions} onChange={addMetric} placeholder="Choose a value…"
                    styles={selectStyles} components={selectComponents} isSearchable menuPortalTarget={document.body}
                  />
                </div>
              </div>
              <div>
                <button
                  role="switch" aria-checked={fixedLapMode.enabled} disabled={!playbackFilename || !blocks}
                  onClick={() => onFixedLapModeChange({ ...fixedLapMode, enabled: !fixedLapMode.enabled })}
                  className="w-full flex items-center justify-between text-[10px] uppercase tracking-wider text-[var(--text-secondary)] disabled:opacity-40 disabled:cursor-not-allowed"
                >
                  <span>Disable playback mode</span>
                  <span className={`w-8 h-4 rounded-full p-0.5 transition-colors ${fixedLapMode.enabled ? 'bg-[var(--border-focus)]' : 'bg-[var(--border)]'}`}><span className={`block w-3 h-3 rounded-full bg-white transition-transform ${fixedLapMode.enabled ? 'translate-x-4' : ''}`} /></span>
                </button>
                {!playbackFilename && <div className="text-[9px] text-[var(--text-secondary)] mt-1">Load a recording to select fixed laps</div>}
              </div>
              {fixedLapMode.enabled ? (
                <div className="space-y-2">
                  <AnalyzeLapSelector
                    id="analyze-lap-a" label="Lap A" value={lapAValue} options={lapOptions} placeholder="Select Lap A…"
                    onChange={option => onFixedLapModeChange({ ...fixedLapMode, lapA: option?.value ?? null })}
                    styles={selectStyles}
                  />
                  <AnalyzeLapSelector
                    id="analyze-lap-b" label="Lap B" value={lapBValue} options={lapOptions} placeholder="Select Lap B…"
                    onChange={option => onFixedLapModeChange({ ...fixedLapMode, lapB: option?.value ?? null })}
                    styles={selectStyles}
                  />
                  {((fixedLapMode.lapA !== null && !fixedPrimary) || (fixedLapMode.lapB !== null && !fixedComparison)) && <div className="text-[9px] text-[var(--text-secondary)]">Loading selected lap data…</div>}
                </div>
              ) : (
                <div>
                  <AnalyzeLapSelector
                    id="analyze-compare-lap" label="Compare Lap" value={compareValue} options={compareOptions} placeholder="Select lap…"
                    onChange={option => onCompareLapChange(option && option.value !== 0 ? option.value : null)}
                    styles={selectStyles} isClearable={false} isDisabled={!playbackFilename || !blocks}
                  />
                  {playbackFilename && compareLapNum !== null && !comparison && <div className="text-[9px] text-[var(--text-secondary)] mt-1">Loading comparison lap…</div>}
                </div>
              )}
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
                      <button
                        title={item.visible ? 'Hide series' : 'Show series'} aria-label={item.visible ? `Hide ${def.label}` : `Show ${def.label}`}
                        onClick={() => updateSeries(config.series.map(entry => entry.metricId === item.metricId ? { ...entry, visible: !entry.visible } : entry))}
                        className={`p-1 hover:text-[var(--text-primary)] ${item.visible ? 'text-[var(--text-secondary)]' : 'text-[var(--text-inactive)]'}`}
                      >
                        {item.visible ? <Eye size={11} /> : <EyeOff size={11} />}
                      </button>
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
        <div className="h-11 px-2 border-b border-[var(--border)] flex items-center gap-1 shrink-0">
          <button
            title={config.collapsed ? 'Open Analyze controls' : 'Collapse Analyze controls'}
            aria-label={config.collapsed ? 'Open Analyze controls' : 'Collapse Analyze controls'}
            onClick={() => save({ ...config, collapsed: !config.collapsed })}
            className="w-7 h-7 rounded flex items-center justify-center shrink-0 text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]"
          >
            {config.collapsed ? <PanelLeftOpen size={15} /> : <PanelLeftClose size={15} />}
          </button>
          <span className="h-5 w-px mx-1 bg-[var(--border)]" />
          <span className="px-1 text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">Zoom</span>
          <button disabled={!fixedLapMode.enabled || !fixedPrimary} title="Zoom out" onClick={() => chartControlsRef.current?.zoomOut()} className="w-7 h-7 rounded flex items-center justify-center text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-25 disabled:pointer-events-none"><ZoomOut size={14} /></button>
          <button disabled={!fixedLapMode.enabled || !fixedPrimary} title="Zoom in" onClick={() => chartControlsRef.current?.zoomIn()} className="w-7 h-7 rounded flex items-center justify-center text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-25 disabled:pointer-events-none"><ZoomIn size={14} /></button>
          <span className="h-5 w-px mx-1 bg-[var(--border)]" />
          <span className="px-1 text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">Pan</span>
          <button disabled={!fixedLapMode.enabled || !fixedPrimary} title="Pan left" onClick={() => chartControlsRef.current?.panLeft()} className="w-7 h-7 rounded flex items-center justify-center text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-25 disabled:pointer-events-none"><ArrowLeft size={14} /></button>
          <button disabled={!fixedLapMode.enabled || !fixedPrimary} title="Pan right" onClick={() => chartControlsRef.current?.panRight()} className="w-7 h-7 rounded flex items-center justify-center text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-25 disabled:pointer-events-none"><ArrowRight size={14} /></button>
          <button disabled={!fixedLapMode.enabled || !fixedPrimary} title="Reset zoom" onClick={() => chartControlsRef.current?.reset()} className="w-7 h-7 rounded flex items-center justify-center text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-25 disabled:pointer-events-none"><RotateCcw size={13} /></button>
          {fixedLapMode.enabled && fixedPrimary && <span className="ml-auto pr-1 text-[9px] text-[var(--text-secondary)] max-[1100px]:hidden">Ctrl+wheel to zoom · drag to pan · double-click to reset</span>}
        </div>
        <div className="flex-1 min-h-0 relative">
          <AnalyzeChartSubscriber
            isDark={isDark} selected={config.series} showYAxis={config.showYAxis}
            currentLapNum={fixedLapMode.enabled ? fixedLapMode.lapA : effectiveCurrentLapNum}
            comparison={comparison} fixedMode={fixedLapMode.enabled} primaryOverride={fixedPrimary}
            controlsRef={chartControlsRef}
          />
        </div>
      </section>
    </div>
  )
}
