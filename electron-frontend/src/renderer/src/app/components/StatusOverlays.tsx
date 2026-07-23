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
        <div className="fixed inset-0 z-[100] flex flex-col items-center justify-center bg-[var(--bg-modal)] backdrop-blur-sm">
          <div className="text-xl font-bold text-[var(--text-primary)] mb-4 tracking-widest uppercase text-sm">Analyzing Session Data</div>
          <div className="w-64 h-1.5 bg-[var(--border)] rounded-full overflow-hidden relative"><div className="absolute inset-y-0 left-0 bg-[#5794F2] w-1/2 rounded-full animate-bounce" style={{ animation: 'scan 1.5s infinite linear' }} /></div>
          <style>{`@keyframes scan { 0% { left: -50%; } 100% { left: 100%; } }`}</style>
        </div>
      )}
      {exportState === 'busy' && (
        <div className="fixed inset-0 z-[100] flex flex-col items-center justify-center bg-[var(--bg-modal)] backdrop-blur-sm">
          <div className="text-xl font-bold text-[var(--text-primary)] mb-4 tracking-widest uppercase text-sm">Exporting to Excel</div>
          <div className="w-64 h-1.5 bg-[var(--border)] rounded-full overflow-hidden relative"><div className="absolute inset-y-0 left-0 bg-[#5794F2] rounded-full transition-[width] duration-150 ease-linear" style={{ width: `${progress}%` }} /></div>
          <div className="mt-3 text-xs font-mono text-[var(--text-secondary)] tracking-wider">{Math.round(progress)}%</div>
          {exportStage && <div className="mt-1 text-[11px] text-[var(--text-secondary)] tracking-wide opacity-80">{exportStage}…</div>}
        </div>
      )}
    </>
  )
})
