import { memo, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { Plus, Search } from 'lucide-react'
import {
  ANALYZE_METRICS, ANALYZE_METRIC_CATEGORIES, ANALYZE_TYRE_CORNERS, ANALYZE_TYRE_ROWS,
} from '../lib/analyzeMetrics'
import { SELECT_MENU_ANIMATION_MS } from '../lib/selectStyles'

type Category = (typeof ANALYZE_METRIC_CATEGORIES)[number]
type Phase = 'closed' | 'open' | 'closing'

const PANEL_WIDTH = 336
const CORNER_METRIC_IDS = new Set(ANALYZE_TYRE_ROWS.flatMap(row => ANALYZE_TYRE_CORNERS.map(corner => `${row.idPrefix}-${corner.key}`)))
const CORNER_TERMS = ANALYZE_TYRE_CORNERS.map(corner => corner.label).join(' ')

// Every token must appear somewhere in the haystack, so "rear ride" and
// "ride rear" both find Rear Ride Height.
function matches(query: string, haystack: string): boolean {
  if (!query) return true
  const text = haystack.toLowerCase()
  return query.split(/\s+/).every(token => text.includes(token))
}

function reduceAnimations(): boolean {
  return document.documentElement.dataset.reduceAnimations === 'true'
}

// Categories stack top to bottom as three-per-row grids of toggle tiles. Each
// per-corner tyre metric is a row of the same tiles: FL / FR / RL / RR pick
// corners, and ALL switches the row between a card per corner and one
// combined card holding the picked corners.
export default memo(function AnalyzeMetricPicker({
  selectedIds, combinedCorners, onSetSelected, onToggleTyreCorner, onToggleTyreCombined,
}: {
  selectedIds: ReadonlySet<string>
  /** Combined card id → its picked corner keys, for rows that are combined. */
  combinedCorners: ReadonlyMap<string, readonly string[]>
  onSetSelected: (metricIds: string[], selected: boolean) => void
  onToggleTyreCorner: (idPrefix: string, cornerKey: string) => void
  onToggleTyreCombined: (idPrefix: string) => void
}) {
  const [phase, setPhase] = useState<Phase>('closed')
  const [query, setQuery] = useState('')
  const [position, setPosition] = useState({ left: 8, top: 8, width: PANEL_WIDTH, maxHeight: 400 })
  const buttonRef = useRef<HTMLButtonElement>(null)
  const panelRef = useRef<HTMLDivElement>(null)
  const searchRef = useRef<HTMLInputElement>(null)
  const closeTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const open = phase === 'open'

  const normalizedQuery = query.trim().toLowerCase()

  const tiles = useMemo(() => new Map(ANALYZE_METRIC_CATEGORIES.map(entry => [
    entry,
    ANALYZE_METRICS.filter(metric =>
      metric.group === entry && !CORNER_METRIC_IDS.has(metric.id) &&
      matches(normalizedQuery, `${entry} ${metric.label} ${metric.unit}`),
    ),
  ])), [normalizedQuery])

  const tyreRows = useMemo(() => ANALYZE_TYRE_ROWS.filter(row =>
    matches(normalizedQuery, `tyres tires ${row.label} all ${CORNER_TERMS}`),
  ), [normalizedQuery])

  const sections = ANALYZE_METRIC_CATEGORIES.filter(entry =>
    (tiles.get(entry)?.length ?? 0) > 0 || (entry === 'Tyres' && tyreRows.length > 0),
  )

  const openPanel = () => {
    if (closeTimerRef.current !== null) clearTimeout(closeTimerRef.current)
    closeTimerRef.current = null
    setQuery('')
    setPhase('open')
  }

  const closePanel = (restoreFocus = false) => {
    if (restoreFocus) buttonRef.current?.focus()
    if (reduceAnimations()) { setPhase('closed'); return }
    setPhase(current => current === 'open' ? 'closing' : current)
    if (closeTimerRef.current !== null) clearTimeout(closeTimerRef.current)
    closeTimerRef.current = setTimeout(() => {
      closeTimerRef.current = null
      setPhase('closed')
    }, SELECT_MENU_ANIMATION_MS)
  }

  useEffect(() => () => {
    if (closeTimerRef.current !== null) clearTimeout(closeTimerRef.current)
  }, [])

  useLayoutEffect(() => {
    if (!open) return
    const place = () => {
      const rect = buttonRef.current?.getBoundingClientRect()
      if (!rect) return
      const top = rect.bottom + 4
      const width = Math.min(PANEL_WIDTH, window.innerWidth - 16)
      // Right-aligned to the button, so the panel opens back over the sidebar.
      setPosition({
        left: Math.max(8, Math.min(rect.right - width, window.innerWidth - width - 8)),
        width,
        top,
        maxHeight: Math.max(160, window.innerHeight - top - 8),
      })
    }
    place()
    searchRef.current?.focus()
    window.addEventListener('resize', place)
    return () => window.removeEventListener('resize', place)
  }, [open])

  useEffect(() => {
    if (!open) return
    const closeOutside = (event: PointerEvent) => {
      const target = event.target as Node
      if (!buttonRef.current?.contains(target) && !panelRef.current?.contains(target)) closePanel()
    }
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') closePanel(true)
    }
    document.addEventListener('pointerdown', closeOutside)
    document.addEventListener('keydown', closeOnEscape)
    return () => {
      document.removeEventListener('pointerdown', closeOutside)
      document.removeEventListener('keydown', closeOnEscape)
    }
  }, [open])

  const toggle = (metricId: string) => onSetSelected([metricId], !selectedIds.has(metricId))

  // Enter charts the first visible metric that is not charted yet.
  const addFirstMatch = () => {
    const candidates = sections.flatMap(entry => (tiles.get(entry) ?? []).map(metric => metric.id))
    const next = candidates.find(id => !selectedIds.has(id))
    if (next) onSetSelected([next], true)
  }

  const heading = (entry: Category) =>
    <div className="px-0.5 text-[9px] uppercase tracking-[0.1em] text-[var(--text-secondary)]">{entry}</div>

  const tile = (id: string, label: string) => {
    const on = selectedIds.has(id)
    return <button
      key={id}
      type="button"
      aria-pressed={on}
      title={`${on ? 'Remove' : 'Add'} ${label}`}
      onClick={() => toggle(id)}
      className={`analyze-toggle-button analyze-metric-tile min-h-8 px-2 py-1 rounded flex items-center justify-center text-center text-[10px] leading-tight break-words focus-visible:outline-none ${on ? 'analyze-toggle-button--active' : ''}`}
    >{label}</button>
  }

  const tyreSegment = (key: string, label: string, title: string, on: boolean, onClick: () => void) =>
    <button
      key={key}
      type="button"
      aria-pressed={on}
      title={title}
      onClick={onClick}
      className={`analyze-toggle-button analyze-metric-tile h-8 rounded text-[9px] tracking-wider focus-visible:outline-none ${on ? 'analyze-toggle-button--active' : ''}`}
    >{label}</button>

  const tyreMatrix = () => tyreRows.length > 0 && <div className="grid grid-cols-[1fr_repeat(5,36px)] gap-1 items-center">
    {tyreRows.flatMap(row => {
      const corners = combinedCorners.get(`${row.idPrefix}-all`)
      const combined = corners !== undefined
      return [
        <span key={`${row.idPrefix}-label`} className="px-0.5 truncate text-[10px] text-[var(--text-primary)]">{row.label}</span>,
        tyreSegment(
          `${row.idPrefix}-all`, 'ALL',
          combined ? `Show ${row.label} corners as separate graphs` : `Show ${row.label} corners together in one graph`,
          combined, () => onToggleTyreCombined(row.idPrefix),
        ),
        ...ANALYZE_TYRE_CORNERS.map(corner => {
          const on = combined ? corners.includes(corner.key) : selectedIds.has(`${row.idPrefix}-${corner.key}`)
          return tyreSegment(
            `${row.idPrefix}-${corner.key}`, corner.label,
            `${on ? 'Remove' : 'Add'} ${row.label} ${corner.label}`,
            on, () => onToggleTyreCorner(row.idPrefix, corner.key),
          )
        }),
      ]
    })}
  </div>

  return <>
    <button
      ref={buttonRef}
      type="button"
      title="Add Metrics"
      aria-label="Add Metrics"
      aria-haspopup="dialog"
      aria-expanded={open}
      onClick={() => open ? closePanel() : openPanel()}
      className={`w-7 h-7 rounded flex items-center justify-center shrink-0 transition-colors hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] ${
        open ? 'text-[var(--text-primary)] bg-[var(--bg-hover)]' : 'text-[var(--text-secondary)]'
      }`}
    ><Plus size={15} /></button>

    {phase !== 'closed' && createPortal(
      <div
        ref={panelRef}
        role="dialog"
        aria-label="Add Metrics"
        className="react-select-no-drag fixed z-[10000] flex flex-col rounded-md border border-[var(--border)] bg-[var(--bg-menu)] font-mono shadow-[0_8px_32px_rgba(0,0,0,0.6)] overflow-hidden select-none"
        style={{
          left: position.left, top: position.top, width: position.width, maxHeight: position.maxHeight,
          animation: reduceAnimations()
            ? undefined
            : `${open ? 'selectMenuEnterBottom' : 'selectMenuExitBottom'} ${SELECT_MENU_ANIMATION_MS}ms cubic-bezier(0.2, 0, 0, 1) both`,
          pointerEvents: open ? undefined : 'none',
        }}
      >
        <div className="p-2 border-b border-[var(--border)] shrink-0">
          <div className="relative">
            <Search size={12} className="absolute left-2.5 top-1/2 -translate-y-1/2 text-[var(--text-secondary)] pointer-events-none" />
            <input
              ref={searchRef}
              value={query}
              onChange={event => setQuery(event.target.value)}
              onKeyDown={event => { if (event.key === 'Enter') addFirstMatch() }}
              placeholder="Filter metrics"
              aria-label="Filter metrics"
              className="h-8 w-full rounded border border-[var(--border)] bg-[var(--bg-input)] pl-7 pr-2.5 text-[11px] text-[var(--text-primary)] outline-none transition-colors placeholder:text-[var(--text-secondary)] placeholder:opacity-80 focus:border-[var(--border-focus)]"
            />
          </div>
        </div>

        <div className="p-2 space-y-3 overflow-y-auto">
          {sections.map(entry => <div key={entry} role="group" aria-label={entry} className="space-y-1.5">
            {heading(entry)}
            {entry === 'Tyres' && tyreMatrix()}
            {(tiles.get(entry)?.length ?? 0) > 0 && <div className="grid grid-cols-3 gap-1">
              {(tiles.get(entry) ?? []).map(metric => tile(metric.id, metric.label))}
            </div>}
          </div>)}

          {sections.length === 0 && (
            <div className="px-2 py-3 text-center text-[11px] text-[var(--text-secondary)]">No metrics match “{query.trim()}”</div>
          )}
        </div>
      </div>,
      document.body,
    )}
  </>
})
