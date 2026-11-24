import { createContext, useContext, useMemo, useRef, type ReactNode } from 'react'
import type { AnalyzeLapData } from '../types'
import { emptyView, type ColumnView } from './columnStore'

/**
 * Which file a chart is drawing when two recordings are open.
 *  - 'A': the main recording, read from the telemetry store as usual, but kept
 *    on a lap-distance axis so it lines up with B.
 *  - 'B': the second recording's rows for the chart's window, from B's stream
 *    (lib/secondaryStream.ts), so they run to B's playhead and B plays back
 *    beside A; or a complete comparison lap.
 * Charts outside any provider (one file open) behave exactly as before.
 */
export type LapDataSide =
  | { side: 'A' }
  | { side: 'B'; lapNum: number | null; lapData: AnalyzeLapData | null }

const Context = createContext<LapDataSide | null>(null)

/** `null`: not split (one file open), the chart behaves as before. */
export function LapDataSideProvider({ value, children }: { value: LapDataSide | null; children: ReactNode }) {
  return <Context.Provider value={value}>{children}</Context.Provider>
}

export function useLapDataSide(): LapDataSide | null {
  return useContext(Context)
}

type LapRowsKey = 'telemetry' | 'motion' | 'motionEx' | 'statusHistory' | 'damageHistory'
const ROW_TYPE: Record<LapRowsKey, string> = {
  telemetry: 'telemetry', motion: 'motion', motionEx: 'motion_ex', statusHistory: 'status', damageHistory: 'damage',
}

/**
 * The rows a chart should draw: on B's side, B's rows for its window;
 * otherwise the rows the chart already selected from the store. The hooks run
 * every render, so this is safe to call unconditionally.
 */
export function useSideRows<V extends ColumnView<{ session_time: number }>>(key: LapRowsKey, storeRows: V): V {
  const side = useContext(Context)
  const full = side?.side === 'B' ? side.lapData?.[key] ?? emptyView(ROW_TYPE[key]) : null
  // A chart with zero rows swaps itself for its "No data" placeholder and is
  // rebuilt when rows return: a flicker at every lap line, where B's new lap
  // has no rows yet. So B's half never hands over zero rows while B has had
  // any: until the new lap's first row arrives, the first sample of the one
  // before.
  const lastRef = useRef<ColumnView<{ session_time: number }> | null>(null)
  // eslint-disable-next-line react-hooks/refs -- the last rows this chart drew, kept for the gap
  if (full && full.length > 0) lastRef.current = full
  // eslint-disable-next-line react-hooks/refs
  const held = full && full.length === 0 ? lastRef.current : null
  const rows = useMemo(() => held ? held.slice(0, 1) : full, [held, full])
  return (rows ?? storeRows) as unknown as V
}
