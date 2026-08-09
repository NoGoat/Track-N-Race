import { memo, useCallback, useEffect, useMemo, useRef, useState, type ChangeEvent } from 'react'
import Select, { type SingleValue } from 'react-select'
import { ChevronLeft, ChevronRight, Download, Pause, Play } from 'lucide-react'
import { buildSelectStyles } from '../../lib/selectStyles'
import { selectComponents } from '../../lib/selectComponents'
import { fmtLap } from '../bannerHelpers'
import { PLAYBACK_SPEED_OPTIONS } from '../appConfig'
import { playbackDebug } from '../../lib/playbackDebug'

const selectStyles = buildSelectStyles(true)

interface PlaybackBarProps {
  compact: boolean
  currentLapNum: number | null
  exportError: string | null
  exportState: 'idle' | 'busy' | 'error'
  onExport: () => void
  onSeekBackward: () => void
  onSeekForward: () => void
  onTogglePlay: () => void
  sessionFileStart: number
  speedRpmBlocks: any[] | null
  state: any
}

const ProgressTracker = memo(function ProgressTracker({ compact, currentTime, progressPct, totalTime }: { compact: boolean; currentTime: number; progressPct: number; totalTime: number }) {
  const inputRef = useRef<HTMLInputElement>(null)
  const draggingRef = useRef(false)
  const dragProgressRef = useRef(progressPct)
  const lastSeekRef = useRef(0)
  const [displayPct, setDisplayPct] = useState(progressPct)
  const startSessionTime = currentTime - progressPct * totalTime
  const displayTime = draggingRef.current ? startSessionTime + displayPct * totalTime : currentTime

  useEffect(() => {
    if (!draggingRef.current) {
      setDisplayPct(progressPct)
      if (inputRef.current) inputRef.current.value = String(progressPct)
    }
  }, [progressPct])

  const finishDrag = useCallback(() => {
    draggingRef.current = false
    playbackDebug('progress-finish', {
      progress: dragProgressRef.current,
      currentTime,
      totalTime,
      targetSessionTime: startSessionTime + dragProgressRef.current * totalTime,
    })
    window.playerBridge.seek(dragProgressRef.current)
  }, [currentTime, startSessionTime, totalTime])

  const handleChange = useCallback((event: ChangeEvent<HTMLInputElement>) => {
    const value = parseFloat(event.target.value)
    setDisplayPct(value)
    dragProgressRef.current = value
    const now = Date.now()
    if (now - lastSeekRef.current > 100) {
      lastSeekRef.current = now
      playbackDebug('progress-drag-seek', {
        progress: value,
        currentTime,
        totalTime,
        targetSessionTime: startSessionTime + value * totalTime,
      })
      window.playerBridge.seek(value)
    }
  }, [currentTime, startSessionTime, totalTime])

  return (
    <div className={`flex-1 flex items-center ${compact ? 'gap-2' : 'gap-4'}`}>
      <span className="text-xs font-mono text-[var(--text-secondary)] tabular-nums">{fmtLap(displayTime)}</span>
      <input ref={inputRef} type="range" min="0" max="1" step="0.001" defaultValue={progressPct} onMouseDown={() => { draggingRef.current = true }} onMouseUp={finishDrag} onTouchStart={() => { draggingRef.current = true }} onTouchEnd={finishDrag} onChange={handleChange} className="flex-1 h-1.5 bg-[var(--border)] rounded-full appearance-none outline-none cursor-pointer [&::-webkit-slider-thumb]:appearance-none [&::-webkit-slider-thumb]:w-3 [&::-webkit-slider-thumb]:h-3 [&::-webkit-slider-thumb]:rounded-full [&::-webkit-slider-thumb]:bg-[#5794F2] [&::-webkit-slider-thumb]:cursor-pointer" style={{ background: `linear-gradient(to right, #5794F2 ${displayPct * 100}%, var(--border) ${displayPct * 100}%)` }} />
      <span className="text-xs font-mono text-[var(--text-secondary)] tabular-nums">{fmtLap(totalTime)}</span>
    </div>
  )
})

export default memo(function PlaybackBar({ compact, currentLapNum, exportError, exportState, onExport, onSeekBackward, onSeekForward, onTogglePlay, sessionFileStart, speedRpmBlocks, state }: PlaybackBarProps) {
  const speedValue = PLAYBACK_SPEED_OPTIONS.find(option => option.value === state.speed) ?? PLAYBACK_SPEED_OPTIONS[2]
  const lapOptions = useMemo(() => speedRpmBlocks?.map(block => ({ value: block.lapNum, label: String(block.lapNum) })) ?? [], [speedRpmBlocks])
  const lapValue = currentLapNum !== null ? { value: currentLapNum, label: String(currentLapNum) } : null
  const handleSpeed = useCallback((option: SingleValue<(typeof PLAYBACK_SPEED_OPTIONS)[number]>) => { if (option) window.playerBridge.setSpeed(option.value) }, [])
  const handleLap = useCallback((option: SingleValue<{ value: number; label: string }>) => {
    if (!option || state.totalTime <= 0 || !speedRpmBlocks) {
      playbackDebug('lap-select-rejected', {
        selectedLap: option?.value ?? null,
        totalTime: state.totalTime,
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
    const ratio = (block.startSessionTime - sessionFileStart) / state.totalTime
    const clampedRatio = Math.max(0, Math.min(1, ratio))
    playbackDebug('lap-select-seek', {
      selectedLap: option.value,
      blockStart: block.startSessionTime,
      blockEnd: block.endSessionTime,
      sessionFileStart,
      currentTime: state.currentTime,
      totalTime: state.totalTime,
      rawProgress: ratio,
      sentProgress: clampedRatio,
    })
    window.playerBridge.seek(clampedRatio)
  }, [sessionFileStart, speedRpmBlocks, state.currentTime, state.totalTime])

  const controlSize = compact ? 'w-6 h-6' : 'w-8 h-8'
  return (
    <div className={`${compact ? 'h-10 gap-1' : 'h-14 gap-2'} border-t border-[var(--border)] bg-[var(--bg-panel)] shrink-0 flex items-center pl-1 pr-2 z-40 select-none`}>
      <div className={`flex items-center ${compact ? 'gap-1' : 'gap-3'}`}>
        <button onClick={onSeekBackward} className={`${controlSize} flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors`}><ChevronLeft size={compact ? 14 : 18} /></button>
        <button onClick={onTogglePlay} className={`${compact ? 'w-5 h-5' : 'w-7 h-7'} flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors`}>{state.isPlaying ? <Pause size={compact ? 13 : 16} fill="currentColor" /> : <Play size={compact ? 13 : 16} fill="currentColor" className="ml-0.5" />}</button>
        <button onClick={onSeekForward} className={`${controlSize} flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors`}><ChevronRight size={compact ? 14 : 18} /></button>
      </div>
      <ProgressTracker compact={compact} currentTime={state.currentTime} progressPct={state.progressPct} totalTime={state.totalTime} />
      <div className={`${compact ? 'w-[3.5rem]' : 'w-[4.5rem]'} shrink-0`}><Select value={speedValue} onChange={handleSpeed} options={PLAYBACK_SPEED_OPTIONS} styles={selectStyles} components={selectComponents} menuPlacement="top" isSearchable={false} /></div>
      {lapOptions.length > 0 && <div className={`${compact ? 'w-[3.5rem]' : 'w-[4.5rem]'} shrink-0`}><Select value={lapValue} options={lapOptions} onChange={handleLap} isSearchable={false} maxMenuHeight={150} menuPlacement="top" styles={selectStyles} components={selectComponents} placeholder="—" /></div>}
      <button onClick={onExport} disabled={exportState === 'busy'} title={exportState === 'error' ? exportError ?? 'Export failed' : 'Export session to Excel (.xlsx)'} className={`${controlSize} flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors disabled:opacity-50 shrink-0`}><Download size={compact ? 14 : 16} className={exportState === 'busy' ? 'animate-pulse' : ''} /></button>
    </div>
  )
})
