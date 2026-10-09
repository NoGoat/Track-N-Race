import { memo } from 'react'

interface StatusOverlaysProps {
  exportProgress: number
  exportStage: string
  exportState: 'idle' | 'busy' | 'error'
  isScanning: boolean
}

export default memo(function StatusOverlays({ exportProgress, exportStage, exportState, isScanning }: StatusOverlaysProps) {
  const progress = Math.max(0, Math.min(100, exportProgress))
  return (
    <>
      {isScanning && (
        <div className="loading-scrim fixed inset-0 z-[100] flex flex-col items-center justify-center">
          <div className="loading-scrim-label text-xl font-bold mb-4 tracking-widest uppercase text-sm">Analyzing Session Data</div>
          <div className="loading-scrim-track w-64 h-1.5 rounded-full overflow-hidden relative"><div className="loading-scrim-fill absolute inset-y-0 left-0 w-1/2 rounded-full animate-bounce" style={{ animation: 'scan 1.5s infinite linear' }} /></div>
          <style>{`@keyframes scan { 0% { left: -50%; } 100% { left: 100%; } }`}</style>
        </div>
      )}
      {exportState === 'busy' && (
        <div className="loading-scrim fixed inset-0 z-[100] flex flex-col items-center justify-center">
          <div className="loading-scrim-label text-xl font-bold mb-4 tracking-widest uppercase text-sm">Exporting to Excel</div>
          <div className="loading-scrim-track w-64 h-1.5 rounded-full overflow-hidden relative"><div className="loading-scrim-fill absolute inset-y-0 left-0 rounded-full transition-[width] duration-150 ease-linear" style={{ width: `${progress}%` }} /></div>
          <div className="loading-scrim-muted mt-3 text-xs font-mono tracking-wider">{Math.round(progress)}%</div>
          {exportStage && <div className="loading-scrim-muted mt-1 text-[11px] tracking-wide opacity-80">{exportStage}…</div>}
        </div>
      )}
    </>
  )
})
