import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import type { SecondaryFileData } from '../../components/AnalyzeScreen'
import type { PlaybackLapBlock, PlaybackState } from '../../types'
import { mergedTimeForA, parseMergedTimeline, positionAt, type MergedPosition, type MergedTimeline } from '../../lib/mergedTimeline'

export interface LapSyncedPlayback {
  timeline: MergedTimeline
  position: MergedPosition
  playing: boolean
  seek: (mergedTime: number) => void
  togglePlay: () => void
  stepLap: (direction: -1 | 1) => void
}

/**
 * Lap-synced playback of two files (the second-file comparison): one merged
 * timeline where each lap lasts as long as the slower file's (lap rules in
 * tnrp/MergedLapTimeline.h). The engine plays it (Engine::playerSetMergedTimeline):
 * its clock is the merged one, file A holds just short of its line while the
 * slower file finishes the lap, and file B's stream is placed from the same
 * clock. This builds the timeline, hands it to the engine, and reads the
 * engine's merged time back for the bar.
 */
export function useLapSyncedPlayback(
  file: SecondaryFileData | null,
  aBlocks: PlaybackLapBlock[] | null,
  playbackState: PlaybackState | null,
  sessionFileStart: number,
): LapSyncedPlayback | null {
  const totalTime = playbackState?.totalTime ?? 0
  const { input, timeline } = useMergedTimeline(file, playbackState?.filename ? aBlocks : null, sessionFileStart, totalTime)
  const stateRef = useRef(playbackState)
  stateRef.current = playbackState

  // The engine plays the timeline while it exists; without it, file A alone.
  const engineInput = timeline ? input : null
  useEffect(() => {
    if (!engineInput) return
    window.playerBridge.setMergedTimeline(engineInput)
    return () => window.playerBridge.setMergedTimeline(null)
  }, [engineInput])

  // The engine's merged clock, from the playback state (throttled for the UI,
  // as file A's position is). Before the engine has the timeline, A's own time.
  const mergedTime = !timeline ? 0
    : playbackState?.mergedTime
      ?? (playbackState ? mergedTimeForA(timeline, playbackState.currentTime) : null) ?? 0

  const seek = useCallback((target: number) => {
    if (!timeline) return
    const position = positionAt(timeline, target)
    const state = stateRef.current
    // The engine places file A from the merged time; the fraction is A's
    // nearest position, for anything that reads it before the engine answers.
    const pct = state && state.totalTime > 0 && position.a.time !== null
      ? Math.max(0, Math.min(1, (position.a.time - sessionFileStart) / state.totalTime)) : 0
    window.playerBridge.seek(pct, position.time)
  }, [timeline, sessionFileStart])

  const togglePlay = useCallback(() => {
    if (stateRef.current?.isPlaying) window.playerBridge.pause()
    else window.playerBridge.play()
  }, [])

  const position = useMemo(() => timeline ? positionAt(timeline, mergedTime) : null, [timeline, mergedTime])
  const stepLap = useCallback((direction: -1 | 1) => {
    if (!timeline || !position) return
    const segments = timeline.segments
    // Back: to this lap's start, or the previous lap's when already near it.
    let index = position.segmentIndex
    if (direction < 0) index = position.offset > 2 ? index : index - 1
    else index += 1
    index = Math.max(0, Math.min(segments.length - 1, index))
    seek(segments[index].start)
  }, [timeline, position, seek])

  if (!timeline || !position) return null
  return { timeline, position, playing: playbackState?.isPlaying ?? false, seek, togglePlay, stepLap }
}

/** The merged table for file A's laps and file B's, built by the engine. */
function useMergedTimeline(
  file: SecondaryFileData | null,
  aBlocks: PlaybackLapBlock[] | null,
  sessionFileStart: number,
  totalTime: number,
): { input: string | null; timeline: MergedTimeline | null } {
  const input = useMemo(() => {
    if (!file || !aBlocks || aBlocks.length === 0 || file.blocks.length === 0) return null
    const laps = (blocks: readonly { lapNum: number; startSessionTime: number; endSessionTime: number }[]) => blocks
      .filter(block => block.lapNum >= 1 && block.endSessionTime > block.startSessionTime)
      .map(block => ({ lap_num: block.lapNum, start: block.startSessionTime, end: block.endSessionTime }))
    return JSON.stringify({
      a: {
        laps: laps(aBlocks),
        ...totalTime > 0 ? { recording_start: sessionFileStart, recording_end: sessionFileStart + totalTime } : {},
      },
      b: { laps: laps(file.blocks) },
    })
  }, [aBlocks, file, sessionFileStart, totalTime])
  const [result, setResult] = useState<{ input: string; timeline: MergedTimeline | null } | null>(null)
  useEffect(() => {
    if (!input) return
    let cancelled = false
    window.analysisBridge.mergedLapTimeline(input).then(json => {
      if (!cancelled) setResult({ input, timeline: parseMergedTimeline(json) })
    }).catch(() => {
      if (!cancelled) setResult({ input, timeline: null })
    })
    return () => { cancelled = true }
  }, [input])
  return { input, timeline: input && result?.input === input ? result.timeline : null }
}

