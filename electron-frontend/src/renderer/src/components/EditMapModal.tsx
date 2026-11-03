import { useEffect, useMemo, useState } from 'react'
import { createPortal } from 'react-dom'
import { RotateCcw, RotateCw, X } from 'lucide-react'
import { useModalPresenceValue } from '../lib/useModalPresence'
import { BUTTON_CLASS, PRIMARY_BUTTON_CLASS } from '../lib/buttonStyles'
import { TRACK_MAPS, type TrackMapData } from '../lib/trackMaps'
import TrackMap from './TrackMap'

interface Props {
  /** The map to edit, or null when closed. */
  mapId: string | null
  isDark: boolean
  onClose: () => void
  /** After a save; a built-in map's edit is saved under a new ID. */
  onSaved?: (id: string) => void
}

const NO_MARKERS = () => []

function wrapDegrees(value: number): number {
  const wrapped = value % 360
  return wrapped < 0 ? wrapped + 360 : wrapped
}

const INPUT_CLASS = 'w-full rounded border border-[var(--border-muted)] bg-[var(--bg-card)] px-2.5 py-1.5 text-xs text-[var(--text-primary)] outline-none focus:border-[var(--border-focus)]'

// Edit Map: a map's names and rotation, with a live preview.
export default function EditMapModal({ mapId, isDark, onClose, onSaved }: Props) {
  const { mounted, visible, transitionTargetRef, value: shownId } = useModalPresenceValue(mapId)
  const [base, setBase] = useState<TrackMapData | null>(null)
  const [name, setName] = useState('')
  const [trackName, setTrackName] = useState('')
  const [circuitName, setCircuitName] = useState('')
  const [rotation, setRotation] = useState(0)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    if (mapId === null) return
    let cancelled = false
    const fill = (map: TrackMapData | null) => {
      if (cancelled) return
      setBase(map)
      setError(map ? null : 'This map could not be loaded.')
      setName(map?.display_name ?? map?.track_name ?? '')
      setTrackName(map?.track_name ?? '')
      setCircuitName(map?.circuit_name ?? '')
      setRotation(wrapDegrees(map?.rotation_deg ?? 0))
    }
    setBase(null)
    if (mapId.startsWith('builtin:')) fill(TRACK_MAPS[Number(mapId.slice('builtin:'.length))] ?? null)
    else void window.trackMapsBridge.get(mapId).then(value => fill(value as TrackMapData | null))
    return () => { cancelled = true }
  }, [mapId])

  useEffect(() => {
    if (mapId === null) return
    const onKey = (event: KeyboardEvent) => { if (event.key === 'Escape') onClose() }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [mapId, onClose])

  const preview = useMemo(() => base ? { ...base, rotation_deg: rotation } : null, [base, rotation])

  if (!mounted || !shownId) return null

  const builtin = shownId.startsWith('builtin:')
  const save = async () => {
    setSaving(true)
    setError(null)
    try {
      const result = await window.trackMapsBridge.edit(shownId, {
        display_name: name,
        track_name: trackName,
        circuit_name: circuitName,
        rotation_deg: rotation,
      })
      if (!result.ok) { setError(result.error || 'The map could not be saved.'); return }
      onSaved?.(result.id)
      onClose()
    } finally {
      setSaving(false)
    }
  }

  const field = (label: string, value: string, onChange: (value: string) => void) => (
    <label className="flex flex-col gap-1">
      <span className="text-[10px] uppercase tracking-wider text-[var(--text-secondary)]">{label}</span>
      <input value={value} onChange={event => onChange(event.target.value)} className={INPUT_CLASS} />
    </label>
  )

  return createPortal(
    <div
      ref={transitionTargetRef}
      data-state={visible ? 'open' : 'closed'}
      className="modal-backdrop fixed inset-0 z-[120] flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
      role="dialog"
      aria-modal="true"
      aria-labelledby="edit-map-title"
      onMouseDown={event => { if (event.target === event.currentTarget) onClose() }}
    >
      <div className="modal-panel bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[min(60vw,860px)] max-w-[calc(100vw-2rem)] max-h-[85vh] flex flex-col overflow-hidden">
        <div className="flex items-center justify-between px-3 h-[55px] border-b border-[var(--border)] shrink-0 select-none">
          <div id="edit-map-title" className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest">
            Edit Map
          </div>
          <button
            onClick={onClose}
            aria-label="Close edit map"
            className="w-7 h-7 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#e10600] transition-colors shrink-0"
          >
            <X size={16} />
          </button>
        </div>

        <div className="flex-1 min-h-0 overflow-auto flex gap-5 p-5">
          <div className="relative flex-1 min-w-0 h-[340px] rounded-lg border border-[var(--border-muted)]">
            {preview && <TrackMap
              trackId={preview.track_id}
              mapOverride={preview}
              participants={null}
              isDark={isDark}
              markerSource={NO_MARKERS}
            />}
          </div>
          <div className="w-64 shrink-0 flex flex-col gap-3">
            {field('Name', name, setName)}
            {field('Grand Prix', trackName, setTrackName)}
            {field('Circuit', circuitName, setCircuitName)}
            <div className="flex flex-col gap-1">
              <span className="text-[10px] uppercase tracking-wider text-[var(--text-secondary)]">Rotation</span>
              <input
                type="range" min={0} max={359} step={1}
                value={Math.round(rotation)}
                onChange={event => setRotation(Number(event.target.value))}
                aria-label="Rotation"
              />
              <div className="flex items-center gap-1.5">
                <button className={BUTTON_CLASS} aria-label="Rotate 90° left"
                  onClick={() => setRotation(value => wrapDegrees(value - 90))}><RotateCcw size={12} /></button>
                <input
                  type="number" step={1}
                  value={Math.round(rotation * 10) / 10}
                  onChange={event => {
                    const value = Number(event.target.value)
                    if (Number.isFinite(value)) setRotation(wrapDegrees(value))
                  }}
                  className={`${INPUT_CLASS} text-center`}
                  aria-label="Rotation in degrees"
                />
                <button className={BUTTON_CLASS} aria-label="Rotate 90° right"
                  onClick={() => setRotation(value => wrapDegrees(value + 90))}><RotateCw size={12} /></button>
              </div>
            </div>
            {builtin && <p className="text-[10px] text-[var(--text-muted)]">Saved as a copy for this track.</p>}
            {error && <p className="text-xs text-red-400">{error}</p>}
          </div>
        </div>

        <div className="flex items-center justify-end gap-2 px-5 py-3 border-t border-[var(--border)] shrink-0">
          <button className={BUTTON_CLASS} onClick={onClose}>Cancel</button>
          <button className={PRIMARY_BUTTON_CLASS} disabled={!base || saving} onClick={() => void save()}>
            {saving ? 'Saving…' : 'Save'}
          </button>
        </div>
      </div>
    </div>,
    document.body,
  )
}
