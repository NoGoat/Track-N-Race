import { useEffect, useState } from 'react'
import { Download } from 'lucide-react'
import type { AvailableUpdate, UpdateProgress } from '../../types'
import { BUTTON_CLASS, PRIMARY_BUTTON_CLASS } from '../../lib/buttonStyles'
import { useModalPresenceValue } from '../../lib/useModalPresence'

interface UpdateAvailableDialogProps {
  update: AvailableUpdate | null
  onClose: () => void
}

type InstallState =
  | { kind: 'idle' }
  | { kind: 'working'; progress: UpdateProgress | null }
  | { kind: 'failed'; error: string }

function megabytes(bytes: number): string {
  return (bytes / (1024 * 1024)).toFixed(1)
}

function progressLabel(progress: UpdateProgress | null): string {
  if (!progress) return 'Starting download…'
  if (progress.phase === 'verifying') return 'Verifying…'
  if (progress.phase === 'installing') return 'Starting installer…'
  return `Downloading ${megabytes(progress.received)} / ${megabytes(progress.total)} MB`
}

export default function UpdateAvailableDialog({ update, onClose }: UpdateAvailableDialogProps) {
  const { mounted, visible, transitionTargetRef, value: displayedUpdate } = useModalPresenceValue(update)
  const [install, setInstall] = useState<InstallState>({ kind: 'idle' })

  useEffect(() => window.updateBridge.onProgress(progress => {
    setInstall(current => current.kind === 'working' ? { kind: 'working', progress } : current)
  }), [])

  if (!mounted || !displayedUpdate) return null

  const working = install.kind === 'working'
  const fraction = working && install.progress && install.progress.total > 0
    ? Math.min(1, install.progress.received / install.progress.total)
    : 0

  const skipVersion = () => {
    window.updateBridge.skipVersion(displayedUpdate.latestVersion)
    onClose()
  }

  const download = () => {
    void window.updateBridge.openDownloadPage()
    onClose()
  }

  const installNow = async () => {
    setInstall({ kind: 'working', progress: null })
    const error = await window.updateBridge.install()
    // On success the app quits; leave the progress showing until it does.
    if (error) setInstall({ kind: 'failed', error })
  }

  return (
    <div
      ref={transitionTargetRef}
      data-state={visible ? 'open' : 'closed'}
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
          {!displayedUpdate.installable && (
            <p className="text-xs text-[var(--text-secondary)] leading-relaxed">
              Download opens the GitHub Releases page.
            </p>
          )}
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
          {working && (
            <div className="flex flex-col gap-2" data-testid="update-progress">
              <div className="h-1 w-full overflow-hidden rounded-full bg-[var(--border-subtle)]">
                <div
                  className="h-full bg-[var(--color-info)] transition-[width] duration-150 ease-linear"
                  style={{ width: `${(install.progress?.phase === 'downloading' || !install.progress ? fraction : 1) * 100}%` }}
                />
              </div>
              <span className="text-[11px] font-mono tabular-nums text-[var(--text-muted)]">{progressLabel(install.progress)}</span>
            </div>
          )}
          {install.kind === 'failed' && (
            <p className="text-xs font-mono text-[#e10600] leading-relaxed break-words" data-testid="update-error">
              Update failed: {install.error}
            </p>
          )}
        </div>

        <div className="flex items-center justify-end gap-3 px-6 py-4 border-t border-[var(--border)] bg-[var(--bg-card)]/10 shrink-0">
          <button onClick={skipVersion} className={BUTTON_CLASS} disabled={working}>
            Skip this version
          </button>
          <button onClick={onClose} className={BUTTON_CLASS} disabled={working}>
            Remind me Later
          </button>
          {displayedUpdate.installable && install.kind !== 'failed' ? (
            <button onClick={() => void installNow()} className={PRIMARY_BUTTON_CLASS} disabled={working} data-testid="update-install">
              <Download size={12} />
              Update now
            </button>
          ) : (
            <button onClick={download} className={PRIMARY_BUTTON_CLASS} disabled={working}>
              <Download size={12} />
              Download
            </button>
          )}
        </div>
      </div>
    </div>
  )
}
