import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import PlaybackBar from '../app/components/PlaybackBar'
import { getPlaybackCursorTime } from '../lib/playbackCursor'
import { useTelemetryStore } from '../stores/telemetryStore'
import type { AnalyzeLapData, PlayerPositionPoint } from '../types'
import TrackMap, { type TrackMapMarker } from './TrackMap'

interface Props {
  comparison: AnalyzeLapData | null
  comparisonColor: string
  compatibleCircuit: boolean
  current: AnalyzeLapData | null
  currentColor: string
  fixedMode: boolean
  isDark: boolean
  mapDimmed: boolean
  reduceAnimations: boolean
  sectorColors: boolean
  trackId: number | null
  focus: AnalyzeMapFocus | null
}

export interface AnalyzeMapFocus {
  id: number
  elapsedSeconds: number
}

interface LocalClock {
  cursor: number
  playing: boolean
  speed: number
  startedAt: number
  total: number
}

interface BarState {
  currentTime: number
  isPlaying: boolean
  progressPct: number
  speed: number
  totalTime: number
}

function lapDuration(lap: AnalyzeLapData | null): number {
  return lap ? Math.max(0, lap.endSessionTime - lap.startSessionTime) : 0
}

function readClock(clock: LocalClock, now = performance.now()): number {
  if (!clock.playing) return clock.cursor
  return Math.min(clock.total, clock.cursor + (now - clock.startedAt) / 1000 * clock.speed)
}

function writeMarkerAt(
  marker: TrackMapMarker,
  points: readonly PlayerPositionPoint[],
  target: number,
): boolean {
  if (points.length === 0) return false
  if (target <= points[0].session_time) {
    marker.x = points[0].x
    marker.z = points[0].z
    return true
  }
  const last = points[points.length - 1]
  if (target >= last.session_time) {
    marker.x = last.x
    marker.z = last.z
    return true
  }
  let lo = 0
  let hi = points.length - 1
  while (lo + 1 < hi) {
    const mid = (lo + hi) >>> 1
    if (points[mid].session_time <= target) lo = mid
    else hi = mid
  }
  const before = points[lo]
  const after = points[hi]
  const span = after.session_time - before.session_time
  const ratio = span > 0 ? (target - before.session_time) / span : 0
  marker.x = before.x + (after.x - before.x) * ratio
  marker.z = before.z + (after.z - before.z) * ratio
  return true
}

export default function AnalyzeMapComparison({
  comparison, comparisonColor, compatibleCircuit, current, currentColor,
  fixedMode, isDark, mapDimmed, reduceAnimations,
  sectorColors, trackId, focus,
}: Props) {
  const aeroMode = useTelemetryStore(state => state.protocolStatus?.aero_mode ?? 'drs')
  const total = Math.max(lapDuration(current), lapDuration(comparison))
  const initialCursor = Math.max(0, Math.min(total, focus?.elapsedSeconds ?? 0))
  const clockRef = useRef<LocalClock>({ cursor: initialCursor, playing: false, speed: 1, startedAt: performance.now(), total })
  const [barState, setBarState] = useState<BarState>({ currentTime: initialCursor, isPlaying: false, progressPct: total > 0 ? initialCursor / total : 0, speed: 1, totalTime: total })
  const markerBufferRef = useRef<TrackMapMarker[]>([])
  const currentMarkerRef = useRef<TrackMapMarker>({ x: 0, z: 0, label: '', color: currentColor })
  const comparisonMarkerRef = useRef<TrackMapMarker>({ x: 0, z: 0, label: '', color: comparisonColor })

  const publishClock = useCallback((cursor: number, playing: boolean) => {
    const clock = clockRef.current
    const next: BarState = {
      currentTime: cursor,
      isPlaying: playing,
      progressPct: clock.total > 0 ? cursor / clock.total : 0,
      speed: clock.speed,
      totalTime: clock.total,
    }
    setBarState(previous => previous.currentTime === next.currentTime &&
      previous.isPlaying === next.isPlaying && previous.progressPct === next.progressPct &&
      previous.speed === next.speed && previous.totalTime === next.totalTime ? previous : next)
  }, [])

  const setClockTime = useCallback((cursor: number) => {
    const clock = clockRef.current
    clock.cursor = Math.max(0, Math.min(clock.total, cursor))
    clock.startedAt = performance.now()
    publishClock(clock.cursor, clock.playing)
  }, [publishClock])

  useEffect(() => {
    const clock = clockRef.current
    const cursor = Math.max(0, Math.min(total, focus?.elapsedSeconds ?? 0))
    clock.cursor = cursor
    clock.playing = false
    clock.startedAt = performance.now()
    clock.total = total
    publishClock(cursor, false)
  }, [comparison, current, focus, publishClock, total])

  useEffect(() => {
    if (!fixedMode) {
      const clock = clockRef.current
      clock.cursor = readClock(clock)
      clock.playing = false
      clock.startedAt = performance.now()
      publishClock(clock.cursor, false)
      return
    }
    const timer = window.setInterval(() => {
      const clock = clockRef.current
      let cursor = readClock(clock)
      if (clock.playing && cursor >= clock.total) {
        cursor = clock.total
        clock.cursor = cursor
        clock.playing = false
        clock.startedAt = performance.now()
      }
      publishClock(cursor, clock.playing)
    }, 100)
    return () => window.clearInterval(timer)
  }, [fixedMode, publishClock])

  const markerSource = useCallback((): readonly TrackMapMarker[] => {
    const globalCursorTime = getPlaybackCursorTime()
    const elapsed = fixedMode
      ? readClock(clockRef.current)
      : focus
        ? Math.max(0, Math.min(total, focus.elapsedSeconds))
      : current && globalCursorTime !== null
        ? globalCursorTime - current.startSessionTime
        : 0
    const output = markerBufferRef.current
    output.length = 0

    if (current) {
      const marker = currentMarkerRef.current
      marker.label = fixedMode ? 'Lap A' : 'Current'
      marker.color = currentColor
      const target = current.startSessionTime + Math.max(0, Math.min(lapDuration(current), elapsed))
      if (writeMarkerAt(marker, current.playerPositions, target)) output.push(marker)
    }
    if (comparison) {
      const marker = comparisonMarkerRef.current
      marker.label = fixedMode ? 'Lap B' : 'Compare'
      marker.color = comparisonColor
      const target = comparison.startSessionTime + Math.max(0, Math.min(lapDuration(comparison), elapsed))
      if (writeMarkerAt(marker, comparison.playerPositions, target)) output.push(marker)
    }
    return output
  }, [comparison, comparisonColor, current, currentColor, fixedMode, focus, total])

  const togglePlay = useCallback(() => {
    const clock = clockRef.current
    let cursor = readClock(clock)
    if (!clock.playing && cursor >= clock.total) cursor = 0
    clock.cursor = cursor
    clock.playing = !clock.playing && clock.total > 0
    clock.startedAt = performance.now()
    publishClock(clock.cursor, clock.playing)
  }, [publishClock])

  const setSpeed = useCallback((speed: number) => {
    const clock = clockRef.current
    clock.cursor = readClock(clock)
    clock.speed = speed
    clock.startedAt = performance.now()
    publishClock(clock.cursor, clock.playing)
  }, [publishClock])

  const map = useMemo(() => <TrackMap
    trackId={compatibleCircuit ? trackId : null}
    participants={null}
    isDark={isDark}
    sectorColors={sectorColors}
    reduceAnimations={reduceAnimations}
    mapDimmed={mapDimmed}
    aeroMode={aeroMode}
    markerSource={markerSource}
  />, [aeroMode, compatibleCircuit, isDark, mapDimmed, markerSource, reduceAnimations, sectorColors, trackId])

  return <div className="h-full min-h-0 flex flex-col">
    <div className="flex-1 min-h-0 relative">
      {map}
    </div>
    {fixedMode && <PlaybackBar
      compact
      onSeekProgress={progress => setClockTime(progress * clockRef.current.total)}
      onSeekBackward={() => setClockTime(readClock(clockRef.current) - 5)}
      onSeekForward={() => setClockTime(readClock(clockRef.current) + 5)}
      onSpeedChange={setSpeed}
      onTogglePlay={togglePlay}
      showExport={false}
      showLapSelect={false}
      state={barState}
    />}
  </div>
}
