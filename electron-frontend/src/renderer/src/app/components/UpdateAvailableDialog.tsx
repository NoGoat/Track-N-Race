import { Download } from 'lucide-react'
import type { AvailableUpdate } from '../../types'
import { BUTTON_CLASS, PRIMARY_BUTTON_CLASS } from '../../lib/buttonStyles'
import { useModalPresenceValue } from '../../lib/useModalPresence'

interface UpdateAvailableDialogProps {
  update: AvailableUpdate | null
  onClose: () => void
}

export default function UpdateAvailableDialog({ update, onClose }: UpdateAvailableDialogProps) {
  const modalPresence = useModalPresenceValue(update)
  const displayedUpdate = modalPresence.value
  if (!modalPresence.mounted || !displayedUpdate) return null

  const skipVersion = () => {
    window.updateBridge.skipVersion(displayedUpdate.latestVersion)
    onClose()
  }

  const download = () => {
    void window.updateBridge.openDownloadPage()
    onClose()
  }

  return (
    <div
      data-state={modalPresence.visible ? 'open' : 'closed'}
      className="modal-backdrop fixed inset-0 z-[130] flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
      role="dialog"
      aria-modal="true"
      aria-labelledby="update-available-title"
    >
      <div className="modal-panel bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[520px] max-w-[calc(100vw-2rem)] flex flex-col overflow-hidden">
        <div className="flex items-center px-6 py-4 border-b border-[var(--border)] shrink-0 select-none">
          <div
            id="update-available-title"
            className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest flex items-center gap-2"
          >
            <Download size={16} strokeWidth={2.4} className="text-[var(--color-info)]" />
            <span>Update Available</span>
          </div>
        </div>

        <div className="p-6 flex flex-col gap-4">
          <p className="text-sm font-semibold text-[var(--text-primary)]">
            Track N Race {displayedUpdate.latestVersion} is available.
          </p>
          <p className="text-xs text-[var(--text-secondary)] leading-relaxed">
            Please note that the app currently doesn't support auto updates. Clicking Download will open the Github Releases page for the latest version instead.
          </p>
          <div className="grid grid-cols-2 gap-3 rounded-lg bg-[var(--bg-card)]/30 border border-[var(--border)] p-3 font-mono text-xs">
            <div>
              <div className="text-[9px] uppercase tracking-wider text-[var(--text-secondary)]">Installed</div>
              <div className="mt-1 font-semibold text-[var(--text-primary)]">{displayedUpdate.currentVersion}</div>
            </div>
            <div>
              <div className="text-[9px] uppercase tracking-wider text-[var(--text-secondary)]">Latest</div>
              <div className="mt-1 font-semibold text-[var(--text-primary)]">{displayedUpdate.latestVersion}</div>
            </div>
          </div>
        </div>

        <div className="flex items-center justify-end gap-3 px-6 py-4 border-t border-[var(--border)] bg-[var(--bg-card)]/10 shrink-0">
          <button onClick={skipVersion} className={BUTTON_CLASS}>
            Skip this version
          </button>
          <button onClick={onClose} className={BUTTON_CLASS}>
            Remind me Later
          </button>
          <button onClick={download} className={PRIMARY_BUTTON_CLASS}>
            <Download size={12} />
            Download
          </button>
        </div>
      </div>
    </div>
  )
}
