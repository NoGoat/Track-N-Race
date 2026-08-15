import { memo, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState, type CSSProperties, type MutableRefObject } from 'react'
import { createPortal } from 'react-dom'
import { type GroupBase, type SingleValue } from 'react-select'
import Select from '../lib/AnimatedSelect'
import { Chrome, ChromeInputType } from '@uiw/react-color'
import { AlertTriangle, ArrowDownUp, ArrowLeft, ArrowRight, Axis3d, ChevronLeft, ChevronRight, Eye, GripVertical, PanelLeftClose, PanelLeftOpen, RotateCcw, Trash2, Upload, X, ZoomIn, ZoomOut } from 'lucide-react'
import { useAppConfig } from '../hooks/useAppConfig'
import {
  ANALYZE_METRICS, ANALYZE_METRIC_BY_ID, DEFAULT_ANALYZE_CONFIG,
  DEFAULT_DELTA_NEGATIVE_COLOR, DEFAULT_DELTA_POSITIVE_COLOR, sanitizeAnalyzeConfig,
  type AnalyzeConfig, type AnalyzeSeriesConfig,
} from '../lib/analyzeMetrics'
import { buildSelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import { BUTTON_CLASS, PRIMARY_BUTTON_CLASS } from '../lib/buttonStyles'
import { useLabels } from '../lib/labels'
import { useModalPresenceValue } from '../lib/useModalPresence'
import { dataMaskForAnalyze } from '../lib/historyDependencies'
import { mergeAnalyzeLapData } from '../lib/analyzeLapData'
import { useTelemetryStore } from '../stores/telemetryStore'
import type { AnalyzeLapData } from '../types'
import AnalyzeTimeChart, { type AnalyzeChartControls } from './charts/AnalyzeTimeChart'
import AnalyzeMapComparison, { type AnalyzeMapFocus } from './AnalyzeMapComparison'

interface Props {
  isDark: boolean
  playbackFilename: string | null
  currentLapNum: number | null
  compareLapNum: number | null
  onCompareLapChange: (lapNum: number | null) => void
  fixedLapMode: AnalyzeFixedLapMode
  onFixedLapModeChange: (mode: AnalyzeFixedLapMode) => void
  mapDimmed: boolean
  reduceAnimations: boolean
  sectorColors: boolean
  onDataMaskChange: (mask: number) => void
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
interface LapOption {
  value: number
  label: string
  compound: string | null
  compoundColor: string | null
  lapTime: string | null
  isFastest: boolean
}
interface ComparisonLapOption extends Omit<LapOption, 'value'> {
  value: string
  lapNum: number
}
interface SecondaryFileData {
  filename: string
  trackId: number | null
  blocks: LapBlock[]
  fastestLapNum: number | null
  lapTimesByNum: Record<number, number>
  deltaAvailable: boolean
}
type AnalysisFileSource = 'file1' | 'file2'
interface PendingCircuitMismatch {
  filePath: string
  data: any
  trackId: number | null
  trackName: string
}

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
    style={{ backgroundColor: 'color-mix(in srgb, var(--color-fastest) 14%, transparent)', color: 'var(--color-fastest)' }}
  >FL</span>
}

function formatLapTimeMs(milliseconds: number | undefined): string | null {
  if (!Number.isFinite(milliseconds) || milliseconds! <= 0) return null
  const minutes = Math.floor(milliseconds! / 60_000)
  const seconds = ((milliseconds! % 60_000) / 1000).toFixed(3).padStart(6, '0')
  return `${minutes}:${seconds}`
}

function formatLapOption(option: LapOption) {
  if (!option.compound) return <span className="inline-flex items-center min-w-0">
    {option.value === 0 ? 'None' : `Lap ${option.value}`}
    {option.lapTime && <span>&nbsp;· {option.lapTime}</span>}
    {option.isFastest && <FastestLapChip />}
  </span>
  return <span className="inline-flex items-center min-w-0">
    <span style={{ color: option.compoundColor ?? 'var(--text-primary)' }}>{option.compound}</span>
    <span>&nbsp;· Lap {option.value}</span>
    {option.lapTime && <span>&nbsp;· {option.lapTime}</span>}
    {option.isFastest && <FastestLapChip />}
  </span>
}

function formatComparisonLapOption(option: ComparisonLapOption) {
  return formatLapOption({ ...option, value: option.lapNum })
}

function AnalyzeComparisonSelector({
  id, label, placeholder, value, options, onChange, styles, isDisabled = false,
}: {
  id: string
  label: string
  placeholder: string
  value: ComparisonLapOption | null
  options: GroupBase<ComparisonLapOption>[]
  onChange: (option: SingleValue<ComparisonLapOption>) => void
  styles: ReturnType<typeof buildSelectStyles>
  isDisabled?: boolean
}) {
  return <div className="flex items-center gap-2">
    <label htmlFor={id} className="shrink-0 text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">{label}</label>
    <div className="flex-1 min-w-0">
      <Select<ComparisonLapOption, false, GroupBase<ComparisonLapOption>>
        inputId={id} value={value} options={options} placeholder={placeholder} onChange={onChange}
        formatOptionLabel={formatComparisonLapOption} styles={styles} components={selectComponents}
        isSearchable={false} isClearable isDisabled={isDisabled} menuPortalTarget={document.body}
      />
    </div>
  </div>
}

function parseAnalyzeLapData(payload: any): AnalyzeLapData | null {
  if (!payload || payload.type !== 'playback_lap_data' || !Number.isFinite(payload.lapNum)) return null
  return {
    lapNum: payload.lapNum,
    startSessionTime: payload.startSessionTime,
    endSessionTime: payload.endSessionTime,
    telemetry: payload.telemetry ?? [],
    motion: payload.motionHistory ?? [],
    motionEx: payload.motionExHistory ?? [],
    statusHistory: payload.statusHistory ?? [],
    damageHistory: payload.damageHistory ?? [],
    lapProgress: payload.lapProgress ?? [],
    playerPositions: payload.playerPositions ?? [],
    rowTypeMask: Number.isFinite(payload.rowTypeMask) ? payload.rowTypeMask >>> 0 : 0xFFFFFFFF,
  }
}

function AnalysisViewSelector({
  value, mapDisabled, onChange,
}: {
  value: 'graph' | 'map'
  mapDisabled: boolean
  onChange: (value: 'graph' | 'map') => void
}) {
  return <div
    className="h-6 inline-flex overflow-hidden rounded-[4px] border border-[var(--border)] divide-x divide-[var(--border)]"
    role="group"
    aria-label="Analysis view"
  >
    {(['graph', 'map'] as const).map(option => {
      const selected = value === option
      const disabled = option === 'map' && mapDisabled
      return <button
        key={option}
        type="button"
        aria-pressed={selected}
        disabled={disabled}
        onClick={() => onChange(option)}
        className={`w-11 h-full text-[8px] font-semibold uppercase tracking-[0.1em] transition-colors ${
          selected
            ? 'bg-[var(--border-focus)]/15 text-[var(--text-primary)]'
            : 'bg-transparent text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
        } disabled:opacity-30 disabled:pointer-events-none`}
      >{option}</button>
    })}
  </div>
}

function AnalyzeColorPicker({
  label, color, onChange, triggerClassName, triggerStyle,
}: {
  label: string
  color: string
  onChange: (color: string) => void
  triggerClassName?: string
  triggerStyle?: CSSProperties
}) {
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
      className={triggerClassName ?? 'w-5 h-5 rounded border border-[var(--border)] cursor-pointer shrink-0 shadow-inner'}
      style={{ backgroundColor: color, ...triggerStyle }}
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

function DeltaColorPicker({
  positiveColor, negativeColor, onPositiveChange, onNegativeChange, disabled = false,
}: {
  positiveColor: string
  negativeColor: string
  onPositiveChange: (color: string) => void
  onNegativeChange: (color: string) => void
  disabled?: boolean
}) {
  return <div aria-disabled={disabled} className={`flex gap-1 shrink-0 ${disabled ? 'grayscale opacity-40 pointer-events-none' : ''}`}>
    <AnalyzeColorPicker
      label="Positive delta"
      color={positiveColor}
      onChange={onPositiveChange}
    />
    <AnalyzeColorPicker
      label="Negative delta"
      color={negativeColor}
      onChange={onNegativeChange}
    />
  </div>
}

const AnalyzeChartSubscriber = memo(function AnalyzeChartSubscriber({
  isDark, selected, deltaPositiveColor, deltaNegativeColor,
  currentLapNum, comparison, fixedMode, primaryOverride, distanceMode, controlsRef,
  onInspectMap,
}: {
  isDark: boolean
  selected: AnalyzeSeriesConfig[]
  deltaPositiveColor: string
  deltaNegativeColor: string
  currentLapNum: number | null
  comparison: AnalyzeLapData | null
  fixedMode: boolean
  primaryOverride: AnalyzeLapData | null
  distanceMode: boolean
  controlsRef: MutableRefObject<AnalyzeChartControls | null>
  onInspectMap?: (elapsedSeconds: number) => void
}) {
  const telemetry = useTelemetryStore(s => fixedMode ? EMPTY_ROWS : s.analyzeLapTelemetry)
  const motion = useTelemetryStore(s => fixedMode ? EMPTY_ROWS : s.analyzeLapMotion)
  const motionEx = useTelemetryStore(s => fixedMode ? EMPTY_ROWS : s.analyzeLapMotionEx)
  const statusHistory = useTelemetryStore(s => fixedMode ? EMPTY_ROWS : s.analyzeLapStatusHistory)
  const damageHistory = useTelemetryStore(s => fixedMode ? EMPTY_ROWS : s.analyzeLapDamageHistory)
  const lapProgress = useTelemetryStore(s => fixedMode ? EMPTY_ROWS : s.analyzeLapProgress)
  const startSessionTime = useTelemetryStore(s => fixedMode ? 0 : s.analyzeLapStartTime)
  const liveRevision = useTelemetryStore(s => fixedMode ? 0 : s.analyzeLapRevision)
  const trackLengthM = useTelemetryStore(s => s.analyzeTrackLengthM)
  const playbackCurrentLap = useTelemetryStore(s =>
    !fixedMode && currentLapNum !== null && s.speedRpmBlocks !== null
      ? s.playbackLapDataCache[currentLapNum] ?? null
      : null)

  const liveCurrent = useMemo<AnalyzeLapData>(() => {
    const ends = [telemetry, motion, motionEx, statusHistory, damageHistory]
      .flatMap(rows => rows.length ? [rows[rows.length - 1].session_time] : [])
    return {
      lapNum: currentLapNum ?? 0,
      startSessionTime: playbackCurrentLap?.startSessionTime ?? startSessionTime,
      endSessionTime: ends.length ? Math.max(...ends) : startSessionTime,
      telemetry, motion, motionEx, statusHistory, damageHistory,
      lapProgress: playbackCurrentLap?.lapProgress ?? lapProgress,
      playerPositions: playbackCurrentLap?.playerPositions ?? [],
    }
  }, [currentLapNum, damageHistory, lapProgress, motion, motionEx, playbackCurrentLap, startSessionTime, statusHistory, telemetry])
  const current = fixedMode ? primaryOverride ?? EMPTY_ANALYZE_LAP : liveCurrent
  const currentRevision = fixedMode
    ? `fixed:${current.lapNum}:${current.startSessionTime}:${current.endSessionTime}`
    : `${liveRevision}:${current.lapNum}:${playbackCurrentLap ? `cached:${playbackCurrentLap.startSessionTime}` : 'stream'}`

  return <div className="absolute inset-0">
    <AnalyzeTimeChart
      isDark={isDark} current={current} currentRevision={currentRevision} comparison={comparison}
      selected={selected}
      primaryLabel={fixedMode ? `LAP A · L${current.lapNum || '—'}` : undefined}
      comparisonLabel={fixedMode && comparison ? `LAP B · L${comparison.lapNum}` : undefined}
      distanceMode={distanceMode} trackLengthM={trackLengthM}
      deltaPositiveColor={deltaPositiveColor} deltaNegativeColor={deltaNegativeColor}
      zoomEnabled={fixedMode && primaryOverride !== null} controlsRef={controlsRef}
      onInspectMap={onInspectMap}
    />
  </div>
})

const EMPTY_ROWS: never[] = []
const EMPTY_ANALYZE_LAP: AnalyzeLapData = {
  lapNum: 0, startSessionTime: 0, endSessionTime: 0,
  telemetry: [], motion: [], motionEx: [], statusHistory: [], damageHistory: [],
  lapProgress: [], playerPositions: [],
  rowTypeMask: 0,
}

export default function AnalyzeScreen({
  isDark, playbackFilename, currentLapNum, compareLapNum, onCompareLapChange,
  fixedLapMode, onFixedLapModeChange, mapDimmed, reduceAnimations, sectorColors,
  onDataMaskChange,
}: Props) {
  const [rawConfig, setRawConfig] = useAppConfig<AnalyzeConfig>('analyze', DEFAULT_ANALYZE_CONFIG)
  const config = useMemo(() => sanitizeAnalyzeConfig(rawConfig), [rawConfig])
  const analysisView = playbackFilename ? config.view : 'graph'
  const dataMask = useMemo(() => dataMaskForAnalyze(analysisView, config.series), [analysisView, config.series])
  useLayoutEffect(() => onDataMaskChange(dataMask), [dataMask, onDataMaskChange])
  const allAxesEnabled = config.series.every(item => item.showYAxis)
  const blocks = useTelemetryStore(s => s.speedRpmBlocks) as LapBlock[] | null
  const lapCache = useTelemetryStore(s => s.playbackLapDataCache)
  const liveLapNum = useTelemetryStore(s => s.lap?.lap_num ?? null)
  const fastestLapNum = useTelemetryStore(s => s.fastestLapNum)
  const lapTimesByNum = useTelemetryStore(s => s.lapTimesByNum)
  const deltaAvailable = useTelemetryStore(s => s.analyzeDeltaAvailable)
  const primaryTrackId = useTelemetryStore(s => s.playbackTrackId)
  const primaryTrackName = useTelemetryStore(s => s.playbackTrackName)
  const { tn } = useLabels()
  const effectiveCurrentLapNum = currentLapNum ?? liveLapNum
  const [draggedMetric, setDraggedMetric] = useState<string | null>(null)
  const [secondaryFile, setSecondaryFile] = useState<SecondaryFileData | null>(null)
  const [secondaryLapNum, setSecondaryLapNum] = useState<number | null>(null)
  const [lapASource, setLapASource] = useState<AnalysisFileSource>('file1')
  const [lapBSource, setLapBSource] = useState<AnalysisFileSource>('file1')
  const [secondaryLapCache, setSecondaryLapCache] = useState<Record<number, AnalyzeLapData>>({})
  const [secondaryLoading, setSecondaryLoading] = useState(false)
  const [secondaryError, setSecondaryError] = useState<string | null>(null)
  const [pendingCircuitMismatch, setPendingCircuitMismatch] = useState<PendingCircuitMismatch | null>(null)
  const circuitMismatchPresence = useModalPresenceValue(pendingCircuitMismatch)
  const displayedCircuitMismatch = circuitMismatchPresence.value
  const [mapFocus, setMapFocus] = useState<AnalyzeMapFocus | null>(null)
  const mapFocusIdRef = useRef(0)
  const requestedRef = useRef(new Map<number, number>())
  const secondaryRequestedRef = useRef(new Map<number, number>())
  const chartControlsRef = useRef<AnalyzeChartControls | null>(null)
  const sidebarRef = useRef<HTMLElement>(null)
  const previousCollapsedRef = useRef(config.collapsed)
  const selectStyles = useMemo(() => buildSelectStyles(isDark, {
    solidBg: true,
    controlHeight: 32,
    labelStyleGroupHeadings: true,
  }), [isDark])

  const save = useCallback((next: AnalyzeConfig) => setRawConfig(next), [setRawConfig])
  const updateSeries = useCallback((series: AnalyzeSeriesConfig[]) => save({ ...config, series }), [config, save])
  const inspectMapAt = useCallback((elapsedSeconds: number) => {
    if (!playbackFilename) return
    setMapFocus({ id: ++mapFocusIdRef.current, elapsedSeconds })
    save({ ...config, view: 'map' })
  }, [config, playbackFilename, save])

  const selectedIds = useMemo(() => new Set(config.series.map(item => item.metricId)), [config.series])
  const metricOptions = useMemo(() => ['Driving', 'Motion', 'Power', 'Tyres'].map(group => ({
    label: group,
    options: ANALYZE_METRICS.filter(metric => metric.group === group && !selectedIds.has(metric.id)).map(metric => ({ value: metric.id, label: metric.label })),
  })).filter(group => group.options.length), [selectedIds])

  const makeLapOption = useCallback((block: LapBlock, fastest: number | null, lapTimes: Record<number, number>): LapOption => {
    const status = lastTyreStatus(block)
    const isFastest = block.lapNum === fastest
    const lapTime = formatLapTimeMs(lapTimes[block.lapNum])
    if (!status) return {
      value: block.lapNum,
      label: `Lap ${block.lapNum}${lapTime ? ` · ${lapTime}` : ''}${isFastest ? ' · FL' : ''}`,
      compound: null, compoundColor: null, lapTime, isFastest,
    }
    const actual = tn('tyre.actual', status.tyre_compound)
    return {
      value: block.lapNum,
      label: `${actual} · Lap ${block.lapNum}${lapTime ? ` · ${lapTime}` : ''}${isFastest ? ' · FL' : ''}`,
      compound: actual,
      compoundColor: COMPOUND_COLORS[status.visual_compound] ?? null,
      lapTime,
      isFastest,
    }
  }, [tn])

  const lapOption = useCallback(
    (block: LapBlock): LapOption => makeLapOption(block, fastestLapNum, lapTimesByNum),
    [fastestLapNum, lapTimesByNum, makeLapOption],
  )

  const file1CompareOptions = useMemo<ComparisonLapOption[]>(() => (blocks ?? []).map(block => {
    const option = lapOption(block)
    return { ...option, value: `file1:${block.lapNum}`, lapNum: block.lapNum }
  }), [blocks, lapOption])
  const file2CompareOptions = useMemo<ComparisonLapOption[]>(() => (secondaryFile?.blocks ?? []).map(block => {
    const option = makeLapOption(block, secondaryFile?.fastestLapNum ?? null, secondaryFile?.lapTimesByNum ?? {})
    return { ...option, value: `file2:${block.lapNum}`, lapNum: block.lapNum }
  }), [makeLapOption, secondaryFile])
  const compareOptions = useMemo<GroupBase<ComparisonLapOption>[]>(() => [
    { label: 'Primary File', options: file1CompareOptions },
    ...(secondaryFile ? [{ label: 'Secondary File', options: file2CompareOptions }] : []),
  ], [file1CompareOptions, file2CompareOptions, secondaryFile])
  const compareValue = secondaryLapNum !== null
    ? file2CompareOptions.find(option => option.lapNum === secondaryLapNum) ?? null
    : file1CompareOptions.find(option => option.lapNum === compareLapNum) ?? null
  const lapAValue = fixedLapMode.lapA === null ? null
    : (lapASource === 'file2' ? file2CompareOptions : file1CompareOptions)
      .find(option => option.lapNum === fixedLapMode.lapA) ?? null
  const lapBValue = fixedLapMode.lapB === null ? null
    : (lapBSource === 'file2' ? file2CompareOptions : file1CompareOptions)
      .find(option => option.lapNum === fixedLapMode.lapB) ?? null
  const fixedPrimary = fixedLapMode.lapA === null ? null
    : lapASource === 'file2'
      ? secondaryLapCache[fixedLapMode.lapA] ?? null
      : lapCache[fixedLapMode.lapA] ?? null
  const fixedComparison = fixedLapMode.lapB === null ? null
    : lapBSource === 'file2'
      ? secondaryLapCache[fixedLapMode.lapB] ?? null
      : lapCache[fixedLapMode.lapB] ?? null
  const comparison = fixedLapMode.enabled
    ? fixedComparison
    : secondaryLapNum !== null
      ? secondaryLapCache[secondaryLapNum] ?? null
      : compareLapNum !== null ? lapCache[compareLapNum] ?? null : null
  const current = fixedLapMode.enabled
    ? fixedPrimary
    : effectiveCurrentLapNum !== null ? lapCache[effectiveCurrentLapNum] ?? null : null
  const mapTrackIds = [
    fixedLapMode.enabled
      ? fixedLapMode.lapA !== null ? (lapASource === 'file2' ? secondaryFile?.trackId ?? null : primaryTrackId) : null
      : current ? primaryTrackId : null,
    fixedLapMode.enabled
      ? fixedLapMode.lapB !== null ? (lapBSource === 'file2' ? secondaryFile?.trackId ?? null : primaryTrackId) : null
      : comparison ? (secondaryLapNum !== null ? secondaryFile?.trackId ?? null : primaryTrackId) : null,
  ].filter((trackId): trackId is number => trackId !== null)
  const distinctMapTrackIds = [...new Set(mapTrackIds)]
  const mapTrackId = distinctMapTrackIds[0] ?? primaryTrackId
  const compatibleMapCircuit = distinctMapTrackIds.length <= 1
  const selectedDistanceMode = deltaAvailable && (fixedLapMode.enabled
    ? (lapASource === 'file1' || secondaryFile?.deltaAvailable === true) &&
      (lapBSource === 'file1' || secondaryFile?.deltaAvailable === true)
    : secondaryLapNum === null || secondaryFile?.deltaAvailable === true)

  const applySecondaryFile = useCallback((filePath: string, data: any, trackId: number | null) => {
    const times: Record<number, number> = {}
    for (const lap of data?.laps ?? []) {
      if (Number.isFinite(lap.lapNum) && Number.isFinite(lap.lapTimeMs) && lap.lapTimeMs > 0) {
        times[lap.lapNum] = lap.lapTimeMs
      }
    }
    setSecondaryFile({
      filename: filePath.split(/[\\/]/).pop() ?? filePath,
      trackId,
      blocks: Array.isArray(data?.blocks) ? data.blocks : [],
      fastestLapNum: Number.isFinite(data?.fastestLapNum) ? data.fastestLapNum : null,
      lapTimesByNum: times,
      deltaAvailable: data?.lapDistanceAvailable === true || data?.deltaAvailable === true,
    })
    setSecondaryLapCache({})
    secondaryRequestedRef.current.clear()
    setSecondaryLapNum(null)
    setLapASource('file1')
    setLapBSource('file1')
  }, [])

  const loadSecondaryFile = useCallback(async () => {
    const filePath = await window.fsBridge.selectTNRDFile()
    if (!filePath) return
    setSecondaryLoading(true)
    setSecondaryError(null)
    const result = await window.analysisBridge.loadFile(filePath)
    setSecondaryLoading(false)
    if (!result.ok) {
      setSecondaryFile(null)
      setSecondaryLapCache({})
      secondaryRequestedRef.current.clear()
      setSecondaryLapNum(null)
      setLapASource('file1')
      setLapBSource('file1')
      setSecondaryError(result.error ?? 'The recording could not be opened.')
      return
    }
    const data = result.data as any
    if (primaryTrackId !== null && Number.isFinite(result.trackId) && result.trackId !== primaryTrackId) {
      setPendingCircuitMismatch({
        filePath,
        data,
        trackId: Number.isFinite(result.trackId) ? result.trackId! : null,
        trackName: result.trackName || `Circuit ${result.trackId}`,
      })
      return
    }
    applySecondaryFile(filePath, data, Number.isFinite(result.trackId) ? result.trackId! : null)
  }, [applySecondaryFile, primaryTrackId])

  const clearSecondaryFile = useCallback(() => {
    window.analysisBridge.closeFile()
    setSecondaryFile(null)
    setSecondaryLapCache({})
    secondaryRequestedRef.current.clear()
    setSecondaryLapNum(null)
    setSecondaryError(null)
    setPendingCircuitMismatch(null)
    if (lapASource === 'file2' || lapBSource === 'file2') {
      onFixedLapModeChange({
        ...fixedLapMode,
        lapA: lapASource === 'file2' ? null : fixedLapMode.lapA,
        lapB: lapBSource === 'file2' ? null : fixedLapMode.lapB,
      })
    }
    setLapASource('file1')
    setLapBSource('file1')
  }, [fixedLapMode, lapASource, lapBSource, onFixedLapModeChange])

  useEffect(() => {
    requestedRef.current.clear()
  }, [playbackFilename])

  useLayoutEffect(() => {
    const sidebar = sidebarRef.current
    if (!sidebar) return

    const targetWidth = config.collapsed ? 0 : 315
    if (previousCollapsedRef.current === config.collapsed) {
      sidebar.style.width = `${targetWidth}px`
      return
    }
    previousCollapsedRef.current = config.collapsed

    const startWidth = sidebar.getBoundingClientRect().width
    if (reduceAnimations || window.matchMedia('(prefers-reduced-motion: reduce)').matches || startWidth === targetWidth) {
      sidebar.style.width = `${targetWidth}px`
      return
    }

    const duration = 200
    const startedAt = performance.now()
    let animationFrame = 0
    const animate = (now: number) => {
      const progress = Math.min(1, (now - startedAt) / duration)
      const eased = 1 - Math.pow(1 - progress, 3)
      sidebar.style.width = `${startWidth + (targetWidth - startWidth) * eased}px`
      if (progress < 1) animationFrame = requestAnimationFrame(animate)
    }
    animationFrame = requestAnimationFrame(animate)
    return () => cancelAnimationFrame(animationFrame)
  }, [config.collapsed, reduceAnimations])

  useEffect(() => {
    if (!playbackFilename) return
    const targets = fixedLapMode.enabled
      ? [lapASource === 'file1' ? fixedLapMode.lapA : null, lapBSource === 'file1' ? fixedLapMode.lapB : null]
      : [compareLapNum, effectiveCurrentLapNum]
    for (const lapNum of targets) {
      if (lapNum === null) continue
      const loadedMask = lapCache[lapNum]?.rowTypeMask ?? 0
      if ((loadedMask & dataMask) === dataMask) {
        requestedRef.current.delete(lapNum)
        continue
      }
      if (((requestedRef.current.get(lapNum) ?? 0) & dataMask) === dataMask) continue
      requestedRef.current.set(lapNum, dataMask)
      window.playerBridge.getLapData(lapNum, dataMask)
    }
  }, [compareLapNum, dataMask, effectiveCurrentLapNum, fixedLapMode.enabled, fixedLapMode.lapA, fixedLapMode.lapB, lapASource, lapBSource, lapCache, playbackFilename])

  useEffect(() => {
    const targets = [
      secondaryLapNum,
      fixedLapMode.enabled && lapASource === 'file2' ? fixedLapMode.lapA : null,
      fixedLapMode.enabled && lapBSource === 'file2' ? fixedLapMode.lapB : null,
    ]
    for (const lapNum of new Set(targets)) {
      const loadedMask = lapNum === null ? 0 : secondaryLapCache[lapNum]?.rowTypeMask ?? 0
      if (lapNum === null || (loadedMask & dataMask) === dataMask ||
          (((secondaryRequestedRef.current.get(lapNum) ?? 0) & dataMask) === dataMask)) continue
      secondaryRequestedRef.current.set(lapNum, dataMask)
      window.analysisBridge.getLapData(lapNum, dataMask).then(payload => {
        const lapData = parseAnalyzeLapData(payload)
        if (lapData) setSecondaryLapCache(cache => {
          const prior = cache[lapData.lapNum]
          return { ...cache, [lapData.lapNum]: prior ? mergeAnalyzeLapData(prior, lapData) : lapData }
        })
        else secondaryRequestedRef.current.delete(lapNum)
      })
    }
  }, [dataMask, fixedLapMode.enabled, fixedLapMode.lapA, fixedLapMode.lapB, lapASource, lapBSource, secondaryLapCache, secondaryLapNum])

  const addMetric = useCallback((option: SingleValue<SelectOption>) => {
    if (!option) return
    const def = ANALYZE_METRIC_BY_ID.get(option.value)
    if (!def || config.series.some(item => item.metricId === def.id)) return
    updateSeries([...config.series, { metricId: def.id, color: def.defaultColor, visible: true, showYAxis: true }])
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
      <aside
        ref={sidebarRef}
        className={`${config.collapsed ? 'border-r-0' : 'border-r'} shrink-0 border-[var(--border)] overflow-hidden bg-[var(--bg-panel)]`}
      >
          <div className={`w-[315px] h-full flex flex-col transition-[visibility] duration-0 ${config.collapsed ? 'invisible delay-200' : 'visible delay-0'}`}>
            <div className="h-11 px-3 flex items-center border-b border-[var(--border)] shrink-0">
              <div className="text-[11px] font-bold uppercase tracking-widest text-[var(--text-primary)] shrink-0">Analysis</div>
              <div className="ml-auto">
                <AnalysisViewSelector
                  value={analysisView}
                  mapDisabled={!playbackFilename}
                  onChange={view => {
                    setMapFocus(null)
                    save({ ...config, view })
                  }}
                />
              </div>
            </div>

            <div className="p-3 border-b border-[var(--border)] space-y-3 shrink-0">
              <div className="flex items-center gap-2">
                <label htmlFor="analyze-add-metric" className="shrink-0 text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">Add metric</label>
                <div className="flex-1 min-w-0">
                  <Select<SelectOption, false>
                    inputId="analyze-add-metric" value={null} options={metricOptions} onChange={addMetric} placeholder="Choose a value…"
                    styles={selectStyles} components={selectComponents} isSearchable menuPortalTarget={document.body}
                  />
                </div>
              </div>
              {playbackFilename && blocks && <div>
                <button
                  role="switch" aria-checked={fixedLapMode.enabled}
                  onClick={() => onFixedLapModeChange({ ...fixedLapMode, enabled: !fixedLapMode.enabled })}
                  className="w-full flex items-center justify-between text-[10px] uppercase tracking-wider text-[var(--text-secondary)]"
                >
                  <span>Compare Mode</span>
                  <span className={`w-8 h-4 rounded-full p-0.5 transition-colors ${fixedLapMode.enabled ? 'bg-[var(--border-focus)]' : 'bg-[var(--border)]'}`}><span className={`block w-3 h-3 rounded-full bg-white transition-transform ${fixedLapMode.enabled ? 'translate-x-4' : ''}`} /></span>
                </button>
              </div>}
              {playbackFilename && blocks && <div className="space-y-1">
                <div className="h-8 flex items-center gap-1 min-w-0">
                  <span
                    className={`flex-1 min-w-0 truncate text-[11px] ${secondaryFile ? 'text-[var(--text-primary)]' : 'text-[var(--text-secondary)]'}`}
                    title={secondaryFile?.filename}
                  >
                    {secondaryLoading ? 'Loading file' : secondaryFile?.filename ?? 'Choose file'}
                  </span>
                  <button
                    type="button" onClick={loadSecondaryFile} disabled={secondaryLoading}
                    title={secondaryFile ? 'Replace Secondary File' : 'Open Secondary File'}
                    aria-label={secondaryFile ? 'Replace Secondary File' : 'Open Secondary File'}
                    className="w-7 h-7 rounded flex items-center justify-center shrink-0 text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors disabled:opacity-40"
                  ><Upload size={14} /></button>
                  {secondaryFile && <button
                    type="button" onClick={clearSecondaryFile} disabled={secondaryLoading}
                    title="Clear Secondary File" aria-label="Clear Secondary File"
                    className="w-7 h-7 rounded flex items-center justify-center shrink-0 text-[var(--text-secondary)] hover:text-[#d44252] hover:bg-[var(--bg-hover)] transition-colors disabled:opacity-40"
                  ><X size={13} /></button>}
                </div>
                {secondaryError && <div className="px-1 text-[9px] text-[#d44252]">{secondaryError}</div>}
              </div>}
              {fixedLapMode.enabled ? (
                <div className="space-y-2">
                  <AnalyzeComparisonSelector
                    id="analyze-lap-a" label="Lap A" value={lapAValue} options={compareOptions} placeholder="Select Lap A…"
                    onChange={option => {
                      setLapASource(option?.value.startsWith('file2:') ? 'file2' : 'file1')
                      onFixedLapModeChange({ ...fixedLapMode, lapA: option?.lapNum ?? null })
                    }}
                    styles={selectStyles}
                  />
                  <AnalyzeComparisonSelector
                    id="analyze-lap-b" label="Lap B" value={lapBValue} options={compareOptions} placeholder="Select Lap B…"
                    onChange={option => {
                      setLapBSource(option?.value.startsWith('file2:') ? 'file2' : 'file1')
                      onFixedLapModeChange({ ...fixedLapMode, lapB: option?.lapNum ?? null })
                    }}
                    styles={selectStyles}
                  />
                </div>
              ) : (
                <div>
                  <AnalyzeComparisonSelector
                    id="analyze-compare-lap" label="Compare Lap" placeholder="Select lap…"
                    value={compareValue} options={compareOptions}
                    onChange={option => {
                      if (!option) {
                        setSecondaryLapNum(null)
                        onCompareLapChange(null)
                      } else if (option.value.startsWith('file2:')) {
                        setSecondaryLapNum(option.lapNum)
                        onCompareLapChange(null)
                      } else {
                        setSecondaryLapNum(null)
                        onCompareLapChange(option.lapNum)
                      }
                    }}
                    styles={selectStyles} isDisabled={!playbackFilename || !blocks}
                  />
                </div>
              )}
              {analysisView === 'map' && <div className="grid grid-cols-2 gap-2">
                <div className="h-8 px-2 flex items-center gap-2 rounded border border-[var(--border)] bg-[var(--bg-card)]/20">
                  <AnalyzeColorPicker
                    label={fixedLapMode.enabled ? 'Lap A' : 'Current'}
                    color={config.mapCurrentColor}
                    onChange={mapCurrentColor => save({ ...config, mapCurrentColor })}
                  />
                  <span className="text-[9px] uppercase tracking-wider text-[var(--text-secondary)] truncate">{fixedLapMode.enabled ? 'Lap A' : 'Current'}</span>
                </div>
                <div className="h-8 px-2 flex items-center gap-2 rounded border border-[var(--border)] bg-[var(--bg-card)]/20">
                  <AnalyzeColorPicker
                    label={fixedLapMode.enabled ? 'Lap B' : 'Compare'}
                    color={config.mapComparisonColor}
                    onChange={mapComparisonColor => save({ ...config, mapComparisonColor })}
                  />
                  <span className="text-[9px] uppercase tracking-wider text-[var(--text-secondary)] truncate">{fixedLapMode.enabled ? 'Lap B' : 'Compare'}</span>
                </div>
              </div>}
              <button
                type="button"
                onClick={() => updateSeries(config.series.map(item => ({ ...item, showYAxis: !allAxesEnabled })))}
                className={`${BUTTON_CLASS} w-full`}
              >
                <span>Toggle Y-Axes</span>
              </button>
            </div>

            <div className="flex-1 min-h-0 overflow-y-auto p-2 space-y-1">
              {config.series.length === 0 && <div className="p-4 text-center text-[10px] text-[var(--text-secondary)]">No metrics selected</div>}
              {config.series.map((item, index) => {
                if (item.metricId === 'delta') return (
                  <div
                    key="delta" draggable onDragStart={() => setDraggedMetric('delta')} onDragEnd={() => setDraggedMetric(null)}
                    onDragOver={event => event.preventDefault()} onDrop={() => dropMetric('delta')}
                    className={`flex items-center gap-1.5 px-1.5 py-1.5 rounded border border-transparent hover:border-[var(--border)] hover:bg-[var(--bg-hover)] ${draggedMetric === 'delta' ? 'opacity-40' : ''}`}
                  >
                    <GripVertical size={13} className="text-[var(--text-secondary)] cursor-grab shrink-0" />
                    <DeltaColorPicker
                      positiveColor={item.color}
                      negativeColor={item.negativeColor ?? DEFAULT_DELTA_NEGATIVE_COLOR}
                      onPositiveChange={color => updateSeries(config.series.map(entry => entry.metricId === 'delta' ? { ...entry, color } : entry))}
                      onNegativeChange={negativeColor => updateSeries(config.series.map(entry => entry.metricId === 'delta' ? { ...entry, negativeColor } : entry))}
                      disabled={!!playbackFilename && !selectedDistanceMode}
                    />
                    <div className="flex-1 min-w-0">
                      <div className="text-[10px] text-[var(--text-primary)] truncate">Delta</div>
                      <div className="text-[8px] uppercase tracking-wider text-[var(--text-secondary)] truncate">
                        {playbackFilename && !selectedDistanceMode ? 'Not supported in this file.' : 'Time · + / −'}
                      </div>
                    </div>
                    {(!playbackFilename || selectedDistanceMode) && <div className="flex items-center shrink-0">
                      <button disabled={index === 0} title="Move up" onClick={() => moveMetric('delta', -1)} className="p-1 text-[var(--text-secondary)] hover:text-[var(--text-primary)] disabled:opacity-20"><ChevronLeft size={12} className="rotate-90" /></button>
                      <button disabled={index === config.series.length - 1} title="Move down" onClick={() => moveMetric('delta', 1)} className="p-1 text-[var(--text-secondary)] hover:text-[var(--text-primary)] disabled:opacity-20"><ChevronRight size={12} className="rotate-90" /></button>
                      <button title={item.showYAxis ? 'Hide Y-axis' : 'Show Y-axis'} aria-label={item.showYAxis ? 'Hide Delta Y-axis' : 'Show Delta Y-axis'} onClick={() => updateSeries(config.series.map(entry => entry.metricId === 'delta' ? { ...entry, showYAxis: !entry.showYAxis } : entry))} className={`p-1 hover:text-[var(--text-primary)] ${item.showYAxis ? 'text-[var(--text-secondary)]' : 'text-[var(--text-inactive)]'}`}><Axis3d size={11} /></button>
                      <button title={item.visible ? 'Hide series' : 'Show series'} aria-label={item.visible ? 'Hide Delta' : 'Show Delta'} onClick={() => updateSeries(config.series.map(entry => entry.metricId === 'delta' ? { ...entry, visible: !entry.visible } : entry))} className={`p-1 hover:text-[var(--text-primary)] ${item.visible ? 'text-[var(--text-secondary)]' : 'text-[var(--text-inactive)]'}`}><Eye size={11} /></button>
                      <button title="Reset colors" onClick={() => updateSeries(config.series.map(entry => entry.metricId === 'delta' ? { ...entry, color: DEFAULT_DELTA_POSITIVE_COLOR, negativeColor: DEFAULT_DELTA_NEGATIVE_COLOR } : entry))} className="p-1 text-[var(--text-secondary)] hover:text-[var(--text-primary)]"><RotateCcw size={11} /></button>
                      <button disabled title="Delta cannot be removed" className="p-1 text-[var(--text-inactive)] opacity-30 cursor-not-allowed"><Trash2 size={11} /></button>
                    </div>}
                  </div>
                )
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
                        title={item.showYAxis ? 'Hide Y-axis' : 'Show Y-axis'} aria-label={item.showYAxis ? `Hide ${def.label} Y-axis` : `Show ${def.label} Y-axis`}
                        onClick={() => updateSeries(config.series.map(entry => entry.metricId === item.metricId ? { ...entry, showYAxis: !entry.showYAxis } : entry))}
                        className={`p-1 hover:text-[var(--text-primary)] ${item.showYAxis ? 'text-[var(--text-secondary)]' : 'text-[var(--text-inactive)]'}`}
                      ><Axis3d size={11} /></button>
                      <button
                        title={item.visible ? 'Hide series' : 'Show series'} aria-label={item.visible ? `Hide ${def.label}` : `Show ${def.label}`}
                        onClick={() => updateSeries(config.series.map(entry => entry.metricId === item.metricId ? { ...entry, visible: !entry.visible } : entry))}
                        className={`p-1 hover:text-[var(--text-primary)] ${item.visible ? 'text-[var(--text-secondary)]' : 'text-[var(--text-inactive)]'}`}
                      >
                        <Eye size={11} />
                      </button>
                      <button title="Reset color" onClick={() => updateSeries(config.series.map(entry => entry.metricId === item.metricId ? { ...entry, color: def.defaultColor } : entry))} className="p-1 text-[var(--text-secondary)] hover:text-[var(--text-primary)]"><RotateCcw size={11} /></button>
                      <button title="Remove metric" onClick={() => updateSeries(config.series.filter(entry => entry.metricId !== item.metricId))} className="p-1 text-[var(--text-secondary)] hover:text-[#d44252]"><Trash2 size={11} /></button>
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
            title={config.collapsed ? 'Open Analysis controls' : 'Collapse Analysis controls'}
            aria-label={config.collapsed ? 'Open Analysis controls' : 'Collapse Analysis controls'}
            onClick={() => save({ ...config, collapsed: !config.collapsed })}
            className="w-7 h-7 rounded flex items-center justify-center shrink-0 text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]"
          >
            {config.collapsed ? <PanelLeftOpen size={15} /> : <PanelLeftClose size={15} />}
          </button>
          {analysisView === 'graph' ? <>
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
            {!fixedLapMode.enabled && <span className="ml-auto pr-1 text-[9px] uppercase tracking-wider text-[var(--text-secondary)]">{selectedDistanceMode ? 'Lap distance' : 'Elapsed time'}</span>}
          </> : <span className="ml-auto pr-1 text-[9px] uppercase tracking-wider text-[var(--text-secondary)]">Elapsed time comparison</span>}
        </div>
        <div className="flex-1 min-h-0 relative">
          <div className={`absolute inset-0 ${analysisView === 'graph' ? '' : 'hidden'}`}>
            <AnalyzeChartSubscriber
              isDark={isDark} selected={config.series}
              deltaPositiveColor={config.series.find(item => item.metricId === 'delta')?.color ?? DEFAULT_DELTA_POSITIVE_COLOR}
              deltaNegativeColor={config.series.find(item => item.metricId === 'delta')?.negativeColor ?? DEFAULT_DELTA_NEGATIVE_COLOR}
              currentLapNum={fixedLapMode.enabled ? fixedLapMode.lapA : effectiveCurrentLapNum}
              comparison={comparison} fixedMode={fixedLapMode.enabled} primaryOverride={fixedPrimary}
              distanceMode={selectedDistanceMode}
              controlsRef={chartControlsRef}
              onInspectMap={playbackFilename ? inspectMapAt : undefined}
            />
          </div>
          {analysisView === 'map' && <AnalyzeMapComparison
            current={current}
            comparison={comparison}
            currentColor={config.mapCurrentColor}
            comparisonColor={config.mapComparisonColor}
            fixedMode={fixedLapMode.enabled}
            trackId={mapTrackId}
            compatibleCircuit={compatibleMapCircuit}
            isDark={isDark}
            sectorColors={sectorColors}
            reduceAnimations={reduceAnimations}
            mapDimmed={mapDimmed}
            focus={mapFocus}
          />}
        </div>
      </section>

      {circuitMismatchPresence.mounted && displayedCircuitMismatch && <div
        data-state={circuitMismatchPresence.visible ? 'open' : 'closed'}
        className="modal-backdrop fixed inset-0 z-[120] flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
        role="dialog" aria-modal="true" aria-labelledby="analysis-circuit-mismatch-title"
      >
        <div className="modal-panel bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[500px] max-w-[calc(100vw-2rem)] flex flex-col overflow-hidden">
          <div className="flex items-center justify-between px-6 py-4 border-b border-[var(--border)] shrink-0 select-none">
            <div id="analysis-circuit-mismatch-title" className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest flex items-center gap-2">
              <AlertTriangle size={15} className="text-amber-500" />
              <span>Circuit Mismatch</span>
            </div>
            <button onClick={() => setPendingCircuitMismatch(null)} aria-label="Cancel loading Secondary File" className="w-8 h-8 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#d44252] transition-colors">
              <X size={14} />
            </button>
          </div>

          <div className="p-6 flex flex-col gap-4">
            <p className="text-sm font-semibold text-[var(--text-primary)]">The Secondary File was recorded on a different circuit.</p>
            <p className="text-xs text-[var(--text-secondary)] leading-relaxed">Lap comparisons may not align correctly. You can cancel or load the file anyway.</p>
            <div className="p-3 rounded-lg bg-[var(--bg-card)]/30 border border-[var(--border)] text-xs font-mono text-[var(--text-secondary)] space-y-2">
              <div><span className="text-[var(--text-muted)]">Loaded File: </span><span className="font-semibold">{primaryTrackName || `Circuit ${primaryTrackId}`}</span></div>
              <div><span className="text-[var(--text-muted)]">Selected File: </span><span className="font-semibold">{displayedCircuitMismatch.trackName}</span></div>
            </div>
          </div>

          <div className="flex items-center justify-end gap-3 px-6 py-4 border-t border-[var(--border)] bg-[var(--bg-card)]/10 shrink-0">
            <button onClick={() => setPendingCircuitMismatch(null)} className={BUTTON_CLASS}>Cancel</button>
            <button onClick={() => {
              applySecondaryFile(displayedCircuitMismatch.filePath, displayedCircuitMismatch.data, displayedCircuitMismatch.trackId)
              setPendingCircuitMismatch(null)
            }} className={PRIMARY_BUTTON_CLASS}>Load Anyway</button>
          </div>
        </div>
      </div>}
    </div>
  )
}
