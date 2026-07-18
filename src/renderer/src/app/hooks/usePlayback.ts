import { useCallback, useEffect, useRef, useState } from 'react'
import { useTelemetryStore } from '../../stores/telemetryStore'

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
        const lapNum = blocks.find(block => next.currentTime >= block.startSessionTime && next.currentTime <= block.endSessionTime)?.lapNum ?? null
        if (lapNum !== currentLapRef.current) {
          currentLapRef.current = lapNum
          setCurrentLapNum(lapNum)
        }
      }
    })
    return () => {
      unsubscribe()
      if (uiTimerRef.current) clearTimeout(uiTimerRef.current)
    }
  }, [])

  useEffect(() => {
    currentLapRef.current = null
    setCurrentLapNum(null)
  }, [state?.filename])

  const seekBackward = useCallback(() => {
    const current = stateRef.current
    if (current) window.playerBridge.seek(Math.max(0, current.currentTime - 5) / current.totalTime)
  }, [])
  const seekForward = useCallback(() => {
    const current = stateRef.current
    if (current) window.playerBridge.seek(Math.min(current.totalTime, current.currentTime + 5) / current.totalTime)
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
  const close = useCallback(() => { onClose(); window.playerBridge.close() }, [onClose])
  const selectFile = useCallback(async () => {
    const file = await window.fsBridge.selectTNRDFile()
    if (file) {
      setState((previous: any) => ({ ...(previous || {}), isScanning: true }))
      window.playerBridge.load(file)
    }
  }, [])

  return {
    close, confirmOpenFilePath, currentLapNum, exportError, exportProgress, exportStage,
    exportState, exportXlsx, loadError, seekBackward, seekForward, selectFile,
    sessionFileStart: sessionFileStartRef.current, setConfirmOpenFilePath, setLoadError,
    speedRpmBlocks, state, togglePlay,
  }
}
