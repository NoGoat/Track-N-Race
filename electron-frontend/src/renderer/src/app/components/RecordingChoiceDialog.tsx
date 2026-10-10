import { useEffect, useState } from 'react'
import { Save } from 'lucide-react'
import type { RecordingChoice, RecordingChoiceMsg } from '../../types'
import { BUTTON_CLASS } from '../../lib/buttonStyles'
import { useModalPresenceValue } from '../../lib/useModalPresence'

// Ask-scope recordings: the session wrote both an all-drivers and a
// driver-only file, and the user keeps one or both. Choices queue; the dialog
// has no close button and ignores Esc, so one is always made. The main
// process deletes the file not kept.
export default function RecordingChoiceDialog() {
  const [queue, setQueue] = useState<RecordingChoiceMsg[]>([])
  const [resolving, setResolving] = useState(false)

  useEffect(() => {
    const add = (choices: RecordingChoiceMsg[]) => setQueue(current => {
      const known = new Set(current.map(choice => choice.id))
      const added = choices.filter(choice => !known.has(choice.id))
      return added.length ? [...current, ...added] : current
    })
    const unsubscribe = window.recordingBridge.onChoice(choice => add([choice]))
    // Choices raised before this window mounted (or before a reload).
    void window.recordingBridge.pendingChoices().then(add)
    return unsubscribe
  }, [])

  const current = queue[0] ?? null
  const { mounted, visible, transitionTargetRef, value: displayed } = useModalPresenceValue(current)
  if (!mounted || !displayed) return null

  const resolve = (choice: RecordingChoice) => {
    if (resolving) return
    setResolving(true)
    void window.recordingBridge.resolveChoice(displayed.id, choice).finally(() => {
      setResolving(false)
      setQueue(items => items.filter(item => item.id !== displayed.id))
    })
  }

  const session = [displayed.trackName, displayed.sessionName].filter(Boolean).join(' – ')

  return (
    <div
      ref={transitionTargetRef}
      data-state={visible ? 'open' : 'closed'}
      className="modal-backdrop fixed inset-0 z-[115] flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
      role="dialog"
      aria-modal="true"
      aria-labelledby="recording-choice-title"
    >
      <div className="modal-panel bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[540px] max-w-[calc(100vw-2rem)] flex flex-col overflow-hidden">
        <div className="flex items-center px-6 py-4 border-b border-[var(--border)] shrink-0 select-none">
          <div
            id="recording-choice-title"
            className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest flex items-center gap-2"
          >
            <Save size={15} className="text-[var(--text-secondary)]" />
            <span>Save Recording</span>
          </div>
        </div>

        <div className="p-6 flex flex-col gap-3">
          <p className="text-sm font-semibold text-[var(--text-primary)]">
            {session ? `Which recording of ${session} do you want to keep?` : 'Which recording do you want to keep?'}
          </p>
          <p className="text-xs text-[var(--text-secondary)] leading-relaxed">
            Driver Only keeps just your car&apos;s telemetry. All Drivers keeps every car&apos;s.
            The file you don&apos;t keep is deleted.
          </p>
          {queue.length > 1 && (
            <p className="text-[10px] font-mono text-[var(--text-muted)] uppercase tracking-wider">
              {queue.length - 1} more waiting
            </p>
          )}
        </div>

        <div className="flex items-center justify-end gap-2 px-6 py-4 border-t border-[var(--border)] bg-[var(--bg-card)]/10 shrink-0">
          <button disabled={resolving} onClick={() => resolve('driver_only')} className={BUTTON_CLASS}>
            Driver Only
          </button>
          <button disabled={resolving} onClick={() => resolve('all_drivers')} className={BUTTON_CLASS}>
            All Drivers
          </button>
          <button autoFocus disabled={resolving} onClick={() => resolve('both')} className={BUTTON_CLASS}>
            Both
          </button>
        </div>
      </div>
    </div>
  )
}
