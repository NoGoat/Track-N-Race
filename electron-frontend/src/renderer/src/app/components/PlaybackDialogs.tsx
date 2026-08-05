import { AlertTriangle, X } from 'lucide-react'
import { TEXT_ACTION_BUTTON_CLASS } from '../../lib/buttonStyles'

interface PlaybackDialogsProps {
  confirmOpenFilePath: string | null
  loadError: string | null
  setConfirmOpenFilePath: (path: string | null) => void
  setLoadError: (error: string | null) => void
}

export default function PlaybackDialogs({ confirmOpenFilePath, loadError: playbackLoadError, setConfirmOpenFilePath, setLoadError: setPlaybackLoadError }: PlaybackDialogsProps) {
  return (
    <>
      {confirmOpenFilePath && (
        <div className="fixed inset-0 z-[100] flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]">
          <div className="bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[480px] flex flex-col overflow-hidden animate-[eventFadeIn_0.2s_ease-out]">
            {/* Header */}
            <div className="flex items-center justify-between px-6 py-4 border-b border-[var(--border)] shrink-0 select-none">
              <div>
                <div className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest flex items-center gap-2">
                  <AlertTriangle size={14} className="text-amber-500" />
                  <span>Open Session File</span>
                </div>
              </div>
              <button
                onClick={() => setConfirmOpenFilePath(null)}
                className="w-8 h-8 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#e10600] transition-colors"
              >
                <X size={14} />
              </button>
            </div>

            {/* Body */}
            <div className="p-6 flex flex-col gap-4">
              <div className="flex flex-col gap-2">
                <p className="text-sm font-semibold text-[var(--text-primary)]">
                  Are you sure you want to open this file?
                </p>
                <p className="text-xs text-[var(--text-secondary)] leading-relaxed">
                  Opening it will stop the event bridge and if you have an session right now with the game, it will be closed.
                </p>
              </div>
              
              {confirmOpenFilePath && (
                <div className="p-3 rounded-lg bg-[var(--bg-card)]/30 border border-[var(--border)] flex items-center gap-3 select-none">
                  <span className="text-[10px] font-mono text-[var(--text-muted)] uppercase tracking-wider shrink-0">
                    Selected file:
                  </span>
                  <span className="text-xs font-mono text-[var(--text-secondary)] truncate flex-1 font-semibold">
                    {confirmOpenFilePath.split(/[\\/]/).pop()}
                  </span>
                </div>
              )}
            </div>

            {/* Footer / Actions */}
            <div className="flex items-center justify-end gap-3 px-6 py-4 border-t border-[var(--border)] bg-[var(--bg-card)]/10 shrink-0">
              <button
                onClick={() => setConfirmOpenFilePath(null)}
                className={TEXT_ACTION_BUTTON_CLASS}
              >
                No
              </button>
              <button
                onClick={() => {
                  window.playerBridge.load(confirmOpenFilePath)
                  setConfirmOpenFilePath(null)
                }}
                className={TEXT_ACTION_BUTTON_CLASS}
              >
                Yes
              </button>
            </div>
          </div>
        </div>
      )}

      {playbackLoadError && (
        <div
          className="fixed inset-0 z-[110] flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
          role="dialog"
          aria-modal="true"
          aria-labelledby="playback-load-error-title"
        >
          <div className="bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[480px] max-w-[calc(100vw-2rem)] flex flex-col overflow-hidden animate-[eventFadeIn_0.2s_ease-out]">
            <div className="flex items-center justify-between px-6 py-4 border-b border-[var(--border)] shrink-0 select-none">
              <div
                id="playback-load-error-title"
                className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest flex items-center gap-2"
              >
                <AlertTriangle size={15} className="text-[#e10600]" />
                <span>Recording Load Failed</span>
              </div>
              <button
                onClick={() => setPlaybackLoadError(null)}
                aria-label="Close error"
                className="w-8 h-8 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#e10600] transition-colors"
              >
                <X size={14} />
              </button>
            </div>

            <div className="p-6 flex flex-col gap-3">
              <p className="text-sm font-semibold text-[var(--text-primary)]">
                Could not open the recording file.
              </p>
              <div className="p-3 rounded-lg bg-[#e10600]/[0.06] border border-[#e10600]/30">
                <p className="text-xs font-mono text-[var(--text-secondary)] leading-relaxed break-words">
                  {playbackLoadError}
                </p>
              </div>
            </div>

            <div className="flex items-center justify-end px-6 py-4 border-t border-[var(--border)] bg-[var(--bg-card)]/10 shrink-0">
              <button
                autoFocus
                onClick={() => setPlaybackLoadError(null)}
                className={TEXT_ACTION_BUTTON_CLASS}
              >
                OK
              </button>
            </div>
          </div>
        </div>
      )}
    </>
  )
}

