import { AlertTriangle, X } from 'lucide-react'
import type { RecordingErrorMsg } from '../../types'
import { TEXT_ACTION_BUTTON_CLASS } from '../../lib/buttonStyles'
import { useModalPresenceValue } from '../../lib/useModalPresence'

interface RecordingErrorDialogProps {
  error: RecordingErrorMsg | null
  onClose: () => void
}

export default function RecordingErrorDialog({ error, onClose }: RecordingErrorDialogProps) {
  const modalPresence = useModalPresenceValue(error)
  const displayedError = modalPresence.value
  if (!modalPresence.mounted || !displayedError) return null

  return (
    <div
      data-state={modalPresence.visible ? 'open' : 'closed'}
      className="modal-backdrop fixed inset-0 z-[120] flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
      role="dialog"
      aria-modal="true"
      aria-labelledby="recording-error-title"
    >
      <div className="modal-panel bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[540px] max-w-[calc(100vw-2rem)] flex flex-col overflow-hidden">
        <div className="flex items-center justify-between px-6 py-4 border-b border-[var(--border)] shrink-0 select-none">
          <div
            id="recording-error-title"
            className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest flex items-center gap-2"
          >
            <AlertTriangle size={15} className="text-[#e10600]" />
            <span>Session Recording Failed</span>
          </div>
          <button
            onClick={onClose}
            aria-label="Close recording error"
            className="w-8 h-8 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#e10600] transition-colors"
          >
            <X size={14} />
          </button>
        </div>

        <div className="p-6 flex flex-col gap-4">
          <p className="text-sm font-semibold text-[var(--text-primary)]">
            Track N Race could not write the session recording.
          </p>
          <div className="p-3 rounded-lg bg-[#e10600]/[0.06] border border-[#e10600]/30 flex flex-col gap-2">
            <div className="text-xs font-mono text-[var(--text-secondary)] leading-relaxed break-words">
              <span className="text-[var(--text-muted)]">Operation: </span>
              {displayedError.operation}
            </div>
            <div className="text-xs font-mono text-[var(--text-secondary)] leading-relaxed break-words">
              {displayedError.message}
            </div>
          </div>
          {displayedError.path && (
            <div className="flex flex-col gap-1.5">
              <span className="text-[10px] font-mono text-[var(--text-muted)] uppercase tracking-wider">
                Destination
              </span>
              <div className="p-3 rounded-lg bg-[var(--bg-card)]/30 border border-[var(--border)] text-xs font-mono text-[var(--text-secondary)] leading-relaxed break-all select-text">
                {displayedError.path}
              </div>
            </div>
          )}
        </div>

        <div className="flex items-center justify-end px-6 py-4 border-t border-[var(--border)] bg-[var(--bg-card)]/10 shrink-0">
          <button
            autoFocus
            onClick={onClose}
            className={TEXT_ACTION_BUTTON_CLASS}
          >
            OK
          </button>
        </div>
      </div>
    </div>
  )
}
