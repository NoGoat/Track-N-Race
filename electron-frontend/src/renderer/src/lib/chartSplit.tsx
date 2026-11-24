import {
  createContext, useCallback, useContext, useMemo, useRef, useState,
  type Dispatch, type ReactNode, type SetStateAction,
} from 'react'
import { createPortal } from 'react-dom'
import { LapDataSideProvider, useLapDataSide, type LapDataSide } from './lapDataSide'
import { ChartCursorSyncFileProvider } from './chartCursorSync'
import type { SecondaryLapState } from '../app/hooks/useSecondaryLap'

/*
 * Side-by-side charts while a second file is open (Overview 6, Inputs 5,
 * Power 6, Misc 5, Tyre graphs 6). Each chart splits on its own: one header
 * across the top (title, lap picker, "both files", legend), then file A's plot
 * on the left and file B's copy of the same lap on the right.
 */

const SecondaryLapContext = createContext<SecondaryLapState | null>(null)

export function SecondaryLapProvider({ value, children }: { value: SecondaryLapState | null; children: ReactNode }) {
  return <SecondaryLapContext.Provider value={value}>{children}</SecondaryLapContext.Provider>
}

export function useSecondaryLapState(): SecondaryLapState | null {
  return useContext(SecondaryLapContext)
}

const SplitRegionContext = createContext(false)

/**
 * Marks a page region whose charts split into A | B while a second file is
 * open. With one file open it renders its children unchanged.
 */
export function FileSplitRegion({ children }: { children: ReactNode }) {
  return <SplitRegionContext.Provider value={true}>{children}</SplitRegionContext.Provider>
}

/** The second file's lap when charts here split, otherwise null. */
export function useChartSplit(): SecondaryLapState | null {
  const secondary = useContext(SecondaryLapContext)
  return useContext(SplitRegionContext) ? secondary : null
}

interface SplitChartValue {
  /** Where side A portals the chart's one header. */
  slot: HTMLDivElement | null
  /** Where side B portals its own window selector, over B's half of that header. */
  slotB: HTMLDivElement | null
  /** Legend state both halves share, so a toggle hides the series on both. */
  legend: unknown
  setLegend: Dispatch<SetStateAction<unknown>>
}

const SplitChartContext = createContext<SplitChartValue | null>(null)

const SIDE_A: LapDataSide = { side: 'A' }

/** True inside a page region whose charts split while a second file is open. */
export function useInSplitRegion(): boolean {
  return useContext(SplitRegionContext)
}

/**
 * One chart, rendered for A and for B under a single shared header.
 *
 * Rendered whether or not B is open, with the same elements either way, so
 * toggling side-by-side keeps file A's chart mounted (no rebuild). With
 * `secondary` null the wrappers are `display: contents`, so the chart lays out
 * exactly as before, and B's half is simply absent.
 */
export function ChartSplit({ secondary, children }: { secondary: SecondaryLapState | null; children: ReactNode }) {
  const [slot, setSlot] = useState<HTMLDivElement | null>(null)
  const [slotB, setSlotB] = useState<HTMLDivElement | null>(null)
  const [legend, setLegend] = useState<unknown>(undefined)
  const split = secondary !== null
  const shared = useMemo<SplitChartValue | null>(
    () => split ? { slot, slotB, legend, setLegend } : null,
    [legend, slot, slotB, split],
  )
  const lapNum = secondary?.lapNum ?? null
  const lapData = secondary?.lapData ?? null
  // B plays back beside A: its half draws up to B's place on the lap-synced playhead.
  const sideB = useMemo<LapDataSide>(() => ({ side: 'B', lapNum, lapData }), [lapData, lapNum])
  return (
    <SplitChartContext.Provider value={shared}>
      <div className={split ? 'file-split-chart flex-1 min-w-0 min-h-0 h-full flex flex-col bg-[var(--bg-panel)]' : 'contents'}>
        {/* One header across both halves (A's title and legend), with B's own
            window selector at the start of B's half. */}
        <div className={split ? 'relative shrink-0' : 'hidden'}>
          <div ref={setSlot} className="file-split-header" />
          <div ref={setSlotB} className="file-split-header-b absolute flex items-center" />
        </div>
        <div className={split ? 'flex-1 min-h-0 flex divide-x divide-[var(--border)]' : 'contents'}>
          <div className={split ? 'file-split-half flex-1 min-w-0 min-h-0 flex flex-col' : 'contents'}>
            <LapDataSideProvider value={split ? SIDE_A : null}>{children}</LapDataSideProvider>
          </div>
          {secondary && (
            <div className="file-split-half flex-1 min-w-0 min-h-0 flex flex-col relative">
              {/* B's half syncs with A's by lap distance, never by session time. */}
              {/* B's window scope draws "No lap N" itself, in the lap windows only. */}
              <ChartCursorSyncFileProvider>
                <LapDataSideProvider value={sideB}>{children}</LapDataSideProvider>
              </ChartCursorSyncFileProvider>
            </div>
          )}
        </div>
      </div>
    </SplitChartContext.Provider>
  )
}

/**
 * A chart's header row. Split, side A draws it once across both halves; side
 * B's stays mounted but hidden, so its window selector can portal itself into
 * the shared header over B's half. Otherwise it renders in place as before.
 */
export function ChartHeader({ className, children }: { className: string; children: ReactNode }) {
  const split = useContext(SplitChartContext)
  const side = useLapDataSide()
  if (!split || !side) return <div className={className}>{children}</div>
  if (side.side === 'B') return <div className="hidden">{children}</div>
  if (!split.slot) return null
  return createPortal(<div className={className}>{children}</div>, split.slot)
}

/** Where B's half of a split chart puts its window selector (null when not split). */
export function useChartHeaderSlotB(): HTMLDivElement | null {
  return useContext(SplitChartContext)?.slotB ?? null
}

/** The split header's mount point (null when not split or not mounted yet). */
export function useChartHeaderSlot(): HTMLDivElement | null {
  return useContext(SplitChartContext)?.slot ?? null
}

/**
 * A chart's legend (hidden/visible series) state. Split, both halves share one
 * copy; otherwise it is plain local state.
 */
export function useSplitLegendState<T>(initial: T): [T, Dispatch<SetStateAction<T>>] {
  const local = useState<T>(initial)
  const split = useContext(SplitChartContext)
  const initialRef = useRef(initial)
  const setLegend = split?.setLegend
  const setShared = useCallback((action: SetStateAction<T>) => {
    setLegend?.((prev: unknown) => {
      const current = (prev === undefined ? initialRef.current : prev) as T
      return typeof action === 'function' ? (action as (value: T) => T)(current) : action
    })
  }, [setLegend])
  if (!split) return local
  return [(split.legend === undefined ? initialRef.current : split.legend) as T, setShared]
}
