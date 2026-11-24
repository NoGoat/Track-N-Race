import { AlertTriangle, X } from 'lucide-react'
import { useTelemetryStore } from '../../stores/telemetryStore'
import { BUTTON_CLASS, PRIMARY_BUTTON_CLASS } from '../../lib/buttonStyles'
import { useModalPresenceValue } from '../../lib/useModalPresence'
import type { PendingCircuitMismatch } from '../../lib/secondaryFile'

/** Asks before using a second file recorded on another circuit (moved here from Analysis). */
export default function SecondaryFileMismatchModal({ pending, onConfirm, onCancel }: {
  pending: PendingCircuitMismatch | null
  onConfirm: () => void
  onCancel: () => void
}) {
  const primaryTrackId = useTelemetryStore(s => s.playbackTrackId)
  const primaryTrackName = useTelemetryStore(s => s.playbackTrackName)
  const { mounted, visible, transitionTargetRef, value: displayed } = useModalPresenceValue(pending)
  if (!mounted || !displayed) return null
  return (
    <div
      ref={transitionTargetRef}
      data-state={visible ? 'open' : 'closed'}
      className="modal-backdrop fixed inset-0 z-[120] flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
      role="dialog" aria-modal="true" aria-labelledby="secondary-circuit-mismatch-title"
    >
      <div className="modal-panel bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[500px] max-w-[calc(100vw-2rem)] flex flex-col overflow-hidden">
        <div className="flex items-center justify-between px-6 py-4 border-b border-[var(--border)] shrink-0 select-none">
          <div id="secondary-circuit-mismatch-title" className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest flex items-center gap-2">
            <AlertTriangle size={15} className="text-amber-500" />
            <span>Circuit Mismatch</span>
          </div>
          <button onClick={onCancel} aria-label="Cancel loading Secondary File" className="w-9 h-9 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#d44252] transition-colors">
            <X size={18} />
          </button>
        </div>

        <div className="p-6 flex flex-col gap-4">
          <p className="text-sm font-semibold text-[var(--text-primary)]">The Secondary File was recorded on a different circuit.</p>
          <p className="text-xs text-[var(--text-secondary)] leading-relaxed">Lap comparisons may not align correctly. You can cancel or load the file anyway.</p>
          <div className="p-3 rounded-lg bg-[var(--bg-card)]/30 border border-[var(--border)] text-xs font-mono text-[var(--text-secondary)] space-y-2">
            <div><span className="text-[var(--text-muted)]">Loaded File: </span><span className="font-semibold">{primaryTrackName || `Circuit ${primaryTrackId}`}</span></div>
            <div><span className="text-[var(--text-muted)]">Selected File: </span><span className="font-semibold">{displayed.trackName}</span></div>
          </div>
        </div>

        <div className="flex items-center justify-end gap-3 px-6 py-4 border-t border-[var(--border)] bg-[var(--bg-card)]/10 shrink-0">
          <button onClick={onCancel} className={BUTTON_CLASS}>Cancel</button>
          <button onClick={onConfirm} className={PRIMARY_BUTTON_CLASS}>Load Anyway</button>
        </div>
      </div>
    </div>
  )
}
