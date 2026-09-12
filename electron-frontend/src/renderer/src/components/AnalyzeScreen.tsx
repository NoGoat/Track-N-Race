import { memo, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState, type MutableRefObject, type ReactNode } from 'react'
import { createPortal, flushSync } from 'react-dom'
import { type GroupBase, type SingleValue } from 'react-select'
import Select from '../lib/AnimatedSelect'
import { AlertTriangle, ArrowLeft, ArrowRight, Axis3d, ChartNoAxesCombined, ChevronLeft, ChevronRight, CircleHelp, Columns2, Columns3, Eye, ListChevronsUpDown, GripVertical, LineChart, Map as MapIcon, PanelLeftClose, PanelLeftOpen, RotateCcw, Rows3, Trash2, Upload, X, ZoomIn, ZoomOut } from 'lucide-react'
import { useAppConfig } from '../hooks/useAppConfig'
import {
  ANALYZE_METRICS, ANALYZE_METRIC_BY_ID, DEFAULT_ANALYZE_CONFIG,
  DEFAULT_COMPARE_LABEL, DEFAULT_CURRENT_LABEL, DEFAULT_LAP_A_LABEL, DEFAULT_LAP_B_LABEL,
  DEFAULT_DELTA_NEGATIVE_COLOR, DEFAULT_DELTA_POSITIVE_COLOR, sanitizeAnalyzeConfig,
  type AnalyzeConfig, type AnalyzeSeriesConfig,
} from '../lib/analyzeMetrics'
import { buildSelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import { BUTTON_CLASS, PRIMARY_BUTTON_CLASS } from '../lib/buttonStyles'
import { useLabels } from '../lib/labels'
import { useModalPresence, useModalPresenceValue } from '../lib/useModalPresence'
import { DATA_ROW, dataMaskForAnalyze } from '../lib/historyDependencies'
import { mergeAnalyzeLapData } from '../lib/analyzeLapData'
import { buildLapProgressMap, findSectorSplits, type LapProgressMap } from '../lib/lapDelta'
import { getPlaybackCursorTime, subscribePlaybackCursor } from '../lib/playbackCursor'
import { useTelemetryStore } from '../stores/telemetryStore'
import type { AnalyzeDeltaData, AnalyzeDeltaSample, AnalyzeLapData } from '../types'
import AnalyzeTimeChart, { type AnalyzeChartControls } from './charts/AnalyzeTimeChart'
import AnalyzeStackedTimeCharts from './charts/AnalyzeStackedTimeCharts'
import AnalyzeMapComparison, { type AnalyzeMapFocus } from './AnalyzeMapComparison'
import SyncedTooltipIcon from '../app/components/SyncedTooltipIcon'
import ColorPicker from './ColorPicker'

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
const ANALYZE_TOGGLE_BUTTON_CLASS = 'analyze-toggle-button flex h-8 min-w-0 flex-1 items-center justify-center rounded focus-visible:outline-none disabled:pointer-events-none disabled:opacity-35'
const ANALYSIS_MOTION_EASING = 'cubic-bezier(0.65, 0, 0.35, 1)'
const ANALYSIS_MOTION_DURATION = 260
const ANALYSIS_PRESENCE_DURATION = ANALYSIS_MOTION_DURATION + 20

function easeInOutCubic(progress: number): number {
  return progress < 0.5
    ? 4 * progress * progress * progress
    : 1 - Math.pow(-2 * progress + 2, 3) / 2
}

interface PendingCircuitMismatch {
  filePath: string
  data: any
  trackId: number | null
  trackName: string
}

interface AnalysisViewTransition {
  ready: Promise<unknown>
  finished: Promise<unknown>
  skipTransition?: () => void
}

function AnalysisHelpItem({ icon, label, children }: {
  icon: ReactNode
  label: string
  children: ReactNode
}) {
  return <div className="flex min-w-0 gap-3 rounded-lg border border-[var(--border)] bg-[var(--bg-card)]/30 p-3">
    <span className="flex h-8 w-8 shrink-0 items-center justify-center rounded bg-[var(--border-focus)] text-white">
      {icon}
    </span>
    <div className="min-w-0">
      <div className="text-[10px] font-bold uppercase tracking-wider text-[var(--text-primary)]">{label}</div>
      <div className="mt-1 text-[10px] leading-relaxed text-[var(--text-secondary)]">{children}</div>
    </div>
  </div>
}

interface AnalyzeDeltaSummary {
  sectors: [number | null, number | null, number | null]
  lap: number | null
}

function resolvedSectorDistances(current: AnalyzeLapData | null, comparison: AnalyzeLapData | null): [number | null, number | null] {
  const bySector = new Map(findSectorSplits(comparison).map(split => [split.afterSector, split.distance]))
  for (const split of findSectorSplits(current)) bySector.set(split.afterSector, split.distance)
  return [bySector.get(1) ?? null, bySector.get(2) ?? null]
}

function interpolateDeltaAtDistance(samples: readonly AnalyzeDeltaSample[], distance: number | null): number | null {
  if (distance === null) return null
  let before: AnalyzeDeltaSample | null = null
  for (const sample of samples) {
    if (!sample.valid || !Number.isFinite(sample.delta_seconds)) continue
    if (sample.lap_distance_m < distance) {
      before = sample
      continue
    }
    if (sample.lap_distance_m === distance) return sample.delta_seconds
    if (!before) return null
    const span = sample.lap_distance_m - before.lap_distance_m
    if (span <= 0) return sample.delta_seconds
    const ratio = (distance - before.lap_distance_m) / span
    return before.delta_seconds + (sample.delta_seconds - before.delta_seconds) * ratio
  }
  return before?.delta_seconds ?? null
}

function interpolateDistanceAtSessionTime(progress: LapProgressMap, sessionTime: number): number | null {
  const points = progress.points
  if (sessionTime < points[0].session_time || sessionTime > progress.maxSessionTime) return null
  let lo = 1, hi = points.length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (points[mid].session_time < sessionTime) lo = mid + 1
    else hi = mid
  }
  if (lo >= points.length) return points[points.length - 1].lap_distance_m
  const before = points[lo - 1], after = points[lo]
  const span = after.session_time - before.session_time
  const ratio = span > 0 ? (sessionTime - before.session_time) / span : 1
  return before.lap_distance_m + (after.lap_distance_m - before.lap_distance_m) * ratio
}

function splitSectorDeltaSamples(samples: readonly AnalyzeDeltaSample[]): AnalyzeDeltaSample[][] {
  const sectors: AnalyzeDeltaSample[][] = [[]]
  for (const sample of samples) {
    if (!sample.valid) {
      if (sectors[sectors.length - 1].length > 0) sectors.push([])
      continue
    }
    if (Number.isFinite(sample.delta_seconds)) sectors[sectors.length - 1].push(sample)
  }
  return sectors.filter(samplesForSector => samplesForSector.length > 0)
}

function summarizeAnalyzeDelta(
  deltaData: AnalyzeDeltaData | null,
  current: AnalyzeLapData | null,
  comparison: AnalyzeLapData | null,
  cursorDistance?: number | null,
): AnalyzeDeltaSummary {
  if (!deltaData || !current || !comparison ||
      deltaData.currentLapNum !== current.lapNum || deltaData.comparisonLapNum !== comparison.lapNum ||
      cursorDistance === null) {
    return { sectors: [null, null, null], lap: null }
  }

  if (deltaData.sectorDelta) {
    const samplesBySector = splitSectorDeltaSamples(deltaData.samples)
    const fullDistance = samplesBySector.at(-1)?.at(-1)?.lap_distance_m ?? null
    const visibleDistance = cursorDistance === undefined ? fullDistance : cursorDistance
    if (visibleDistance === null) return { sectors: [null, null, null], lap: null }
    const sectors = [0, 1, 2].map(index => {
      const samples = samplesBySector[index]
      if (!samples?.length || visibleDistance < samples[0].lap_distance_m) return null
      return interpolateDeltaAtDistance(samples, Math.min(visibleDistance, samples[samples.length - 1].lap_distance_m))
    }) as AnalyzeDeltaSummary['sectors']
    const visibleSectors = sectors.filter((value): value is number => value !== null)
    const lap = visibleSectors.length > 0
      ? visibleSectors.reduce((total, value) => total + value, 0)
      : null
    return { sectors, lap }
  }

  const validSamples = deltaData.samples.filter(sample => sample.valid && Number.isFinite(sample.delta_seconds))
  const fullDistance = validSamples.at(-1)?.lap_distance_m ?? null
  const visibleDistance = cursorDistance === undefined ? fullDistance : cursorDistance
  if (visibleDistance === null) return { sectors: [null, null, null], lap: null }
  const [sector1End, sector2End] = resolvedSectorDistances(current, comparison)
  const sectorEnds = [sector1End, sector2End, fullDistance]
  const sectorStarts = [validSamples[0]?.lap_distance_m ?? 0, sector1End, sector2End]
  const cumulativeBaselines = [
    0,
    interpolateDeltaAtDistance(validSamples, sector1End),
    interpolateDeltaAtDistance(validSamples, sector2End),
  ]
  const sectors = sectorEnds.map((sectorEnd, index) => {
    const sectorStart = sectorStarts[index]
    if (sectorEnd === null || sectorStart === null || visibleDistance < sectorStart) return null
    const cumulativeDelta = interpolateDeltaAtDistance(validSamples, Math.min(visibleDistance, sectorEnd))
    const baseline = cumulativeBaselines[index]
    return cumulativeDelta === null || baseline === null ? null : cumulativeDelta - baseline
  }) as AnalyzeDeltaSummary['sectors']
  return {
    sectors,
    lap: interpolateDeltaAtDistance(validSamples, Math.min(visibleDistance, fullDistance ?? visibleDistance)),
  }
}

function formatDeltaValue(value: number | null): string {
  if (value === null || !Number.isFinite(value)) return '—.---'
  const normalized = Math.abs(value) < 0.0005 ? 0 : value
  return `${normalized > 0 ? '+' : ''}${normalized.toFixed(3)}`
}

function escapeTooltipText(value: string): string {
  return value.replace(/[&<>"']/g, character => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  })[character]!)
}

function AnalysisDeltaReadout({ deltaData, current, comparison, positiveColor, negativeColor, followPlaybackCursor }: {
  deltaData: AnalyzeDeltaData | null
  current: AnalyzeLapData | null
  comparison: AnalyzeLapData | null
  positiveColor: string
  negativeColor: string
  followPlaybackCursor: boolean
}) {
  const selectionRef = useRef({ current, comparison })
  const retainedDeltaDataRef = useRef<AnalyzeDeltaData | null>(null)
  const selectionChanged = selectionRef.current.current !== current ||
    selectionRef.current.comparison !== comparison
  if (selectionChanged) {
    selectionRef.current = { current, comparison }
    retainedDeltaDataRef.current = null
  } else if (!retainedDeltaDataRef.current && deltaData && current && comparison &&
             deltaData.currentLapNum === current.lapNum &&
             deltaData.comparisonLapNum === comparison.lapNum) {
    retainedDeltaDataRef.current = deltaData
  }
  const retainedDeltaData = retainedDeltaDataRef.current
  const valueRefs = useRef<Array<HTMLSpanElement | null>>([])
  const currentProgress = useMemo(() => buildLapProgressMap(current), [current])
  const updateValues = useCallback(() => {
    const cursorTime = followPlaybackCursor ? getPlaybackCursorTime() : null
    const cursorDistance = followPlaybackCursor
      ? currentProgress && cursorTime !== null
        ? interpolateDistanceAtSessionTime(currentProgress, cursorTime)
        : null
      : undefined
    const summary = summarizeAnalyzeDelta(retainedDeltaData, current, comparison, cursorDistance)
    const values = [...summary.sectors, summary.lap]
    for (let index = 0; index < values.length; index++) {
      const node = valueRefs.current[index]
      if (!node) continue
      const value = values[index]
      node.textContent = formatDeltaValue(value)
      node.style.color = value === null || Math.abs(value) < 0.0005
        ? 'var(--text-secondary)'
        : value > 0 ? positiveColor : negativeColor
    }
  }, [comparison, current, currentProgress, followPlaybackCursor, negativeColor, positiveColor, retainedDeltaData])

  useLayoutEffect(updateValues, [updateValues])
  useEffect(() => {
    if (!followPlaybackCursor) return
    let animationFrame = 0
    const scheduleUpdate = () => {
      if (animationFrame) return
      animationFrame = requestAnimationFrame(() => {
        animationFrame = 0
        updateValues()
      })
    }
    const unsubscribe = subscribePlaybackCursor(scheduleUpdate)
    scheduleUpdate()
    return () => {
      unsubscribe()
      if (animationFrame) cancelAnimationFrame(animationFrame)
    }
  }, [followPlaybackCursor, updateValues])

  return <div className="flex h-full items-center gap-3 font-mono tabular-nums">
    {(['S1', 'S2', 'S3', 'Lap'] as const).map((label, index) => {
      return <span key={label} className="flex h-full items-center gap-1.5">
        <span className="text-[9px] font-bold uppercase leading-none tracking-wider text-[var(--text-secondary)]">{label}</span>
        <span
          ref={node => { valueRefs.current[index] = node }}
          className="inline-block w-[3.5rem] text-left text-[11px] font-semibold leading-none text-[var(--text-secondary)]"
        >—.---</span>
      </span>
    })}
  </div>
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
  colorPicker, showColorPicker = false, displayOnly = false,
}: {
  id: string
  label: string
  placeholder: string
  value: ComparisonLapOption | null
  options: GroupBase<ComparisonLapOption>[]
  onChange: (option: SingleValue<ComparisonLapOption>) => void
  styles: ReturnType<typeof buildSelectStyles>
  isDisabled?: boolean
  colorPicker?: ReactNode
  showColorPicker?: boolean
  displayOnly?: boolean
}) {
  return <div className="flex items-center">
    <label htmlFor={id} title={label} className="mr-2 max-w-[100px] shrink-0 truncate text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">{label}</label>
    <div className={`analyze-map-color-slot ${showColorPicker ? 'analyze-map-color-slot--visible' : ''}`}>
      <div className="analyze-map-color-slot__inner">{colorPicker}</div>
    </div>
    <div className="flex-1 min-w-0">
      <Select<ComparisonLapOption, false, GroupBase<ComparisonLapOption>>
        inputId={id} value={value} options={options} placeholder={placeholder} onChange={onChange}
        formatOptionLabel={formatComparisonLapOption} styles={styles}
        components={displayOnly ? { DropdownIndicator: () => null, ClearIndicator: () => null } : selectComponents}
        isSearchable={false} isClearable={!displayOnly} isDisabled={isDisabled || displayOnly}
        menuPortalTarget={document.body}
      />
    </div>
  </div>
}

function AnalyzeLabelInput({ id, label, value, onChange }: {
  id: string
  label: string
  value: string
  onChange: (value: string) => void
}) {
  return <div className="flex items-center">
    <label htmlFor={id} className="mr-2 shrink-0 text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">{label}</label>
    <div className="relative min-w-0 flex-1">
      <input
        id={id}
        type="text"
        value={value}
        maxLength={40}
        autoComplete="off"
        spellCheck={false}
        aria-label={`${label} label`}
        placeholder={label}
        onChange={event => onChange(event.target.value)}
        className="h-8 w-full rounded border border-[var(--border)] bg-[var(--bg-input)] px-2.5 pr-8 text-[11px] text-[var(--text-primary)] outline-none transition-colors placeholder:text-[var(--text-secondary)] placeholder:opacity-80 focus:border-[var(--border-focus)]"
      />
      {value && <button
        type="button"
        title={`Clear ${label} label`}
        aria-label={`Clear ${label} label`}
        onMouseDown={event => event.preventDefault()}
        onClick={() => onChange('')}
        className="absolute right-1 top-1 flex h-6 w-6 items-center justify-center rounded text-[var(--text-secondary)] transition-colors hover:bg-[var(--bg-hover)] hover:text-[var(--text-primary)]"
      ><X size={12} /></button>}
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

function parseAnalyzeDeltaData(payload: any): AnalyzeDeltaData | null {
  if (!payload || !Number.isFinite(payload.currentLapNum) ||
      !Number.isFinite(payload.comparisonLapNum) || !Array.isArray(payload.samples)) return null
  return {
    currentLapNum: payload.currentLapNum,
    comparisonLapNum: payload.comparisonLapNum,
    sectorDelta: payload.sectorDelta === true,
    maxAbsDeltaSeconds: Number.isFinite(payload.maxAbsDeltaSeconds) ? payload.maxAbsDeltaSeconds : 0,
    samples: payload.samples.flatMap((sample: any) =>
      Number.isFinite(sample?.lap_distance_m) && Number.isFinite(sample?.delta_seconds)
        ? [{
            lap_distance_m: sample.lap_distance_m,
            delta_seconds: sample.delta_seconds,
            valid: sample.valid !== false,
          }]
        : []),
  }
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
    <ColorPicker
      label="Positive delta"
      color={positiveColor}
      onChange={onPositiveChange}
    />
    <ColorPicker
      label="Negative delta"
      color={negativeColor}
      onChange={onNegativeChange}
    />
  </div>
}

const AnalyzeChartSubscriber = memo(function AnalyzeChartSubscriber({
  isDark, selected, deltaPositiveColor, deltaNegativeColor,
  currentLapNum, comparison, comparisonSelected, currentLabel, comparisonLabel, fixedMode, primaryOverride, distanceMode,
  analysisView, syncedTooltip, sectorBoundaries, sectorDelta,
  deltaData, graphControlsRef, stackedControlsRef, onInspectMap,
}: {
  isDark: boolean
  selected: AnalyzeSeriesConfig[]
  deltaPositiveColor: string
  deltaNegativeColor: string
  currentLapNum: number | null
  comparison: AnalyzeLapData | null
  comparisonSelected: boolean
  currentLabel: string
  comparisonLabel: string
  fixedMode: boolean
  primaryOverride: AnalyzeLapData | null
  distanceMode: boolean
  analysisView: 'graph' | 'charts' | 'map'
  syncedTooltip: boolean
  sectorBoundaries: boolean
  sectorDelta: boolean
  deltaData: AnalyzeDeltaData | null
  graphControlsRef: MutableRefObject<AnalyzeChartControls | null>
  stackedControlsRef: MutableRefObject<AnalyzeChartControls | null>
  onInspectMap?: (elapsedSeconds: number) => void
}) {
  const stackedPresence = useModalPresence(analysisView === 'charts', ANALYSIS_PRESENCE_DURATION, {
    animateInitialEnter: false,
  })
  const playbackCurrentLap = useTelemetryStore(s =>
    !fixedMode && currentLapNum !== null && s.speedRpmBlocks !== null
      ? s.playbackLapDataCache[currentLapNum] ?? null
      : null)
  const playbackActive = useTelemetryStore(s => s.speedRpmBlocks !== null)
  const cachedMask = playbackCurrentLap?.rowTypeMask ?? 0
  const telemetry = useTelemetryStore(s =>
    fixedMode || (cachedMask & DATA_ROW.telemetry) !== 0 ? EMPTY_ROWS : s.analyzeLapTelemetry)
  const motion = useTelemetryStore(s =>
    fixedMode || (cachedMask & DATA_ROW.motion) !== 0 ? EMPTY_ROWS : s.analyzeLapMotion)
  const motionEx = useTelemetryStore(s =>
    fixedMode || (cachedMask & DATA_ROW.motionEx) !== 0 ? EMPTY_ROWS : s.analyzeLapMotionEx)
  const statusHistory = useTelemetryStore(s =>
    fixedMode || (cachedMask & DATA_ROW.status) !== 0 ? EMPTY_ROWS : s.analyzeLapStatusHistory)
  const damageHistory = useTelemetryStore(s =>
    fixedMode || (cachedMask & DATA_ROW.damage) !== 0 ? EMPTY_ROWS : s.analyzeLapDamageHistory)
  const lapProgress = useTelemetryStore(s =>
    fixedMode || (cachedMask & DATA_ROW.lap) !== 0 ? EMPTY_ROWS : s.analyzeLapProgress)
  const startSessionTime = useTelemetryStore(s =>
    fixedMode || playbackCurrentLap !== null ? 0 : s.analyzeLapStartTime)
  const liveRevision = useTelemetryStore(s => fixedMode ? 0 : s.analyzeLapRevision)
  const trackLengthM = useTelemetryStore(s => s.analyzeTrackLengthM)

  const liveCurrent = useMemo<AnalyzeLapData>(() => {
    // Playback Analysis already requests the current lap through the native
    // indexed lap cache. Consume each installed family directly from that
    // cache instead of relying on transient active-tab streaming slices: those
    // slices are deliberately discarded while Analysis is hidden and can be
    // incomplete for a frame (or indefinitely when their old coverage marker
    // survives a tab switch). Live sessions continue using the rolling slices.
    const currentTelemetry = playbackCurrentLap && (cachedMask & DATA_ROW.telemetry) ? playbackCurrentLap.telemetry : telemetry
    const currentMotion = playbackCurrentLap && (cachedMask & DATA_ROW.motion) ? playbackCurrentLap.motion : motion
    const currentMotionEx = playbackCurrentLap && (cachedMask & DATA_ROW.motionEx) ? playbackCurrentLap.motionEx : motionEx
    const currentStatus = playbackCurrentLap && (cachedMask & DATA_ROW.status) ? playbackCurrentLap.statusHistory : statusHistory
    const currentDamage = playbackCurrentLap && (cachedMask & DATA_ROW.damage) ? playbackCurrentLap.damageHistory : damageHistory
    const currentProgress = playbackCurrentLap && (cachedMask & DATA_ROW.lap) ? playbackCurrentLap.lapProgress : lapProgress
    const ends = [currentTelemetry, currentMotion, currentMotionEx, currentStatus, currentDamage]
      .flatMap(rows => rows.length ? [rows[rows.length - 1].session_time] : [])
    return {
      lapNum: currentLapNum ?? 0,
      startSessionTime: playbackCurrentLap?.startSessionTime ?? startSessionTime,
      endSessionTime: playbackCurrentLap?.endSessionTime ?? (ends.length ? Math.max(...ends) : startSessionTime),
      telemetry: currentTelemetry,
      motion: currentMotion,
      motionEx: currentMotionEx,
      statusHistory: currentStatus,
      damageHistory: currentDamage,
      lapProgress: currentProgress,
      playerPositions: playbackCurrentLap?.playerPositions ?? [],
      rowTypeMask: cachedMask,
    }
  }, [cachedMask, currentLapNum, damageHistory, lapProgress, motion, motionEx, playbackCurrentLap, startSessionTime, statusHistory, telemetry])
  const current = fixedMode ? primaryOverride ?? EMPTY_ANALYZE_LAP : liveCurrent
  const currentRevision = fixedMode
    ? `fixed:${current.lapNum}:${current.startSessionTime}:${current.endSessionTime}`
    : `${liveRevision}:${current.lapNum}:${playbackCurrentLap
      ? `cached:${playbackCurrentLap.startSessionTime}:${playbackCurrentLap.rowTypeMask ?? 0}`
      : 'stream'}`

  const chartProps = {
    isDark, current, currentRevision, comparison, comparisonSelected, selected,
    primaryLabel: escapeTooltipText(`${currentLabel} · L${current.lapNum || '—'}`),
    comparisonLabel: comparison ? escapeTooltipText(`${comparisonLabel} · L${comparison.lapNum}`) : undefined,
    distanceMode, trackLengthM, deltaPositiveColor, deltaNegativeColor,
    zoomEnabled: fixedMode && primaryOverride !== null,
    // The current playback lap remains cursor-clipped even during the brief
    // rollover interval before its indexed payload reaches the store. Tying
    // this to cache presence reveals the complete native delta curve whenever
    // a minimized renderer wakes on a new lap.
    realtimeCurrent: !fixedMode && playbackActive && currentLapNum !== null,
    deltaData,
    syncedTooltip, sectorBoundaries, sectorDelta,
    onInspectMap,
  }

  return <div className="absolute inset-0">
    <div
      className={`analysis-chart-mode-surface absolute inset-0 ${stackedPresence.visible ? '' : 'analysis-chart-mode-surface--visible'}`}
      aria-hidden={stackedPresence.visible}
      inert={stackedPresence.visible}
    >
      <AnalyzeTimeChart {...chartProps} controlsRef={graphControlsRef} tooltipEnabled={analysisView === 'graph'} />
    </div>
    {stackedPresence.mounted && <div
      className={`analysis-chart-mode-surface absolute inset-0 ${stackedPresence.visible ? 'analysis-chart-mode-surface--visible' : ''}`}
      aria-hidden={!stackedPresence.visible}
      inert={!stackedPresence.visible}
    >
      <AnalyzeStackedTimeCharts {...chartProps} controlsRef={stackedControlsRef} tooltipEnabled={stackedPresence.visible} />
    </div>}
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
  const primaryView = !playbackFilename && config.view !== 'graph' ? 'graph' : config.view
  const splitView = primaryView === 'split'
  const chartsVisible = primaryView !== 'map'
  const mapVisible = primaryView !== 'graph'
  const analysisView = primaryView === 'map' ? 'map' : config.individualGraphs ? 'charts' : 'graph'
  const chartAnalysisView = config.individualGraphs ? 'charts' : 'graph'
  const mapPresence = useModalPresence(mapVisible, ANALYSIS_PRESENCE_DURATION, {
    animateInitialEnter: false,
  })
  const dataMask = useMemo(
    () => dataMaskForAnalyze(splitView ? 'split' : analysisView, config.series),
    [analysisView, config.series, splitView],
  )
  useLayoutEffect(() => onDataMaskChange(dataMask), [dataMask, onDataMaskChange])
  const allAxesEnabled = config.series.every(item => item.showYAxis)
  const blocks = useTelemetryStore(s => s.speedRpmBlocks) as LapBlock[] | null
  const lapCache = useTelemetryStore(s => s.playbackLapDataCache)
  const liveLap = useTelemetryStore(s => s.lap)
  const liveLapNum = liveLap?.lap_num ?? null
  const fastestLapNum = useTelemetryStore(s => s.fastestLapNum)
  const lapTimesByNum = useTelemetryStore(s => s.lapTimesByNum)
  const deltaAvailable = useTelemetryStore(s => s.analyzeDeltaAvailable)
  const primaryTrackId = useTelemetryStore(s => s.playbackTrackId)
  const primaryTrackName = useTelemetryStore(s => s.playbackTrackName)
  const { tn } = useLabels()
  const effectiveCurrentLapNum = currentLapNum ?? liveLapNum
  const resolvedCurrentLabel = config.currentLabel.trim() || DEFAULT_CURRENT_LABEL
  const resolvedCompareLabel = config.compareLabel.trim() || DEFAULT_COMPARE_LABEL
  const resolvedLapALabel = config.lapALabel.trim() || DEFAULT_LAP_A_LABEL
  const resolvedLapBLabel = config.lapBLabel.trim() || DEFAULT_LAP_B_LABEL
  const primaryLabel = fixedLapMode.enabled ? resolvedLapALabel : resolvedCurrentLabel
  const comparisonLabel = fixedLapMode.enabled ? resolvedLapBLabel : resolvedCompareLabel
  const [draggedMetric, setDraggedMetric] = useState<string | null>(null)
  const [secondaryFile, setSecondaryFile] = useState<SecondaryFileData | null>(null)
  const [secondaryLapNum, setSecondaryLapNum] = useState<number | null>(null)
  const [lapASource, setLapASource] = useState<AnalysisFileSource>('file1')
  const [lapBSource, setLapBSource] = useState<AnalysisFileSource>('file1')
  const [secondaryLapCache, setSecondaryLapCache] = useState<Record<number, AnalyzeLapData>>({})
  const [deltaData, setDeltaData] = useState<AnalyzeDeltaData | null>(null)
  const [secondaryLoading, setSecondaryLoading] = useState(false)
  const [secondaryError, setSecondaryError] = useState<string | null>(null)
  const [controlsHelpOpen, setControlsHelpOpen] = useState(false)
  const controlsHelpPresence = useModalPresence(controlsHelpOpen)
  const [pendingCircuitMismatch, setPendingCircuitMismatch] = useState<PendingCircuitMismatch | null>(null)
  const circuitMismatchPresence = useModalPresenceValue(pendingCircuitMismatch)
  const displayedCircuitMismatch = circuitMismatchPresence.value
  const [mapFocus, setMapFocus] = useState<AnalyzeMapFocus | null>(null)
  const mapFocusIdRef = useRef(0)
  const activeAnalysisViewTransitionRef = useRef<AnalysisViewTransition | null>(null)
  const requestedRef = useRef(new Map<number, number>())
  const secondaryRequestedRef = useRef(new Map<number, number>())
  const graphControlsRef = useRef<AnalyzeChartControls | null>(null)
  const stackedControlsRef = useRef<AnalyzeChartControls | null>(null)
  const seriesRowRefs = useRef(new Map<string, HTMLDivElement>())
  const seriesLayoutBeforeUpdateRef = useRef<Map<string, DOMRect> | null>(null)
  const seriesLayoutReadyRef = useRef(false)
  const sidebarRef = useRef<HTMLElement>(null)
  const previousCollapsedRef = useRef(config.collapsed)
  const selectStyles = useMemo(() => buildSelectStyles(isDark, {
    solidBg: true,
    controlHeight: 32,
    labelStyleGroupHeadings: true,
  }), [isDark])
  const lapSelectStyles = useMemo(() => buildSelectStyles(isDark, {
    solidBg: true,
    controlHeight: 32,
    labelStyleGroupHeadings: true,
    menuWidth: 'calc(100% + 50px)',
  }), [isDark])

  const save = useCallback((next: AnalyzeConfig) => setRawConfig(next), [setRawConfig])
  const setAnalysisView = useCallback((nextView: AnalyzeConfig['view']) => {
    if (nextView === primaryView) {
      if (nextView !== config.view) save({ ...config, view: nextView })
      return
    }

    const transitionDocument = document as Document & {
      startViewTransition?: (update: () => void) => AnalysisViewTransition
    }
    const root = document.documentElement

    if (reduceAnimations || !transitionDocument.startViewTransition) {
      activeAnalysisViewTransitionRef.current?.skipTransition?.()
      activeAnalysisViewTransitionRef.current = null
      delete root.dataset.analysisViewTransition
      delete root.dataset.analysisViewTransitionPhase
      save({ ...config, view: nextView })
      return
    }

    activeAnalysisViewTransitionRef.current?.skipTransition?.()
    root.dataset.analysisViewTransition = 'true'
    root.dataset.analysisViewTransitionPhase = 'preparing'
    try {
      const transition = transitionDocument.startViewTransition(() => {
        flushSync(() => save({ ...config, view: nextView }))
      })
      activeAnalysisViewTransitionRef.current = transition
      void transition.ready.then(() => {
        requestAnimationFrame(() => requestAnimationFrame(() => {
          if (activeAnalysisViewTransitionRef.current !== transition) return
          root.dataset.analysisViewTransitionPhase = 'running'
        }))
      }, () => {})
      const clearTransition = () => {
        if (activeAnalysisViewTransitionRef.current !== transition) return
        activeAnalysisViewTransitionRef.current = null
        delete root.dataset.analysisViewTransition
        delete root.dataset.analysisViewTransitionPhase
      }
      void transition.finished.then(clearTransition, clearTransition)
    } catch {
      activeAnalysisViewTransitionRef.current = null
      delete root.dataset.analysisViewTransition
      delete root.dataset.analysisViewTransitionPhase
      save({ ...config, view: nextView })
    }
  }, [config, primaryView, reduceAnimations, save])
  const updateSeries = useCallback((series: AnalyzeSeriesConfig[]) => {
    if (!reduceAnimations) {
      seriesLayoutBeforeUpdateRef.current = new Map(
        [...seriesRowRefs.current].map(([id, node]) => [id, node.getBoundingClientRect()]),
      )
    }
    save({ ...config, series })
  }, [config, reduceAnimations, save])

  useLayoutEffect(() => {
    if (!seriesLayoutReadyRef.current) {
      seriesLayoutReadyRef.current = true
      seriesLayoutBeforeUpdateRef.current = null
      return
    }
    const previous = seriesLayoutBeforeUpdateRef.current
    seriesLayoutBeforeUpdateRef.current = null
    if (reduceAnimations || !previous) return
    for (const [id, node] of seriesRowRefs.current) {
      const before = previous.get(id)
      if (!before) {
        node.animate([
          { opacity: 0, transform: 'translateY(4px)' },
          { opacity: 1, transform: 'translateY(0)' },
        ], { duration: ANALYSIS_MOTION_DURATION, easing: ANALYSIS_MOTION_EASING })
        continue
      }
      const after = node.getBoundingClientRect()
      const deltaX = before.left - after.left
      const deltaY = before.top - after.top
      if (Math.abs(deltaX) < 0.5 && Math.abs(deltaY) < 0.5) continue
      node.animate([
        { transform: `translate(${deltaX}px, ${deltaY}px)` },
        { transform: 'translate(0, 0)' },
      ], { duration: ANALYSIS_MOTION_DURATION, easing: ANALYSIS_MOTION_EASING })
    }
  }, [config.series, reduceAnimations])
  const inspectMapAt = useCallback((elapsedSeconds: number) => {
    if (!playbackFilename) return
    setMapFocus({ id: ++mapFocusIdRef.current, elapsedSeconds })
    if (config.view === 'graph') setAnalysisView('split')
  }, [config.view, playbackFilename, setAnalysisView])


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
  const currentLapValue = useMemo<ComparisonLapOption | null>(() => {
    if (effectiveCurrentLapNum === null) return null
    const existing = file1CompareOptions.find(option => option.lapNum === effectiveCurrentLapNum)
    const elapsedMs = liveLap?.lap_num === effectiveCurrentLapNum ? liveLap.current_lap_ms : undefined
    const lapTime = existing?.lapTime ?? formatLapTimeMs(elapsedMs)
    return {
      value: `current:${effectiveCurrentLapNum}`,
      lapNum: effectiveCurrentLapNum,
      label: existing?.label ?? `Lap ${effectiveCurrentLapNum}${lapTime ? ` · ${lapTime}` : ''}`,
      compound: existing?.compound ?? null,
      compoundColor: existing?.compoundColor ?? null,
      lapTime,
      isFastest: existing?.isFastest ?? false,
    }
  }, [effectiveCurrentLapNum, file1CompareOptions, liveLap?.current_lap_ms, liveLap?.lap_num])
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
  const mismatchedFiles = primaryTrackId !== null && secondaryFile !== null &&
    secondaryFile.trackId !== null && primaryTrackId !== secondaryFile.trackId
  const sectorBoundariesEnabled = !mismatchedFiles && config.sectorBoundaries
  const sectorDeltaEnabled = sectorBoundariesEnabled && config.sectorDelta
  const selectedDistanceMode = deltaAvailable && (fixedLapMode.enabled
    ? (lapASource === 'file1' || secondaryFile?.deltaAvailable === true) &&
      (lapBSource === 'file1' || secondaryFile?.deltaAvailable === true)
    : secondaryLapNum === null || secondaryFile?.deltaAvailable === true)
  const comparisonSelected = fixedLapMode.enabled
    ? fixedLapMode.lapA !== null && fixedLapMode.lapB !== null
    : compareLapNum !== null || secondaryLapNum !== null
  const deltaCurrentLapNum = fixedLapMode.enabled ? fixedLapMode.lapA : effectiveCurrentLapNum
  const deltaComparisonLapNum = fixedLapMode.enabled
    ? fixedLapMode.lapB
    : secondaryLapNum ?? compareLapNum
  const deltaCurrentSource: AnalysisFileSource = fixedLapMode.enabled ? lapASource : 'file1'
  const deltaComparisonSource: AnalysisFileSource = fixedLapMode.enabled
    ? lapBSource
    : secondaryLapNum !== null ? 'file2' : 'file1'

  useEffect(() => {
    let cancelled = false
    if (!playbackFilename || !selectedDistanceMode ||
        deltaCurrentLapNum === null || deltaComparisonLapNum === null) {
      setDeltaData(null)
      return () => { cancelled = true }
    }
    setDeltaData(null)
    void window.analysisBridge.compareLaps(
      deltaCurrentLapNum,
      deltaCurrentSource,
      deltaComparisonLapNum,
      deltaComparisonSource,
      sectorDeltaEnabled,
    ).then(payload => {
      if (!cancelled) setDeltaData(parseAnalyzeDeltaData(payload))
    }).catch(() => {
      if (!cancelled) setDeltaData(null)
    })
    return () => { cancelled = true }
  }, [
    deltaComparisonLapNum, deltaComparisonSource, deltaCurrentLapNum,
    deltaCurrentSource, playbackFilename, secondaryFile,
    sectorDeltaEnabled, selectedDistanceMode,
  ])

  const displayedDeltaData = deltaData?.sectorDelta === sectorDeltaEnabled ? deltaData : null
  const deltaSeries = config.series.find(item => item.metricId === 'delta')
  const deltaPositiveColor = deltaSeries?.color ?? DEFAULT_DELTA_POSITIVE_COLOR
  const deltaNegativeColor = deltaSeries?.negativeColor ?? DEFAULT_DELTA_NEGATIVE_COLOR

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
    if (reduceAnimations || startWidth === targetWidth) {
      sidebar.style.width = `${targetWidth}px`
      return
    }

    const duration = ANALYSIS_MOTION_DURATION
    const startedAt = performance.now()
    let animationFrame = 0
    const animate = (now: number) => {
      const progress = Math.min(1, (now - startedAt) / duration)
      const eased = easeInOutCubic(progress)
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

  const removeMetric = useCallback((metricId: string) => {
    updateSeries(config.series.filter(entry => entry.metricId !== metricId))
  }, [config.series, updateSeries])

  const activeChartControlsRef = analysisView === 'charts' ? stackedControlsRef : graphControlsRef

  return (
    <div className="h-full flex overflow-hidden border-t border-[var(--border)] bg-[var(--bg-panel)]">
      <aside
        ref={sidebarRef}
        className={`${config.collapsed ? 'border-r-0' : 'border-r'} shrink-0 border-[var(--border)] overflow-hidden bg-[var(--bg-panel)]`}
      >
          <div className={`w-[315px] h-full flex flex-col transition-[visibility] duration-0 ${config.collapsed ? 'invisible delay-200' : 'visible delay-0'}`}>
            <div className="h-11 px-3 flex items-center gap-3 border-b border-[var(--border)] shrink-0">
              <div className="text-[11px] font-bold uppercase tracking-widest text-[var(--text-primary)] shrink-0">Analysis</div>
              <div className="flex-1 min-w-0">
                <Select<SelectOption, false>
                  inputId="analyze-add-metric" aria-label="Add a Metric" value={null} options={metricOptions}
                  onChange={addMetric} placeholder="Add a Metric" styles={selectStyles}
                  components={selectComponents} isSearchable menuPortalTarget={document.body}
                />
              </div>
            </div>

            <div className="p-3 border-b border-[var(--border)] space-y-3 shrink-0">
              <div className="flex gap-2">
                <button
                  type="button" aria-label="Graphs" aria-pressed={primaryView === 'graph'} title="Graphs"
                  onClick={() => {
                    setMapFocus(null)
                    setAnalysisView('graph')
                  }}
                  className={`${ANALYZE_TOGGLE_BUTTON_CLASS} ${primaryView === 'graph' ? 'analyze-toggle-button--active' : ''}`}
                ><LineChart size={15} /></button>
                <button
                  type="button" aria-label="Split Mode" aria-pressed={primaryView === 'split'} title="Split Mode"
                  disabled={!playbackFilename || mismatchedFiles}
                  onClick={() => {
                    setMapFocus(null)
                    setAnalysisView('split')
                  }}
                  className={`${ANALYZE_TOGGLE_BUTTON_CLASS} ${primaryView === 'split' ? 'analyze-toggle-button--active' : ''}`}
                ><Columns2 size={15} /></button>
                <button
                  type="button" aria-label="Map" aria-pressed={primaryView === 'map'} title="Map"
                  disabled={!playbackFilename || mismatchedFiles}
                  onClick={() => {
                    setMapFocus(null)
                    setAnalysisView('map')
                  }}
                  className={`${ANALYZE_TOGGLE_BUTTON_CLASS} ${primaryView === 'map' ? 'analyze-toggle-button--active' : ''}`}
                ><MapIcon size={15} /></button>
              </div>
              <div className="flex gap-2">
                <button
                  type="button" aria-label="Individual Graphs" aria-pressed={config.individualGraphs} title="Individual Graphs"
                  onClick={() => save({ ...config, individualGraphs: !config.individualGraphs })}
                  className={`${ANALYZE_TOGGLE_BUTTON_CLASS} ${config.individualGraphs ? 'analyze-toggle-button--active' : ''}`}
                ><Rows3 size={15} /></button>
                <button
                  type="button" aria-label="Synced Tooltip" aria-pressed={config.syncedTooltip} title="Synced Tooltip" disabled={!config.individualGraphs}
                  onClick={() => save({ ...config, syncedTooltip: !config.syncedTooltip })}
                  className={`${ANALYZE_TOGGLE_BUTTON_CLASS} ${config.syncedTooltip ? 'analyze-toggle-button--active' : ''}`}
                ><SyncedTooltipIcon size={15} /></button>
                <button
                  type="button" aria-label="Sector Boundaries" aria-pressed={config.sectorBoundaries} title="Sector Boundaries" disabled={mismatchedFiles}
                  onClick={() => save({ ...config, sectorBoundaries: !config.sectorBoundaries })}
                  className={`${ANALYZE_TOGGLE_BUTTON_CLASS} ${config.sectorBoundaries ? 'analyze-toggle-button--active' : ''}`}
                ><Columns3 size={15} /></button>
                <button
                  type="button" aria-label="Sector Delta" aria-pressed={config.sectorDelta} title="Sector Delta" disabled={mismatchedFiles || !config.sectorBoundaries}
                  onClick={() => save({ ...config, sectorDelta: !config.sectorDelta })}
                  className={`${ANALYZE_TOGGLE_BUTTON_CLASS} ${config.sectorDelta ? 'analyze-toggle-button--active' : ''}`}
                ><ChartNoAxesCombined size={15} /></button>
                <button
                  type="button" aria-label="Comparison" aria-pressed={fixedLapMode.enabled} title="Comparison"
                  disabled={!playbackFilename || !blocks}
                  onClick={() => onFixedLapModeChange({ ...fixedLapMode, enabled: !fixedLapMode.enabled })}
                  className={`${ANALYZE_TOGGLE_BUTTON_CLASS} ${fixedLapMode.enabled ? 'analyze-toggle-button--active' : ''}`}
                ><ListChevronsUpDown size={15} /></button>
              </div>
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
              <div className="analyze-lap-mode-switch">
                <div
                  className={`analyze-lap-mode-panel ${fixedLapMode.enabled ? 'analyze-lap-mode-panel--visible' : 'analyze-lap-mode-panel--hidden-left'}`}
                  aria-hidden={!fixedLapMode.enabled}
                  inert={!fixedLapMode.enabled}
                >
                  <AnalyzeComparisonSelector
                    id="analyze-lap-a" label="Lap A" value={lapAValue} options={compareOptions} placeholder="Select Lap A…"
                    onChange={option => {
                      setLapASource(option?.value.startsWith('file2:') ? 'file2' : 'file1')
                      onFixedLapModeChange({ ...fixedLapMode, lapA: option?.lapNum ?? null })
                    }}
                    styles={lapSelectStyles}
                    showColorPicker={mapVisible}
                    colorPicker={<ColorPicker
                      label="Lap A"
                      color={config.mapCurrentColor}
                      onChange={mapCurrentColor => save({ ...config, mapCurrentColor })}
                    />}
                  />
                  <AnalyzeComparisonSelector
                    id="analyze-lap-b" label="Lap B" value={lapBValue} options={compareOptions} placeholder="Select Lap B…"
                    onChange={option => {
                      setLapBSource(option?.value.startsWith('file2:') ? 'file2' : 'file1')
                      onFixedLapModeChange({ ...fixedLapMode, lapB: option?.lapNum ?? null })
                    }}
                    styles={lapSelectStyles}
                    showColorPicker={mapVisible}
                    colorPicker={<ColorPicker
                      label="Lap B"
                      color={config.mapComparisonColor}
                      onChange={mapComparisonColor => save({ ...config, mapComparisonColor })}
                    />}
                  />
                </div>
                <div
                  className={`analyze-lap-mode-panel ${!fixedLapMode.enabled ? 'analyze-lap-mode-panel--visible' : 'analyze-lap-mode-panel--hidden-right'}`}
                  aria-hidden={fixedLapMode.enabled}
                  inert={fixedLapMode.enabled}
                >
                  <AnalyzeComparisonSelector
                    id="analyze-current-lap" label="Current" placeholder="No current lap"
                    value={currentLapValue} options={[]} onChange={() => {}} styles={lapSelectStyles} displayOnly
                    showColorPicker={mapVisible}
                    colorPicker={<ColorPicker
                      label="Current"
                      color={config.mapCurrentColor}
                      onChange={mapCurrentColor => save({ ...config, mapCurrentColor })}
                    />}
                  />
                  <AnalyzeComparisonSelector
                    id="analyze-compare-lap" label="Compare" placeholder="Select lap…"
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
                    styles={lapSelectStyles} isDisabled={!playbackFilename || !blocks}
                    showColorPicker={mapVisible}
                    colorPicker={<ColorPicker
                      label="Compare"
                      color={config.mapComparisonColor}
                      onChange={mapComparisonColor => save({ ...config, mapComparisonColor })}
                    />}
                  />
                </div>
              </div>
              <div className="analyze-lap-mode-switch">
                <div
                  className={`analyze-lap-mode-panel ${fixedLapMode.enabled ? 'analyze-lap-mode-panel--visible' : 'analyze-lap-mode-panel--hidden-left'}`}
                  aria-hidden={!fixedLapMode.enabled}
                  inert={!fixedLapMode.enabled}
                >
                  <AnalyzeLabelInput
                    id="analyze-lap-a-label" label="Lap A" value={config.lapALabel}
                    onChange={lapALabel => save({ ...config, lapALabel })}
                  />
                  <AnalyzeLabelInput
                    id="analyze-lap-b-label" label="Lap B" value={config.lapBLabel}
                    onChange={lapBLabel => save({ ...config, lapBLabel })}
                  />
                </div>
                <div
                  className={`analyze-lap-mode-panel ${!fixedLapMode.enabled ? 'analyze-lap-mode-panel--visible' : 'analyze-lap-mode-panel--hidden-right'}`}
                  aria-hidden={fixedLapMode.enabled}
                  inert={fixedLapMode.enabled}
                >
                  <AnalyzeLabelInput
                    id="analyze-current-label" label="Current" value={config.currentLabel}
                    onChange={currentLabel => save({ ...config, currentLabel })}
                  />
                  <AnalyzeLabelInput
                    id="analyze-compare-label" label="Compare" value={config.compareLabel}
                    onChange={compareLabel => save({ ...config, compareLabel })}
                  />
                </div>
              </div>
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
                    ref={node => { if (node) seriesRowRefs.current.set('delta', node); else seriesRowRefs.current.delete('delta') }}
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
                    ref={node => { if (node) seriesRowRefs.current.set(item.metricId, node); else seriesRowRefs.current.delete(item.metricId) }}
                    key={item.metricId} draggable onDragStart={() => setDraggedMetric(item.metricId)} onDragEnd={() => setDraggedMetric(null)}
                    onDragOver={event => event.preventDefault()} onDrop={() => dropMetric(item.metricId)}
                    className={`flex items-center gap-1.5 px-1.5 py-1.5 rounded border border-transparent hover:border-[var(--border)] hover:bg-[var(--bg-hover)] ${draggedMetric === item.metricId ? 'opacity-40' : ''}`}
                  >
                    <GripVertical size={13} className="text-[var(--text-secondary)] cursor-grab shrink-0" />
                    <ColorPicker
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
                      <button title="Remove metric" onClick={() => void removeMetric(item.metricId)} className="p-1 text-[var(--text-secondary)] hover:text-[#d44252]"><Trash2 size={11} /></button>
                    </div>
                  </div>
                )
              })}
            </div>
          </div>
      </aside>

      <section className="analysis-view-page-transition flex-1 min-w-0 flex flex-col">
        <div className="h-11 px-2 border-b border-[var(--border)] flex items-center gap-1 shrink-0">
          <button
            title={config.collapsed ? 'Open Analysis controls' : 'Collapse Analysis controls'}
            aria-label={config.collapsed ? 'Open Analysis controls' : 'Collapse Analysis controls'}
            onClick={() => save({ ...config, collapsed: !config.collapsed })}
            className="w-7 h-7 rounded flex items-center justify-center shrink-0 text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]"
          >
            {config.collapsed ? <PanelLeftOpen size={15} /> : <PanelLeftClose size={15} />}
          </button>
          {analysisView !== 'map' && <>
            <span className="h-5 w-px mx-1 bg-[var(--border)]" />
            <span className="px-1 text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">Zoom</span>
            <button disabled={!fixedLapMode.enabled || !fixedPrimary} title="Zoom out" onClick={() => activeChartControlsRef.current?.zoomOut()} className="w-7 h-7 rounded flex items-center justify-center text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-25 disabled:pointer-events-none"><ZoomOut size={14} /></button>
            <button disabled={!fixedLapMode.enabled || !fixedPrimary} title="Zoom in" onClick={() => activeChartControlsRef.current?.zoomIn()} className="w-7 h-7 rounded flex items-center justify-center text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-25 disabled:pointer-events-none"><ZoomIn size={14} /></button>
            <span className="h-5 w-px mx-1 bg-[var(--border)]" />
            <span className="px-1 text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">Pan</span>
            <button disabled={!fixedLapMode.enabled || !fixedPrimary} title="Pan left" onClick={() => activeChartControlsRef.current?.panLeft()} className="w-7 h-7 rounded flex items-center justify-center text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-25 disabled:pointer-events-none"><ArrowLeft size={14} /></button>
            <button disabled={!fixedLapMode.enabled || !fixedPrimary} title="Pan right" onClick={() => activeChartControlsRef.current?.panRight()} className="w-7 h-7 rounded flex items-center justify-center text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-25 disabled:pointer-events-none"><ArrowRight size={14} /></button>
            <button disabled={!fixedLapMode.enabled || !fixedPrimary} title="Reset zoom" onClick={() => activeChartControlsRef.current?.reset()} className="w-7 h-7 rounded flex items-center justify-center text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-25 disabled:pointer-events-none"><RotateCcw size={13} /></button>
          </>}
          <div className="ml-auto flex shrink-0 items-center gap-3 pr-1">
            <AnalysisDeltaReadout
              deltaData={displayedDeltaData}
              current={current}
              comparison={comparison}
              positiveColor={deltaPositiveColor}
              negativeColor={deltaNegativeColor}
              followPlaybackCursor={!!playbackFilename && !fixedLapMode.enabled}
            />
            <button
              type="button"
              title="Analysis controls help"
              aria-label="Open Analysis controls help"
              onClick={() => setControlsHelpOpen(true)}
              className="w-7 h-7 rounded flex items-center justify-center shrink-0 text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]"
            >
              <CircleHelp size={15} />
            </button>
          </div>
        </div>
        <div className="flex-1 min-h-0 relative flex">
          <div
            className={`analysis-view-surface ${splitView ? 'relative basis-1/2 min-w-0 overflow-hidden border-r border-[var(--border)]' : 'absolute inset-0'} ${chartsVisible ? 'analysis-view-surface--visible' : ''}`}
            data-analysis-active={chartsVisible}
            aria-hidden={!chartsVisible}
            inert={!chartsVisible}
          >
            <AnalyzeChartSubscriber
              isDark={isDark} selected={config.series}
              deltaPositiveColor={deltaPositiveColor}
              deltaNegativeColor={deltaNegativeColor}
              currentLapNum={fixedLapMode.enabled ? fixedLapMode.lapA : effectiveCurrentLapNum}
              comparison={comparison} comparisonSelected={comparisonSelected}
              currentLabel={primaryLabel} comparisonLabel={comparisonLabel}
              fixedMode={fixedLapMode.enabled} primaryOverride={fixedPrimary}
              distanceMode={selectedDistanceMode}
              analysisView={chartAnalysisView}
              syncedTooltip={config.syncedTooltip}
              sectorBoundaries={sectorBoundariesEnabled}
              sectorDelta={sectorDeltaEnabled}
              deltaData={deltaData}
              graphControlsRef={graphControlsRef}
              stackedControlsRef={stackedControlsRef}
              onInspectMap={playbackFilename && !mismatchedFiles ? inspectMapAt : undefined}
            />
          </div>
          {mapPresence.mounted && <div
            className={`analysis-view-surface ${splitView ? 'relative basis-1/2 min-w-0 overflow-hidden' : 'absolute inset-0'} ${mapPresence.visible ? 'analysis-view-surface--visible' : ''}`}
            data-analysis-active={mapVisible}
            aria-hidden={!mapPresence.visible}
            inert={!mapPresence.visible}
          >
            <AnalyzeMapComparison
              current={current}
              comparison={comparison}
              currentColor={config.mapCurrentColor}
              currentLabel={primaryLabel}
              comparisonColor={config.mapComparisonColor}
              comparisonLabel={comparisonLabel}
              fixedMode={fixedLapMode.enabled}
              trackId={mapTrackId}
              compatibleCircuit={compatibleMapCircuit}
              isDark={isDark}
              sectorColors={sectorColors}
              reduceAnimations={reduceAnimations}
              mapDimmed={mapDimmed}
              focus={mapFocus}
            />
          </div>}
        </div>
      </section>

      {controlsHelpPresence.mounted && createPortal(<div
        ref={controlsHelpPresence.transitionTargetRef}
        data-state={controlsHelpPresence.visible ? 'open' : 'closed'}
        className="modal-backdrop fixed inset-0 z-[120] flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
        role="dialog" aria-modal="true" aria-labelledby="analysis-controls-help-title"
        onMouseDown={event => {
          if (event.target === event.currentTarget) setControlsHelpOpen(false)
        }}
      >
        <div className="modal-panel flex max-h-[calc(100vh-2rem)] w-[680px] max-w-[calc(100vw-2rem)] flex-col overflow-hidden rounded-xl border border-[var(--border)] bg-[var(--bg-panel)] shadow-[0_0_60px_rgba(0,0,0,0.85)]">
          <div className="flex shrink-0 items-center justify-between border-b border-[var(--border)] px-6 py-4">
            <div>
              <div id="analysis-controls-help-title" className="text-xs font-mono font-bold uppercase tracking-widest text-[var(--text-primary)]">Analysis Controls</div>
              <div className="mt-1 text-[10px] font-mono uppercase tracking-wider text-[var(--text-secondary)]">Views, chart options, and interactions</div>
            </div>
            <button
              type="button" onClick={() => setControlsHelpOpen(false)} aria-label="Close Analysis controls help"
              className="flex h-9 w-9 items-center justify-center rounded-lg text-[var(--text-secondary)] transition-colors hover:text-[#e10600]"
            ><X size={18} /></button>
          </div>

          <div className="min-h-0 overflow-y-auto p-5">
            <div className="mb-2 text-[9px] font-bold uppercase tracking-widest text-[var(--text-secondary)]">Sidebar controls</div>
            <div className="grid grid-cols-2 gap-2">
              <AnalysisHelpItem icon={<LineChart size={15} />} label="Graphs">Shows the data in Graphs.</AnalysisHelpItem>
              <AnalysisHelpItem icon={<Columns2 size={15} />} label="Split Mode">Map on left, Graph on right. What more do you want?</AnalysisHelpItem>
              <AnalysisHelpItem icon={<MapIcon size={15} />} label="Map">Shows a map of the laps.</AnalysisHelpItem>
              <AnalysisHelpItem icon={<Rows3 size={15} />} label="Individual Graphs">Instead of cramming all data into one graph, it splits them off into separate graphs.</AnalysisHelpItem>
              <AnalysisHelpItem icon={<SyncedTooltipIcon size={15} />} label="Synced Tooltip">When hovering over the graph, the tooltip will show the data for all visible graphs at that point.</AnalysisHelpItem>
              <AnalysisHelpItem icon={<Columns3 size={15} />} label="Sector Boundaries">Shows Sectors instead of Distance on the X Axis.</AnalysisHelpItem>
              <AnalysisHelpItem icon={<ChartNoAxesCombined size={15} />} label="Sector Delta">Instead of whole lap delta, it splits the delta into sectors.</AnalysisHelpItem>
              <AnalysisHelpItem icon={<ListChevronsUpDown size={15} />} label="Comparison">Compare two laps.</AnalysisHelpItem>
            </div>

            <div className="mb-2 mt-5 text-[9px] font-bold uppercase tracking-widest text-[var(--text-secondary)]">Chart interactions</div>
            <div className="grid grid-cols-2 gap-2">
              <AnalysisHelpItem icon={<ZoomIn size={15} />} label="Zoom">Ctrl + Wheel to Zoom</AnalysisHelpItem>
              <AnalysisHelpItem icon={<ArrowLeft size={15} />} label="Pan">Click and Drag to Pan</AnalysisHelpItem>
              <AnalysisHelpItem icon={<RotateCcw size={15} />} label="Reset">Double Right Click to Reset</AnalysisHelpItem>
              <AnalysisHelpItem icon={<MapIcon size={15} />} label="Map View">Double Left Click to Open in Map view</AnalysisHelpItem>
            </div>
          </div>

          <div className="flex shrink-0 justify-end border-t border-[var(--border)] bg-[var(--bg-card)]/10 px-6 py-4">
            <button type="button" onClick={() => setControlsHelpOpen(false)} className={BUTTON_CLASS}>Close</button>
          </div>
        </div>
      </div>, document.body)}

      {circuitMismatchPresence.mounted && displayedCircuitMismatch && <div
        ref={circuitMismatchPresence.transitionTargetRef}
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
            <button onClick={() => setPendingCircuitMismatch(null)} aria-label="Cancel loading Secondary File" className="w-9 h-9 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#d44252] transition-colors">
              <X size={18} />
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
