import { memo, useEffect, useId, useMemo, useRef, useState, type PointerEvent, type KeyboardEvent } from 'react'
import { GripVertical } from 'lucide-react'
import { useAppConfig } from '../hooks/useAppConfig'
import type { AnalyzeLapData, StatusRow, TelemetryRow } from '../types'
import type { ColumnView } from '../lib/columnStore'

interface Props {
  current: AnalyzeLapData | null
  comparison: AnalyzeLapData | null
  currentColor: string
  comparisonColor: string
  currentLabel: string
  comparisonLabel: string
  elapsedSource: () => number
}

interface Position { x: number; y: number }

interface ComparisonSample {
  telemetry: TelemetryRow | null
  status: StatusRow | null
  // Which rows these are; an unchanged source keeps the same sample object.
  source: readonly [unknown, number, unknown, number] | null
}

const EMPTY_SAMPLE: ComparisonSample = { telemetry: null, status: null, source: null }

function clampPosition(value: number): number {
  return Math.max(0, Math.min(1, value))
}

function readPosition(value: unknown): Position {
  // Preserve positions saved by the previous corner selector.
  if (typeof value === 'string') return {
    x: value === 'top-right' || value === 'bottom-right' ? 1 : 0,
    y: value === 'bottom-left' || value === 'bottom-right' ? 1 : 0,
  }
  if (value && typeof value === 'object' && 'x' in value && 'y' in value &&
    typeof value.x === 'number' && Number.isFinite(value.x) &&
    typeof value.y === 'number' && Number.isFinite(value.y)) {
    return { x: clampPosition(value.x), y: clampPosition(value.y) }
  }
  return { x: 0, y: 0 }
}

function rowIndexAt(rows: ColumnView<any>, target: number): number {
  if (rows.length === 0) return -1
  return Math.max(0, rows.lowerBound(target, false) - 1)
}

// Hold each source's latest sample at the map cursor, including discrete gear
// and ERS updates. Clamp each lap independently, as the map does its marker.
// Rows are materialised only when the sampled row actually changes.
function sampleAt(lap: AnalyzeLapData | null, elapsed: number, previous?: ComparisonSample): ComparisonSample {
  if (!lap) return EMPTY_SAMPLE
  const target = lap.startSessionTime + Math.max(0, Math.min(
    Math.max(0, lap.endSessionTime - lap.startSessionTime), elapsed,
  ))
  const telemetryIndex = rowIndexAt(lap.telemetry, target)
  const statusIndex = rowIndexAt(lap.statusHistory, target)
  const source = previous?.source
  if (source && source[0] === lap.telemetry && source[1] === telemetryIndex &&
      source[2] === lap.statusHistory && source[3] === statusIndex) return previous
  return {
    telemetry: telemetryIndex >= 0 ? lap.telemetry.row(telemetryIndex) : null,
    status: statusIndex >= 0 ? lap.statusHistory.row(statusIndex) : null,
    source: [lap.telemetry, telemetryIndex, lap.statusHistory, statusIndex],
  }
}

function finite(value: number | undefined): number | null {
  return value !== undefined && Number.isFinite(value) ? value : null
}

const InputBars = memo(function InputBars({ label, field, samples, colors, labels }: {
  label: string
  field: 'steering' | 'brake' | 'throttle' | 'ers_pct'
  samples: readonly ComparisonSample[]
  colors: readonly string[]
  labels: readonly string[]
}) {
  const signed = field === 'steering'
  return <div className="analyze-input-comparison__row">
    <span className="text-[var(--text-secondary)]">{label}</span>
    <div className={`analyze-input-comparison__bars ${signed ? 'analyze-input-comparison__bars--signed' : ''}`}>
      {samples.map((sample, index) => {
        const raw = finite(field === 'ers_pct'
          ? sample.status ? sample.status.ers_pct / 100 : undefined
          : sample.telemetry?.[field])
        const value = raw === null ? null : Math.max(signed ? -1 : 0, Math.min(1, raw))
        const reading = value === null ? 'No data' : signed
          ? `${value < 0 ? 'L ' : value > 0 ? 'R ' : ''}${Math.round(Math.abs(value) * 100)}%`
          : `${Math.round(value * 100)}%`
        return <div key={index} className="analyze-input-comparison__lane" role="img"
          aria-label={`${labels[index]} ${label}: ${reading}`} title={`${labels[index]}: ${reading}`}>
          {value === null
            ? <span className="analyze-input-comparison__missing">—</span>
            : <span className="analyze-input-comparison__fill" style={{
              backgroundColor: colors[index],
              left: `${signed ? 50 + Math.min(0, value) * 50 : 0}%`,
              width: `${Math.abs(value) * (signed ? 50 : 100)}%`,
            }} />}
        </div>
      })}
    </div>
  </div>
})

// The map around this overlay re-renders with the analysis screen; the readings
// here update from their own sampling timer instead.
export default memo(function AnalyzeInputComparison({
  current, comparison, currentColor, comparisonColor, currentLabel, comparisonLabel, elapsedSource,
}: Props) {
  const [collapsed, setCollapsed] = useState(false)
  const [storedPosition, savePosition] = useAppConfig<Position | string>('analyzeInputComparisonPosition', { x: 0, y: 0 })
  const [position, setPosition] = useState(() => readPosition(storedPosition))
  const [dragging, setDragging] = useState(false)
  const panelRef = useRef<HTMLElement>(null)
  const dragRef = useRef<{
    pointerId: number; offsetX: number; offsetY: number; start: Position; latest: Position
  } | null>(null)

  const startDrag = (event: PointerEvent<HTMLButtonElement>) => {
    if (!event.isPrimary || event.button !== 0 || dragRef.current || !panelRef.current) return
    const rect = panelRef.current.getBoundingClientRect()
    event.preventDefault()
    event.currentTarget.focus()
    event.currentTarget.setPointerCapture(event.pointerId)
    dragRef.current = {
      pointerId: event.pointerId, offsetX: event.clientX - rect.left, offsetY: event.clientY - rect.top,
      start: position, latest: position,
    }
    setDragging(true)
  }

  const moveDrag = (event: PointerEvent<HTMLButtonElement>) => {
    const drag = dragRef.current
    const panel = panelRef.current
    const map = panel?.parentElement
    if (!drag || drag.pointerId !== event.pointerId || !panel || !map) return
    const rect = map.getBoundingClientRect()
    const width = map.clientWidth - panel.offsetWidth - 16
    const height = map.clientHeight - panel.offsetHeight - 16
    const next = {
      x: width > 0 ? clampPosition((event.clientX - rect.left - drag.offsetX - 8) / width) : 0,
      y: height > 0 ? clampPosition((event.clientY - rect.top - drag.offsetY - 8) / height) : 0,
    }
    drag.latest = next
    setPosition(next)
  }

  const finishDrag = (event: PointerEvent<HTMLButtonElement>, cancelled = false) => {
    const drag = dragRef.current
    if (!drag || drag.pointerId !== event.pointerId) return
    dragRef.current = null
    setDragging(false)
    if (cancelled) setPosition(drag.start)
    else savePosition(drag.latest)
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId)
    }
  }

  const moveWithKeyboard = (event: KeyboardEvent<HTMLButtonElement>) => {
    const panel = panelRef.current
    const map = panel?.parentElement
    if (dragRef.current || !panel || !map) return
    const step = event.shiftKey ? 1 : 10
    const dx = event.key === 'ArrowLeft' ? -step : event.key === 'ArrowRight' ? step : 0
    const dy = event.key === 'ArrowUp' ? -step : event.key === 'ArrowDown' ? step : 0
    if (!dx && !dy) return
    event.preventDefault()
    const next = {
      x: clampPosition(position.x + dx / Math.max(1, map.clientWidth - panel.offsetWidth - 16)),
      y: clampPosition(position.y + dy / Math.max(1, map.clientHeight - panel.offsetHeight - 16)),
    }
    setPosition(next)
    savePosition(next)
  }
  const contentId = useId()
  const [samples, setSamples] = useState<readonly [ComparisonSample, ComparisonSample]>(() => {
    const elapsed = elapsedSource()
    return [sampleAt(current, elapsed), sampleAt(comparison, elapsed)]
  })

  useEffect(() => {
    if (collapsed) return
    const update = () => {
      const elapsed = elapsedSource()
      setSamples(previous => {
        const primary = sampleAt(current, elapsed, previous[0])
        const reference = sampleAt(comparison, elapsed, previous[1])
        return previous[0] === primary && previous[1] === reference ? previous : [primary, reference]
      })
    }
    update()
    // Keep updates local to this small overlay; the map renders independently.
    const timer = window.setInterval(update, 50)
    return () => window.clearInterval(timer)
  }, [collapsed, comparison, current, elapsedSource])

  const colors = useMemo(() => [currentColor, comparisonColor], [comparisonColor, currentColor])
  const labels = useMemo(
    () => [currentLabel || 'Current lap', comparisonLabel || 'Comparison lap'],
    [comparisonLabel, currentLabel])

  return <section ref={panelRef} className="analyze-input-comparison" data-dragging={dragging} aria-label="Comparison"
    style={{
      // Relative positions adapt to map resizing and the panel's animated height.
      left: `calc(${position.x * 100}% + ${8 - position.x * 16}px)`,
      top: `calc(${position.y * 100}% + ${8 - position.y * 16}px)`,
      transform: `translate(${-position.x * 100}%, ${-position.y * 100}%)`,
    }}>
    <div className="analyze-input-comparison__header">
      <button type="button" className="analyze-input-comparison__drag-handle"
        onPointerDown={startDrag} onPointerMove={moveDrag}
        onPointerUp={event => finishDrag(event)}
        onPointerCancel={event => finishDrag(event, true)}
        onLostPointerCapture={event => finishDrag(event, true)}
        onKeyDown={moveWithKeyboard}
        aria-label="Move comparison. Drag or use arrow keys; hold Shift for fine adjustments."
        title="Drag to move · Arrow keys to adjust position">
        <GripVertical size={12} aria-hidden="true" />
        <span>Data Comparison</span>
      </button>
      <button type="button" className="analyze-input-comparison__action"
        onClick={() => setCollapsed(previous => !previous)}
        aria-expanded={!collapsed} aria-controls={contentId}
        aria-label={collapsed ? 'Expand data comparison' : 'Collapse data comparison'}
        title={collapsed ? 'Expand data comparison' : 'Collapse data comparison'}>
        <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor"
          strokeWidth="2" strokeLinecap="round" aria-hidden="true" focusable="false">
          <path d="M5 12h14" />
          <path d="M12 5v14" className="analyze-input-comparison__toggle-stroke" />
        </svg>
      </button>
    </div>
    <div id={contentId} className={`analyze-input-comparison__body ${collapsed ? '' : 'analyze-input-comparison__body--expanded'}`}
      aria-hidden={collapsed} inert={collapsed}>
      <div className="analyze-input-comparison__body-inner">
        <div className="analyze-input-comparison__content">
          <InputBars label="Steering" field="steering" samples={samples} colors={colors} labels={labels} />
          <InputBars label="Brake" field="brake" samples={samples} colors={colors} labels={labels} />
          <InputBars label="Throttle" field="throttle" samples={samples} colors={colors} labels={labels} />
          <InputBars label="ERS" field="ers_pct" samples={samples} colors={colors} labels={labels} />
          {(['speed_kph', 'gear'] as const).map(field => {
            const fieldLabel = field === 'speed_kph' ? 'Speed' : 'Gear'
            const unit = field === 'speed_kph' ? 'km/h' : ''
            return <div key={field} className="analyze-input-comparison__row">
              <span className="text-[var(--text-secondary)]">{fieldLabel}</span>
              <div className="analyze-input-comparison__values">
                {samples.map((sample, index) => {
                  const value = finite(sample.telemetry?.[field])
                  const reading = value === null ? '—' : field === 'gear'
                    ? value < 0 ? 'R' : value === 0 ? 'N' : String(Math.round(value))
                    : String(Math.round(value))
                  return <span key={index} style={{ color: colors[index] }}
                    aria-label={`${labels[index]} ${fieldLabel}: ${value === null ? 'No data' : `${reading}${unit ? ` ${unit}` : ''}`}`}>
                    {reading}
                  </span>
                })}
                <span className="text-[9px] font-normal text-[var(--text-secondary)]">{unit}</span>
              </div>
            </div>
          })}
        </div>
      </div>
    </div>
  </section>
})