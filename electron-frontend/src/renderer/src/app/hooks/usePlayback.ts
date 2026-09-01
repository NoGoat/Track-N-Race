import { useCallback, useEffect, useRef, useState } from 'react'
import { useTelemetryStore } from '../../stores/telemetryStore'
import { playbackDebug } from '../../lib/playbackDebug'
import { setPlaybackCursorTime } from '../../lib/playbackCursor'

export function usePlayback(onClose: () => void) {
  const speedRpmBlocks = useTelemetryStore(state => state.speedRpmBlocks)
  const [state, setState] = useState<any>(null)
  const stateRef = useRef<any>(null)
  const uiLastRef = useRef(0)
  const uiTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const uiPendingRef = useRef<any>(null)
  const sessionFileStartRef = useRef(0)
  const capturedBlocksRef = useRef<any[] | null>(null)
  const blocksRef = useRef<any[] | null>(null)
  const currentLapRef = useRef<number | null>(null)
  const [currentLapNum, setCurrentLapNum] = useState<number | null>(null)
  const [confirmOpenFilePath, setConfirmOpenFilePath] = useState<string | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)
  const [exportState, setExportState] = useState<'idle' | 'busy' | 'error'>('idle')
  const [exportError, setExportError] = useState<string | null>(null)
  const [exportProgress, setExportProgress] = useState(0)
  const [exportStage, setExportStage] = useState('')

  blocksRef.current = speedRpmBlocks
  if (speedRpmBlocks !== capturedBlocksRef.current) {
    capturedBlocksRef.current = speedRpmBlocks
    if (speedRpmBlocks && stateRef.current) {
      const current = stateRef.current
      sessionFileStartRef.current = current.currentTime - current.progressPct * current.totalTime
    }
  }

  useEffect(() => window.playerBridge.onRequestOpenConfirm(setConfirmOpenFilePath), [])
  useEffect(() => window.playerBridge.onLoadFailed(reason => setLoadError(reason || 'The file could not be read.')), [])
  useEffect(() => window.playerBridge.onExportProgress((progress, stage) => {
    setExportProgress(progress)
    if (stage) setExportStage(stage)
  }), [])

  useEffect(() => {
    const publish = (next: any) => {
      uiLastRef.current = performance.now()
      uiPendingRef.current = null
      uiTimerRef.current = null
      setState(next)
    }
    const unsubscribe = window.playerBridge.onStateChange(next => {
      setPlaybackCursorTime(next?.filename && Number.isFinite(next.currentTime) ? next.currentTime : null)
      const previous = stateRef.current
      stateRef.current = next
      const structuralChange = !previous
        || previous.filename !== next.filename
        || previous.isPlaying !== next.isPlaying
        || previous.isScanning !== next.isScanning
        || previous.speed !== next.speed
      const remaining = 100 - (performance.now() - uiLastRef.current)
      if (structuralChange || remaining <= 0) {
        if (uiTimerRef.current) clearTimeout(uiTimerRef.current)
        publish(next)
      } else {
        uiPendingRef.current = next
        if (!uiTimerRef.current) {
          uiTimerRef.current = setTimeout(() => {
            const pending = uiPendingRef.current
            if (pending) publish(pending)
          }, remaining)
        }
      }
      const blocks = blocksRef.current
      if (blocks) {
        // Adjacent lap blocks share their boundary timestamp. Select the block
        // with the latest start so an exact lap-start seek belongs to the new
        // lap instead of leaving the paused selector on the previous one.
        let currentBlock: any = null
        for (const block of blocks) {
          if (next.currentTime >= block.startSessionTime && next.currentTime <= block.endSessionTime &&
              (!currentBlock || block.startSessionTime > currentBlock.startSessionTime)) {
            currentBlock = block
          }
        }
        const lapNum = currentBlock?.lapNum ?? null
        const timeJump = previous ? next.currentTime - previous.currentTime : 0
        if (!previous || Math.abs(timeJump) >= 1 || lapNum !== currentLapRef.current) {
          playbackDebug('player-state', {
            previousTime: previous?.currentTime ?? null,
            currentTime: next.currentTime,
            timeJump,
            progress: next.progressPct,
            totalTime: next.totalTime,
            isPlaying: next.isPlaying,
            detectedLap: lapNum,
            previousDetectedLap: currentLapRef.current,
          })
        }
        if (lapNum !== currentLapRef.current) {
          currentLapRef.current = lapNum
          setCurrentLapNum(lapNum)
        }
      }
    })
    return () => {
      unsubscribe()
      setPlaybackCursorTime(null)
      if (uiTimerRef.current) clearTimeout(uiTimerRef.current)
    }
  }, [])

  useEffect(() => {
    currentLapRef.current = null
    setCurrentLapNum(null)
  }, [state?.filename])

  const seekBackward = useCallback(() => {
    const current = stateRef.current
    if (current) {
      const progress = Math.max(0, current.currentTime - 5) / current.totalTime
      playbackDebug('seek-backward', { currentTime: current.currentTime, totalTime: current.totalTime, sentProgress: progress })
      window.playerBridge.seek(progress)
    }
  }, [])
  const seekForward = useCallback(() => {
    const current = stateRef.current
    if (current) {
      const progress = Math.min(current.totalTime, current.currentTime + 5) / current.totalTime
      playbackDebug('seek-forward', { currentTime: current.currentTime, totalTime: current.totalTime, sentProgress: progress })
      window.playerBridge.seek(progress)
    }
  }, [])
  const seekProgress = useCallback((progress: number) => {
    window.playerBridge.seek(Math.max(0, Math.min(1, progress)))
  }, [])
  const setSpeed = useCallback((speed: number) => {
    window.playerBridge.setSpeed(speed)
  }, [])
  const togglePlay = useCallback(() => {
    const current = stateRef.current
    if (!current) return
    if (current.isPlaying) window.playerBridge.pause()
    else window.playerBridge.play()
  }, [])
  const exportXlsx = useCallback(async () => {
    if (exportState === 'busy') return
    setExportState('busy')
    setExportError(null)
    setExportProgress(0)
    setExportStage('Preparing export')
    const result = await window.playerBridge.exportXlsx()
    if (result.ok || result.error === 'cancelled') setExportState('idle')
    else {
      setExportState('error')
      setExportError(result.error ?? 'Export failed')
      setTimeout(() => setExportState('idle'), 4000)
    }
  }, [exportState])
  const close = useCallback(() => {
    console.log(`[close-trace] ${new Date().toISOString()} renderer close clicked`)
    onClose()
    console.log(`[close-trace] ${new Date().toISOString()} renderer state cleared; sending player:close`)
    window.playerBridge.close()
  }, [onClose])
  const selectFile = useCallback(async () => {
    const file = await window.fsBridge.selectTNRDFile()
    if (file) {
      setState((previous: any) => ({ ...(previous || {}), isScanning: true }))
      window.playerBridge.load(file)
    }
  }, [])

  return {
    close, confirmOpenFilePath, currentLapNum, exportError, exportProgress, exportStage,
    exportState, exportXlsx, loadError, seekBackward, seekForward, seekProgress, selectFile, setSpeed,
    sessionFileStart: sessionFileStartRef.current, setConfirmOpenFilePath, setLoadError,
    speedRpmBlocks, state, togglePlay,
  }
}
