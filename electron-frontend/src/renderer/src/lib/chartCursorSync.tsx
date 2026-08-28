import { createContext, useContext, useEffect, useMemo, useRef } from 'react'
import { ChartTooltipPortal, useChartTooltip } from '../hooks/useChartTooltip'

export interface ChartCursorSyncParticipant {
  id: string
  order: number
  axisKind: 'time' | 'distance'
  resolveAxisX: (axisX: number) => { sessionTime: number; sampledAxisX: number } | null
  formatAxisX: (axisX: number) => string
  syncToSessionTime: (sessionTime: number, axisX: number, sourceAxisKind: 'time' | 'distance', source: boolean) => {
    current: string
    comparison?: string
    comparisonLabel?: string
    comparisonKey?: string
  }
  clear: () => void
}

interface ChartCursorSyncContextValue {
  isEnabled: () => boolean
  register: (participant: ChartCursorSyncParticipant) => () => void
  publish: (sourceId: string, axisX: number, clientX: number, clientY: number) => void
  clear: (sourceId: string) => void
}

const ChartCursorSyncContext = createContext<ChartCursorSyncContextValue | null>(null)

export function useChartCursorSync() {
  return useContext(ChartCursorSyncContext)
}

export function ChartCursorSyncProvider({ enabled, children }: { enabled: boolean; children: React.ReactNode }) {
  const boundaryRef = useRef<HTMLDivElement>(null)
  const enabledRef = useRef(enabled)
  const participantsRef = useRef(new Map<string, ChartCursorSyncParticipant>())
  const activeSourceRef = useRef<string | null>(null)
  const { tooltipRef, show, hide } = useChartTooltip(boundaryRef)
  enabledRef.current = enabled

  const value = useMemo<ChartCursorSyncContextValue>(() => {
    const clearAll = () => {
      activeSourceRef.current = null
      for (const participant of participantsRef.current.values()) participant.clear()
      hide()
    }
    return {
      isEnabled: () => enabledRef.current,
      register: participant => {
        participantsRef.current.set(participant.id, participant)
        return () => {
          participant.clear()
          participantsRef.current.delete(participant.id)
          if (activeSourceRef.current === participant.id) clearAll()
        }
      },
      publish: (sourceId, axisX, clientX, clientY) => {
        if (!enabledRef.current) return
        const source = participantsRef.current.get(sourceId)
        const resolved = source?.resolveAxisX(axisX)
        if (!source || !resolved) return clearAll()
        const { sessionTime, sampledAxisX } = resolved
        activeSourceRef.current = sourceId
        const samples = [...participantsRef.current.values()]
          .sort((a, b) => a.order - b.order)
          .map(participant => participant.syncToSessionTime(sessionTime, axisX, source.axisKind, participant.id === sourceId))
        const fragments = samples.map(sample => sample.current).filter(Boolean)
        const comparisonGroups = new Map<string, { label: string; fragments: string[] }>()
        for (const sample of samples) {
          if (!sample.comparison || !sample.comparisonLabel) continue
          const key = sample.comparisonKey ?? sample.comparisonLabel
          const group = comparisonGroups.get(key)
          if (group) group.fragments.push(sample.comparison)
          else comparisonGroups.set(key, { label: sample.comparisonLabel, fragments: [sample.comparison] })
        }
        const boundary = boundaryRef.current?.getBoundingClientRect()
        if (!boundary || (fragments.length === 0 && comparisonGroups.size === 0)) return clearAll()
        const header = `<div style="color:var(--text-secondary);margin-bottom:4px">${source.formatAxisX(sampledAxisX)}</div>`
        const comparison = [...comparisonGroups.values()].map(group =>
          `<div style="color:var(--text-secondary);border-top:1px solid var(--border);margin-top:5px;padding-top:4px">${group.label}</div><div style="opacity:0.35">${group.fragments.join('')}</div>`,
        ).join('')
        show(`${header}${fragments.join('')}${comparison}`, clientX - boundary.left, clientY - boundary.top)
      },
      clear: sourceId => {
        if (activeSourceRef.current === sourceId) clearAll()
      },
    }
  }, [hide, show])

  useEffect(() => {
    if (enabled) return
    activeSourceRef.current = null
    for (const participant of participantsRef.current.values()) participant.clear()
    hide()
  }, [enabled, hide])

  return <ChartCursorSyncContext.Provider value={value}>
    <div ref={boundaryRef} className="relative h-full min-h-0">
      {children}
    </div>
    <ChartTooltipPortal tooltipRef={tooltipRef} />
  </ChartCursorSyncContext.Provider>
}
