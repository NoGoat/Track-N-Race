import { memo, useEffect, useLayoutEffect, useMemo, useRef, useState, type ReactNode } from 'react'
import { createPortal } from 'react-dom'
import { Plus, Search } from 'lucide-react'
import {
  ANALYZE_METRICS, ANALYZE_METRIC_CATEGORIES, ANALYZE_TYRE_CORNERS, ANALYZE_TYRE_ROWS,
} from '../lib/analyzeMetrics'
import { SELECT_MENU_ANIMATION_MS } from '../lib/selectStyles'

type Phase = 'closed' | 'open' | 'closing'

const PANEL_WIDTH = 266
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

// Each category is a divider-separated block of wrapping toggle chips. Per-corner
// tyre metrics form a small matrix: FL / FR / RL / RR pick corners, ALL picks
// or clears all four, and COM (combined) switches the row between a card per
// corner and one combined card holding the picked corners.
export default memo(function AnalyzeMetricPicker({
  selectedIds, combinedCorners, onSetSelected, onToggleTyreCorner, onToggleTyreAllCorners, onToggleTyreCombined,
}: {
  selectedIds: ReadonlySet<string>
  /** Combined card id → its picked corner keys, for rows that are combined. */
  combinedCorners: ReadonlyMap<string, readonly string[]>
  onSetSelected: (metricIds: string[], selected: boolean) => void
  onToggleTyreCorner: (idPrefix: string, cornerKey: string) => void
  /** Picks all four corners of a row, or clears them when all are picked. */
  onToggleTyreAllCorners: (idPrefix: string) => void
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
    matches(normalizedQuery, `tyres tires ${row.label} all com combined ${CORNER_TERMS}`),
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

  const chip = (id: string, label: string) => {
    const on = selectedIds.has(id)
    return <button
      key={id}
      type="button"
      aria-pressed={on}
      title={`${on ? 'Remove' : 'Add'} ${label}`}
      onClick={() => toggle(id)}
      className={`h-6 px-2 rounded border text-[10px] whitespace-nowrap transition-colors focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-[var(--color-info)] ${
        on
          ? 'border-[var(--border-focus)] bg-[var(--border-focus)] text-white'
          : 'border-[var(--border)] text-[var(--text-secondary)] hover:border-[var(--border-focus)] hover:text-[var(--text-primary)]'
      }`}
    >{label}</button>
  }

  const tyreCell = (key: string, title: string, on: boolean, onClick: () => void) =>
    <button
      key={key}
      type="button"
      aria-pressed={on}
      title={title}
      onClick={onClick}
      className={`w-[22px] h-5 rounded border transition-colors focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-[var(--color-info)] ${
        on
          ? 'border-[var(--border-focus)] bg-[var(--border-focus)]'
          : 'border-[var(--border)] hover:border-[var(--border-focus)] hover:bg-[var(--bg-hover)]'
      }`}
    />

  const columnLabel = (label: string) =>
    <span key={label} className="w-[22px] text-center text-[8px] tracking-wider text-[var(--text-secondary)]">{label}</span>

  const tyreMatrix = () => tyreRows.length > 0 && <div className="grid grid-cols-[auto_repeat(6,22px)] gap-x-1 gap-y-1 items-center justify-start">
    <span />
    {columnLabel('COM')}
    {ANALYZE_TYRE_CORNERS.map(corner => columnLabel(corner.label))}
    {columnLabel('ALL')}
    {tyreRows.flatMap(tyreRow => {
      const corners = combinedCorners.get(`${tyreRow.idPrefix}-all`)
      const combined = corners !== undefined
      const picked = ANALYZE_TYRE_CORNERS.map(corner =>
        combined ? corners.includes(corner.key) : selectedIds.has(`${tyreRow.idPrefix}-${corner.key}`))
      const allPicked = picked.every(Boolean)
      return [
        <span
          key={`${tyreRow.idPrefix}-label`}
          className={`pr-2 text-[10px] whitespace-nowrap ${picked.some(Boolean) ? 'text-[var(--text-primary)]' : 'text-[var(--text-secondary)]'}`}
        >{tyreRow.label}</span>,
        tyreCell(
          `${tyreRow.idPrefix}-all`,
          combined ? `Show ${tyreRow.label} corners as separate graphs` : `Show ${tyreRow.label} corners together in one graph`,
          combined, () => onToggleTyreCombined(tyreRow.idPrefix),
        ),
        ...ANALYZE_TYRE_CORNERS.map((corner, index) => tyreCell(
          `${tyreRow.idPrefix}-${corner.key}`,
          `${picked[index] ? 'Remove' : 'Add'} ${tyreRow.label} ${corner.label}`,
          picked[index],
          () => onToggleTyreCorner(tyreRow.idPrefix, corner.key),
        )),
        tyreCell(
          `${tyreRow.idPrefix}-every`,
          `${allPicked ? 'Remove' : 'Add'} all ${tyreRow.label} corners`,
          allPicked, () => onToggleTyreAllCorners(tyreRow.idPrefix),
        ),
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
              className="h-7 w-full rounded border border-[var(--border)] bg-[var(--bg-input)] pl-7 pr-2.5 text-[11px] text-[var(--text-primary)] outline-none transition-colors placeholder:text-[var(--text-secondary)] placeholder:opacity-80 focus:border-[var(--border-focus)]"
            />
          </div>
        </div>

        <div className="pb-1 overflow-y-auto">
          {/* The tyre matrix and the whole-car tyre averages are separate blocks. */}
          {sections.flatMap(entry => {
            const entryTiles = tiles.get(entry) ?? []
            const blocks: { key: string, label: string, content: ReactNode }[] = []
            if (entry === 'Tyres' && tyreRows.length > 0) blocks.push({ key: 'Tyres-corners', label: 'Tyres by corner', content: tyreMatrix() })
            if (entryTiles.length > 0) blocks.push({
              key: entry,
              label: entry === 'Tyres' ? 'Tyre averages' : entry,
              content: <div className="flex flex-wrap gap-1">{entryTiles.map(metric => chip(metric.id, metric.label))}</div>,
            })
            return blocks
          }).map((block, index) => <div
            key={block.key}
            role="group"
            aria-label={block.label}
            className={`px-3 py-2.5 ${index > 0 ? 'border-t border-[var(--border)]' : ''}`}
          >{block.content}</div>)}

          {sections.length === 0 && (
            <div className="px-2 py-4 text-center text-[11px] text-[var(--text-secondary)]">No metrics match “{query.trim()}”</div>
          )}
        </div>
      </div>,
      document.body,
    )}
  </>
})
