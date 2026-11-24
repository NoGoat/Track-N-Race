import { createContext, useContext, type ReactNode } from 'react'
import type { AnalyzeLapData, DamageRow, LapRow, StatusRow, TelemetryRow } from '../types'

/** A row header cell on the Tyres rack: the followed driver and "race · 71 laps". */
export interface FileHeaderCell {
  driver: string | null
  detail: string
}

/**
 * File B's live rows at its moment on the lap-synced playhead, for the table
 * cards (Cards 5): the cards run the same resolvers on these that they run on
 * file A's latest rows. Present only while side-by-side is on.
 */
export interface SecondaryLiveRows {
  /** Side-by-side is on: the table cards show both files. Off, only the Tyres rack does. */
  split: boolean
  dateA: string
  dateB: string
  headerA: FileHeaderCell
  headerB: FileHeaderCell
  latest: TelemetryRow | null
  status: StatusRow | null
  lap: LapRow | null
  damage: DamageRow | null
}

const SecondaryLiveContext = createContext<SecondaryLiveRows | null>(null)

export function SecondaryLiveProvider({ value, children }: { value: SecondaryLiveRows | null; children: ReactNode }) {
  return <SecondaryLiveContext.Provider value={value}>{children}</SecondaryLiveContext.Provider>
}

export function useSecondaryLive(): SecondaryLiveRows | null {
  return useContext(SecondaryLiveContext)
}

/** B's rows for the table cards: only while side-by-side is on. */
export function useSecondaryCards(): SecondaryLiveRows | null {
  const live = useContext(SecondaryLiveContext)
  return live?.split ? live : null
}

/** File A's own header cell (date, driver, "race · 71 laps"), for the rack with one file or live. */
export interface PrimaryHeader {
  date: string
  header: FileHeaderCell
}

const PrimaryHeaderContext = createContext<PrimaryHeader | null>(null)

export function PrimaryHeaderProvider({ value, children }: { value: PrimaryHeader | null; children: ReactNode }) {
  return <PrimaryHeaderContext.Provider value={value}>{children}</PrimaryHeaderContext.Provider>
}

export function usePrimaryHeader(): PrimaryHeader | null {
  return useContext(PrimaryHeaderContext)
}

/**
 * Row indices of B's newest rows, one per row kind (-1: none yet). B's lap
 * comes from its stream, so its rows end at its moment on the playhead.
 */
export function secondaryRowIndices(lapData: AnalyzeLapData) {
  return {
    telemetry: lapData.telemetry.length - 1,
    status: lapData.statusHistory.length - 1,
    lap: lapData.lapProgress.length - 1,
    damage: lapData.damageHistory.length - 1,
  }
}

/** Materialises the rows those indices point at. */
export function secondaryRowsAt(lapData: AnalyzeLapData, indices: ReturnType<typeof secondaryRowIndices>) {
  return {
    latest: indices.telemetry >= 0 ? lapData.telemetry.row(indices.telemetry) : null,
    status: indices.status >= 0 ? lapData.statusHistory.row(indices.status) : null,
    lap: indices.lap >= 0 ? lapData.lapProgress.row(indices.lap) as unknown as LapRow : null,
    damage: indices.damage >= 0 ? lapData.damageHistory.row(indices.damage) : null,
  }
}

/** The resolvers' missing value ('-') as the table's ('—'). */
export function tableValue(value: string | undefined): string {
  return !value || value === '-' ? '—' : value
}
