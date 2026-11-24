import { memo, useCallback, useEffect, useMemo, useRef, useState, type ChangeEvent } from 'react'
import { type SingleValue } from 'react-select'
import Select from '../../lib/AnimatedSelect'
import { ChevronLeft, ChevronRight, Download, Pause, Play } from 'lucide-react'
import { buildSelectStyles } from '../../lib/selectStyles'
import { selectComponents } from '../../lib/selectComponents'
import { fmtLap } from '../bannerHelpers'
import { PLAYBACK_SPEED_OPTIONS } from '../appConfig'
import { playbackDebug } from '../../lib/playbackDebug'
import type { DensityMode } from '../../lib/graphSections'
import type { PlaybackLapBlock, PlaybackState } from '../../types'
import type { LapSyncedPlayback } from '../hooks/useLapSyncedPlayback'
import { formatMergedClock, type MergedTimeline } from '../../lib/mergedTimeline'

const selectStyles = buildSelectStyles(true)

interface PlaybackBarProps {
  compact: DensityMode | boolean
  currentLapNum?: number | null
  exportError?: string | null
  exportState?: 'idle' | 'busy' | 'error'
  onExport?: () => void
  onSeekProgress: (progress: number) => void
  onSeekBackward: () => void
  onSeekForward: () => void
  onSpeedChange: (speed: number) => void
  onTogglePlay: () => void
  sessionFileStart?: number
  showExport?: boolean
  showLapSelect?: boolean
  speedRpmBlocks?: PlaybackLapBlock[] | null
  state: Pick<PlaybackState, 'currentTime' | 'isPlaying' | 'progressPct' | 'speed' | 'totalTime'>
  /** Two files open: the lap-synced player (Laps 4) replaces the time slider. */
  merged?: LapSyncedPlayback | null
}

type LapOption = { value: number; label: string }

// The playback state ticks every frame, so the bar around these two selects
// re-renders continuously. Keep react-select out of that path: both leaves take
// only primitives plus stable callbacks, so their memo holds while playing.
const SpeedSelect = memo(function SpeedSelect({ className, onChange, speed }: {
  className: string
  onChange: (option: SingleValue<(typeof PLAYBACK_SPEED_OPTIONS)[number]>) => void
  speed: number
}) {
  const value = PLAYBACK_SPEED_OPTIONS.find(option => option.value === speed) ?? PLAYBACK_SPEED_OPTIONS[2]
  return (
    <div className={className}>
      <Select value={value} onChange={onChange} options={PLAYBACK_SPEED_OPTIONS} styles={selectStyles} components={selectComponents} menuPlacement="top" isSearchable={false} />
    </div>
  )
})

const LapSelect = memo(function LapSelect({ className, currentLapNum, onChange, options }: {
  className: string
  currentLapNum: number | null
  onChange: (option: SingleValue<LapOption>) => void
  options: LapOption[]
}) {
  const value = currentLapNum !== null && currentLapNum > 0 ? { value: currentLapNum, label: String(currentLapNum) } : null
  return (
    <div className={className}>
      <Select value={value} options={options} onChange={onChange} isSearchable={false} maxMenuHeight={150} menuPlacement="top" styles={selectStyles} components={selectComponents} placeholder="—" />
    </div>
  )
})

const ProgressTracker = memo(function ProgressTracker({ compact, currentTime, onSeekProgress, progressPct, totalTime }: { compact: DensityMode | boolean; currentTime: number; onSeekProgress: (progress: number) => void; progressPct: number; totalTime: number }) {
  const inputRef = useRef<HTMLInputElement>(null)
  const draggingRef = useRef(false)
  const dragProgressRef = useRef(progressPct)
  const lastSeekRef = useRef(0)
  const [displayPct, setDisplayPct] = useState(progressPct)
  const progressPctRef = useRef(progressPct)
  progressPctRef.current = progressPct
  // While the thumb is not held, the incoming progress is the truth. Mirroring
  // it into state as well would re-render this bar a second time every frame.
  const activePct = draggingRef.current ? displayPct : progressPct
  const startSessionTime = currentTime - progressPct * totalTime
  const displayTime = draggingRef.current ? startSessionTime + displayPct * totalTime : currentTime
  const isCompact = compact === true || compact === 'compact'
  const isSpacious = compact === 'spacious'

  useEffect(() => {
    if (!draggingRef.current && inputRef.current) inputRef.current.value = String(progressPct)
  }, [progressPct])

  const startDrag = useCallback(() => {
    draggingRef.current = true
    dragProgressRef.current = progressPctRef.current
    setDisplayPct(current => current === progressPctRef.current ? current : progressPctRef.current)
  }, [])

  const finishDrag = useCallback(() => {
    if (!draggingRef.current) return
    draggingRef.current = false
    onSeekProgress(dragProgressRef.current)
  }, [onSeekProgress])

  const handleChange = useCallback((event: ChangeEvent<HTMLInputElement>) => {
    const value = parseFloat(event.target.value)
    dragProgressRef.current = value
    setDisplayPct(value)
    const now = Date.now()
    if (now - lastSeekRef.current > 50) {
      lastSeekRef.current = now
      const seekTime = startSessionTime + value * totalTime
      playbackDebug('progress-slider-change', {
        value,
        seekTime,
        currentTime,
        totalTime,
      })
      onSeekProgress(value)
    }
  }, [currentTime, onSeekProgress, startSessionTime, totalTime])

  return (
    <div className={`flex-1 flex items-center ${isCompact ? 'gap-2' : isSpacious ? 'gap-4' : 'gap-4'}`}>
      <span className={`${isSpacious ? 'text-sm font-bold text-[var(--text-primary)]' : 'text-xs text-[var(--text-secondary)]'} font-mono tabular-nums`}>
        {fmtLap(displayTime)}
      </span>
      {isSpacious && totalTime > 0 && (
        <span className="text-xs font-medium font-mono text-[var(--text-secondary)] tabular-nums shrink-0">
          ({Math.round(activePct * 100)}%)
        </span>
      )}
      <input ref={inputRef} type="range" min="0" max="1" step="0.001" defaultValue={progressPct} onMouseDown={startDrag} onMouseUp={finishDrag} onTouchStart={startDrag} onTouchEnd={finishDrag} onChange={handleChange} className={`flex-1 ${isSpacious ? 'h-2' : 'h-1.5'} bg-[var(--border)] rounded-full appearance-none outline-none cursor-pointer [&::-webkit-slider-thumb]:appearance-none [&::-webkit-slider-thumb]:w-3 [&::-webkit-slider-thumb]:h-3 [&::-webkit-slider-thumb]:rounded-full [&::-webkit-slider-thumb]:bg-[var(--color-info)] [&::-webkit-slider-thumb]:cursor-pointer`} style={{ background: `linear-gradient(to right, var(--color-info) ${activePct * 100}%, var(--border) ${activePct * 100}%)` }} />
      <span className={`${isSpacious ? 'text-sm font-bold' : 'text-xs'} font-mono text-[var(--text-secondary)] tabular-nums`}>{fmtLap(totalTime)}</span>
    </div>
  )
})

/**
 * The two-file player's bar (Laps 4): one segment per merged lap, as wide as
 * the merged lap is long, with a 1 px gap between laps. Laps already played
 * are filled, laps to come dark, and a lap one file lacks (or a lap after a
 * file's race has ended) is hatched. The playhead is a dot in file A's blue;
 * clicking or dragging anywhere on the bar seeks the merged timeline.
 */
const PLAYHEAD_RADIUS = 6

const MergedProgressTracker = memo(function MergedProgressTracker({ timeline, time, onSeek }: {
  timeline: MergedTimeline
  time: number
  onSeek: (mergedTime: number) => void
}) {
  const trackRef = useRef<HTMLDivElement>(null)
  const lastSeekRef = useRef(0)
  // Laps are laid out on the screen's own pixel grid: the bar can start at a
  // fractional position in the row, so the drawing is shifted onto a device
  // pixel and every lap edge is a whole device pixel. Each gap is then the
  // same width. The ends keep room for the playhead dot.
  const [layout, setLayout] = useState({ width: 0, shift: 0, dpr: 1 })
  useEffect(() => {
    const track = trackRef.current
    if (!track) return
    const measure = () => {
      const rect = track.getBoundingClientRect()
      const dpr = window.devicePixelRatio || 1
      const shift = (Math.round(rect.left * dpr) - rect.left * dpr) / dpr
      setLayout(current => current.width === rect.width && current.shift === shift && current.dpr === dpr
        ? current : { width: rect.width, shift, dpr })
    }
    measure()
    const observer = new ResizeObserver(measure)
    observer.observe(track)
    window.addEventListener('resize', measure)
    return () => { observer.disconnect(); window.removeEventListener('resize', measure) }
  }, [])
  const { width, shift, dpr } = layout
  // The end margin is a whole number of device pixels too, so lap edges stay on the grid.
  const pad = Math.round((PLAYHEAD_RADIUS + 1) * dpr) / dpr
  const inner = Math.max(0, width - 2 * pad)
  const total = Math.max(timeline.total, 1e-6)
  const devicePixel = 1 / dpr
  // A merged time's x, on a whole device pixel.
  const toX = (merged: number) => pad + Math.round(merged / total * inner * dpr) / dpr
  const gap = Math.max(1, Math.round(dpr)) * devicePixel
  const seekAt = useCallback((clientX: number, force: boolean) => {
    const track = trackRef.current
    if (!track) return
    const rect = track.getBoundingClientRect()
    const padding = PLAYHEAD_RADIUS + 1
    const fraction = Math.max(0, Math.min(1, (clientX - rect.left - padding) / Math.max(rect.width - 2 * padding, 1)))
    const now = Date.now()
    if (!force && now - lastSeekRef.current < 50) return
    lastSeekRef.current = now
    onSeek(fraction * timeline.total)
  }, [onSeek, timeline.total])
  const playedColor = 'color-mix(in srgb, var(--file-a) 30%, var(--bg-panel))'
  const upcomingColor = 'color-mix(in srgb, var(--file-a) 14%, var(--bg-panel))'
  const hatchLine = 'color-mix(in srgb, var(--text-secondary) 45%, var(--bg-panel))'
  const playheadX = pad + Math.min(inner, Math.max(0, time / total * inner))
  return (
    <div ref={trackRef} role="slider" aria-label="Lap-synced timeline" aria-valuemin={0} aria-valuemax={Math.round(timeline.total)} aria-valuenow={Math.round(time)} tabIndex={0}
      className="relative flex-1 min-w-0 h-3.5 cursor-pointer touch-none"
      onPointerDown={event => { event.currentTarget.setPointerCapture(event.pointerId); seekAt(event.clientX, true) }}
      onPointerMove={event => { if (event.currentTarget.hasPointerCapture(event.pointerId)) seekAt(event.clientX, false) }}
      onPointerUp={event => { if (event.currentTarget.hasPointerCapture(event.pointerId)) seekAt(event.clientX, true) }}>
      {width > 0 && <svg width={width} height={14} className="absolute inset-0 block" style={{ transform: `translateX(${shift}px)` }} shapeRendering="crispEdges" aria-hidden="true">
        <defs>
          <pattern id="merged-lap-hatch" width="4" height="4" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
            <line x1="0" y1="0" x2="0" y2="4" stroke={hatchLine} strokeWidth="1.6" />
          </pattern>
        </defs>
        {timeline.segments.map(segment => {
          const x = toX(segment.start)
          const segmentWidth = Math.max(devicePixel, toX(segment.start + segment.duration) - x - gap)
          const oneFile = !segment.a || !segment.b
          const playedWidth = Math.max(0, Math.min(segmentWidth, Math.round(playheadX * dpr) / dpr - x))
          return (
            <g key={`${segment.kind}:${segment.lap_num}:${segment.start}`}>
              <title>{segment.kind === 'lap' ? `Lap ${segment.lap_num}${!segment.b ? ' · not in file B' : !segment.a ? ' · not in file A' : ''}` : segment.kind === 'pre' ? 'Before lap 1' : 'After the race'}</title>
              <rect x={x} y={4} width={segmentWidth} height={6} fill={oneFile ? 'url(#merged-lap-hatch)' : upcomingColor} />
              {!oneFile && playedWidth > 0 && <rect x={x} y={4} width={playedWidth} height={6} fill={playedColor} />}
            </g>
          )
        })}
        <circle cx={playheadX} cy={7} r={PLAYHEAD_RADIUS} fill="var(--file-a)" shapeRendering="geometricPrecision" />
      </svg>}
    </div>
  )
})

/** Time into the merged lap ("0:09.0"); the lap itself is in the lap selector. */
function MergedReadout({ merged, isSpacious }: { merged: LapSyncedPlayback; isSpacious: boolean }) {
  return (
    <span className={`shrink-0 min-w-[3.25rem] whitespace-nowrap font-mono tabular-nums text-[var(--text-secondary)] ${isSpacious ? 'text-sm' : 'text-[12.5px]'}`}>
      {formatMergedClock(merged.position.offset)}
    </span>
  )
}

export default memo(function PlaybackBar({ compact, currentLapNum = null, exportError = null, exportState = 'idle', onExport, onSeekProgress, onSeekBackward, onSeekForward, onSpeedChange, onTogglePlay, sessionFileStart = 0, showExport = true, showLapSelect = true, speedRpmBlocks = null, state, merged = null }: PlaybackBarProps) {
  const isCompact = compact === true || compact === 'compact'
  const isSpacious = compact === 'spacious'
  // Lap 0 is the formation lap — not a seek target.
  const lapOptions = useMemo(() => speedRpmBlocks?.filter(block => block.lapNum > 0).map(block => ({ value: block.lapNum, label: String(block.lapNum) })) ?? [], [speedRpmBlocks])
  const handleSpeed = useCallback((option: SingleValue<(typeof PLAYBACK_SPEED_OPTIONS)[number]>) => { if (option) onSpeedChange(option.value) }, [onSpeedChange])
  // Seeking needs the live clock, but depending on it here would rebuild the
  // handler every frame and defeat the lap select's memo.
  const stateRef = useRef(state)
  stateRef.current = state
  const handleLap = useCallback((option: SingleValue<LapOption>) => {
    const current = stateRef.current
    if (!option || current.totalTime <= 0 || !speedRpmBlocks) {
      playbackDebug('lap-select-rejected', {
        selectedLap: option?.value ?? null,
        totalTime: current.totalTime,
        hasBlocks: Boolean(speedRpmBlocks),
      })
      return
    }
    const block = speedRpmBlocks.find(item => item.lapNum === option.value)
    if (!block) {
      playbackDebug('lap-select-block-missing', {
        selectedLap: option.value,
        availableLaps: speedRpmBlocks.map(item => item.lapNum),
      })
      return
    }
    const ratio = (block.startSessionTime - sessionFileStart) / current.totalTime
    const clampedRatio = Math.max(0, Math.min(1, ratio))
    playbackDebug('lap-select-seek', {
      selectedLap: option.value,
      blockStart: block.startSessionTime,
      blockEnd: block.endSessionTime,
      sessionFileStart,
      currentTime: current.currentTime,
      totalTime: current.totalTime,
      rawProgress: ratio,
      sentProgress: clampedRatio,
    })
    window.playerBridge.seek(clampedRatio)
  }, [sessionFileStart, speedRpmBlocks])

  // Two files: every lap up to the longer race's last, each a jump to the start of that merged lap.
  const mergedLastLap = merged ? Math.max(merged.timeline.a_last_lap, merged.timeline.b_last_lap) : 0
  const mergedLapOptions = useMemo(() => Array.from({ length: mergedLastLap }, (_, i) => ({ value: i + 1, label: String(i + 1) })), [mergedLastLap])
  const mergedRef = useRef(merged)
  mergedRef.current = merged
  const handleMergedLap = useCallback((option: SingleValue<LapOption>) => {
    const current = mergedRef.current
    if (!option || !current) return
    const segment = current.timeline.segments.find(item => item.kind === 'lap' && item.lap_num === option.value)
    if (segment) current.seek(segment.start)
  }, [])

  const selectWidth = `${isCompact ? 'w-[3.5rem]' : isSpacious ? 'w-[5.5rem]' : 'w-[4.5rem]'} shrink-0`
  const controlSize = isCompact ? 'w-6 h-6' : isSpacious ? 'w-10 h-10' : 'w-8 h-8'
  const playControlSize = isCompact ? 'w-6 h-6' : isSpacious ? 'w-9 h-9' : 'w-7 h-7'
  const playIconSize = isCompact ? 12 : isSpacious ? 20 : 16
  const chevronIconSize = isCompact ? 14 : isSpacious ? 22 : 18
  const downloadIconSize = isCompact ? 14 : isSpacious ? 16 : 16

  return (
    <div className={`${isCompact ? 'h-10 gap-1' : isSpacious ? 'h-16 gap-3 pl-3 pr-4' : 'h-14 gap-2'} border-t border-[var(--border)] bg-[var(--bg-panel)] shrink-0 flex items-center pl-1 pr-2 z-40 select-none`}>
      <div className={`flex items-center ${isCompact ? 'gap-1' : isSpacious ? 'gap-3' : 'gap-3'}`}>
        <button onClick={merged ? () => merged.stepLap(-1) : onSeekBackward} title={merged ? 'Previous lap' : 'Seek backward 5s (-5s)'} aria-label={merged ? 'Previous lap' : undefined} className={`${controlSize} flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors`}><ChevronLeft size={chevronIconSize} /></button>
        <button
          type="button"
          onClick={merged ? merged.togglePlay : onTogglePlay}
          title={(merged ? merged.playing : state.isPlaying) ? 'Pause' : 'Play'}
          aria-label={(merged ? merged.playing : state.isPlaying) ? 'Pause playback' : 'Play playback'}
          className={`${playControlSize} inline-flex shrink-0 items-center justify-center p-0 leading-none rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-[var(--border-focus)] transition-colors`}
        >
          {(merged ? merged.playing : state.isPlaying)
            ? <Pause size={playIconSize} strokeWidth={2} fill="currentColor" className="block" />
            : <Play size={playIconSize} strokeWidth={2} fill="currentColor" className="block translate-x-[0.5px]" />}
        </button>
        <button onClick={merged ? () => merged.stepLap(1) : onSeekForward} title={merged ? 'Next lap' : 'Seek forward 5s (+5s)'} aria-label={merged ? 'Next lap' : undefined} className={`${controlSize} flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors`}><ChevronRight size={chevronIconSize} /></button>
      </div>
      {merged ? <>
        <MergedReadout merged={merged} isSpacious={isSpacious} />
        <MergedProgressTracker timeline={merged.timeline} time={merged.position.time} onSeek={merged.seek} />
        {/* Where Laps 4 shows the last lap: a lap selector up to the longer race's last lap. */}
        <LapSelect className={selectWidth} currentLapNum={merged.position.segment.kind === 'lap' ? merged.position.segment.lap_num : null}
          onChange={handleMergedLap} options={mergedLapOptions} />
      </> : <ProgressTracker compact={compact} currentTime={state.currentTime} onSeekProgress={onSeekProgress} progressPct={state.progressPct} totalTime={state.totalTime} />}
      <div className="flex items-center gap-1.5 shrink-0">
        {isSpacious && <span className="text-[10px] font-bold uppercase tracking-wider text-[var(--text-secondary)]">Speed</span>}
        <SpeedSelect className={selectWidth} onChange={handleSpeed} speed={state.speed} />
      </div>
      {showLapSelect && !merged && lapOptions.length > 0 && (
        <div className="flex items-center gap-1.5 shrink-0">
          {isSpacious && <span className="text-[10px] font-bold uppercase tracking-wider text-[var(--text-secondary)]">Lap</span>}
          <LapSelect className={selectWidth} currentLapNum={currentLapNum} onChange={handleLap} options={lapOptions} />
        </div>
      )}
      {showExport && onExport && (
        isSpacious ? (
          <button
            onClick={onExport}
            disabled={exportState === 'busy'}
            title={exportState === 'error' ? exportError ?? 'Export failed' : 'Export session to Parquet'}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded text-xs font-bold text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] border border-[var(--border)] transition-colors disabled:opacity-50 shrink-0"
          >
            <Download size={downloadIconSize} className={exportState === 'busy' ? 'animate-pulse' : ''} />
            <span>Export (Parquet)</span>
          </button>
        ) : (
          <button onClick={onExport} disabled={exportState === 'busy'} title={exportState === 'error' ? exportError ?? 'Export failed' : 'Export session to Parquet'} className={`${controlSize} flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors disabled:opacity-50 shrink-0`}><Download size={downloadIconSize} className={exportState === 'busy' ? 'animate-pulse' : ''} /></button>
        )
      )}
    </div>
  )
})
