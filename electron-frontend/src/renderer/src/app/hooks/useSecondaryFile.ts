import { useCallback, useState } from 'react'
import type { SecondaryFileData } from '../../components/AnalyzeScreen'
import { useTelemetryStore } from '../../stores/telemetryStore'
import { toSecondaryFileData, type AnalysisFilePayload, type PendingCircuitMismatch } from '../../lib/secondaryFile'

/**
 * Loading and closing the second recording from the title bar. Uses the same
 * desktop file dialog file A uses (fsBridge.selectTNRDFile); the engine reads
 * it through analysisBridge. A file from another circuit waits on the user
 * (pendingMismatch) before it is applied.
 */
export function useSecondaryFile(
  file: SecondaryFileData | null,
  setFile: (file: SecondaryFileData | null) => void,
) {
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [pendingMismatch, setPendingMismatch] = useState<PendingCircuitMismatch | null>(null)

  const select = useCallback(async () => {
    const filePath = await window.fsBridge.selectTNRDFile()
    if (!filePath) return
    setLoading(true)
    setError(null)
    const result = await window.analysisBridge.loadFile(filePath)
    setLoading(false)
    if (!result.ok) {
      setFile(null)
      setError(result.error ?? 'The recording could not be opened.')
      return
    }
    const data = (result.data ?? {}) as AnalysisFilePayload
    const trackId = Number.isFinite(result.trackId) ? result.trackId! : null
    const trackName = result.trackName || null
    const primaryTrackId = useTelemetryStore.getState().playbackTrackId
    if (primaryTrackId !== null && trackId !== null && trackId !== primaryTrackId) {
      setPendingMismatch({ filePath, data, trackId, trackName: trackName || `Circuit ${trackId}` })
      return
    }
    setFile(toSecondaryFileData(filePath, data, trackId, trackName))
  }, [setFile])

  const confirmMismatch = useCallback(() => {
    if (!pendingMismatch) return
    setFile(toSecondaryFileData(pendingMismatch.filePath, pendingMismatch.data, pendingMismatch.trackId, pendingMismatch.trackName))
    setPendingMismatch(null)
  }, [pendingMismatch, setFile])

  // The engine already holds the rejected file; drop it so it matches the UI.
  const cancelMismatch = useCallback(() => {
    setPendingMismatch(null)
    window.analysisBridge.closeFile()
    setFile(null)
  }, [setFile])

  const close = useCallback(() => {
    window.analysisBridge.closeFile()
    setFile(null)
    setError(null)
    setPendingMismatch(null)
  }, [setFile])

  return { file, loading, error, pendingMismatch, select, confirmMismatch, cancelMismatch, close }
}

export type SecondaryFileController = ReturnType<typeof useSecondaryFile>
