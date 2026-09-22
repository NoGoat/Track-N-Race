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
  speedRpmBlocks?: any[] | null
  state: any
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
  const value = currentLapNum !== null ? { value: currentLapNum, label: String(currentLapNum) } : null
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

export default memo(function PlaybackBar({ compact, currentLapNum = null, exportError = null, exportState = 'idle', onExport, onSeekProgress, onSeekBackward, onSeekForward, onSpeedChange, onTogglePlay, sessionFileStart = 0, showExport = true, showLapSelect = true, speedRpmBlocks = null, state }: PlaybackBarProps) {
  const isCompact = compact === true || compact === 'compact'
  const isSpacious = compact === 'spacious'
  const lapOptions = useMemo(() => speedRpmBlocks?.map(block => ({ value: block.lapNum, label: String(block.lapNum) })) ?? [], [speedRpmBlocks])
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

  const selectWidth = `${isCompact ? 'w-[3.5rem]' : isSpacious ? 'w-[5.5rem]' : 'w-[4.5rem]'} shrink-0`
  const controlSize = isCompact ? 'w-6 h-6' : isSpacious ? 'w-10 h-10' : 'w-8 h-8'
  const playControlSize = isCompact ? 'w-6 h-6' : isSpacious ? 'w-9 h-9' : 'w-7 h-7'
  const playIconSize = isCompact ? 12 : isSpacious ? 20 : 16
  const chevronIconSize = isCompact ? 14 : isSpacious ? 22 : 18
  const downloadIconSize = isCompact ? 14 : isSpacious ? 16 : 16

  return (
    <div className={`${isCompact ? 'h-10 gap-1' : isSpacious ? 'h-16 gap-3 pl-3 pr-4' : 'h-14 gap-2'} border-t border-[var(--border)] bg-[var(--bg-panel)] shrink-0 flex items-center pl-1 pr-2 z-40 select-none`}>
      <div className={`flex items-center ${isCompact ? 'gap-1' : isSpacious ? 'gap-3' : 'gap-3'}`}>
        <button onClick={onSeekBackward} title="Seek backward 5s (-5s)" className={`${controlSize} flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors`}><ChevronLeft size={chevronIconSize} /></button>
        <button
          type="button"
          onClick={onTogglePlay}
          title={state.isPlaying ? 'Pause' : 'Play'}
          aria-label={state.isPlaying ? 'Pause playback' : 'Play playback'}
          className={`${playControlSize} inline-flex shrink-0 items-center justify-center p-0 leading-none rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-[var(--border-focus)] transition-colors`}
        >
          {state.isPlaying
            ? <Pause size={playIconSize} strokeWidth={2} fill="currentColor" className="block" />
            : <Play size={playIconSize} strokeWidth={2} fill="currentColor" className="block translate-x-[0.5px]" />}
        </button>
        <button onClick={onSeekForward} title="Seek forward 5s (+5s)" className={`${controlSize} flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors`}><ChevronRight size={chevronIconSize} /></button>
      </div>
      <ProgressTracker compact={compact} currentTime={state.currentTime} onSeekProgress={onSeekProgress} progressPct={state.progressPct} totalTime={state.totalTime} />
      <div className="flex items-center gap-1.5 shrink-0">
        {isSpacious && <span className="text-[10px] font-bold uppercase tracking-wider text-[var(--text-secondary)]">Speed</span>}
        <SpeedSelect className={selectWidth} onChange={handleSpeed} speed={state.speed} />
      </div>
      {showLapSelect && lapOptions.length > 0 && (
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
            title={exportState === 'error' ? exportError ?? 'Export failed' : 'Export session to Excel (.xlsx)'}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded text-xs font-bold text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] border border-[var(--border)] transition-colors disabled:opacity-50 shrink-0"
          >
            <Download size={downloadIconSize} className={exportState === 'busy' ? 'animate-pulse' : ''} />
            <span>Export (.xlsx)</span>
          </button>
        ) : (
          <button onClick={onExport} disabled={exportState === 'busy'} title={exportState === 'error' ? exportError ?? 'Export failed' : 'Export session to Excel (.xlsx)'} className={`${controlSize} flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors disabled:opacity-50 shrink-0`}><Download size={downloadIconSize} className={exportState === 'busy' ? 'animate-pulse' : ''} /></button>
        )
      )}
    </div>
  )
})
