// The second recording ("file B") that every page can compare against.
// Loaded once from the title bar; Analysis and the other pages read the same
// SecondaryFileData. The engine side lives behind window.analysisBridge.

import type { AnalysisDriverLapCatalog, AnalyzeLapData, DamageRow, LapProgressPoint, MotionExRow, MotionRow, StatusRow, TelemetryRow } from '../types'
import { decodeV6History, emptyView, isV6HistoryPayload, type ColumnView, type HistoryFamily } from './columnStore'
import { parseAnalyzeLapData } from './analyzeLapData'
import type { LapBlock, SecondaryFileData } from '../components/AnalyzeScreen'

/** What analysis:load-file returns in `data`. */
export interface AnalysisFilePayload {
  laps?: Array<{ lapNum: number; lapTimeMs: number }>
  blocks?: LapBlock[]
  fastestLapNum?: number
  lapDistanceAvailable?: boolean
  deltaAvailable?: boolean
  analysisDrivers?: AnalysisDriverLapCatalog[]
}

/** A loaded file waiting on the user because its circuit differs from file A's. */
export interface PendingCircuitMismatch {
  filePath: string
  data: AnalysisFilePayload
  trackId: number | null
  trackName: string
}

export function toSecondaryFileData(
  filePath: string,
  data: AnalysisFilePayload,
  trackId: number | null,
  trackName: string | null,
): SecondaryFileData {
  const times: Record<number, number> = {}
  for (const lap of data?.laps ?? []) {
    if (Number.isFinite(lap.lapNum) && Number.isFinite(lap.lapTimeMs) && lap.lapTimeMs > 0) {
      times[lap.lapNum] = lap.lapTimeMs
    }
  }
  return {
    filename: filePath.split(/[\\/]/).pop() ?? filePath,
    filePath,
    trackId,
    trackName,
    blocks: Array.isArray(data?.blocks) ? data.blocks : [],
    fastestLapNum: typeof data?.fastestLapNum === 'number' && Number.isFinite(data.fastestLapNum) ? data.fastestLapNum : null,
    lapTimesByNum: times,
    deltaAvailable: data?.lapDistanceAvailable === true || data?.deltaAvailable === true,
    analysisDrivers: Array.isArray(data?.analysisDrivers) ? data.analysisDrivers : [],
    driverIndex: -1,
  }
}

/**
 * File B as followed through one of its cars: that car's lap catalog in place
 * of the recorded player's, so laps stay in step by that driver's own lap
 * count. -1, or a car the file holds no catalog for, is the file as loaded.
 */
export function secondaryFileForDriver(file: SecondaryFileData, driverIndex: number): SecondaryFileData {
  if (driverIndex < 0) return file.driverIndex === -1 ? file : { ...file, driverIndex: -1 }
  const catalog = file.analysisDrivers.find(driver => driver.driverIndex === driverIndex)
  if (!catalog) return file
  const times: Record<number, number> = {}
  for (const lap of catalog.laps) {
    if (Number.isFinite(lap.lapNum) && Number.isFinite(lap.lapTimeMs) && lap.lapTimeMs > 0) times[lap.lapNum] = lap.lapTimeMs
  }
  return {
    ...file,
    blocks: catalog.blocks,
    lapTimesByNum: times,
    fastestLapNum: catalog.fastestLapNum > 0 ? catalog.fastestLapNum : null,
    driverIndex,
  }
}

/** Identifies file B and the car followed in it, for caches of B's data. */
export function secondaryFileKey(file: SecondaryFileData): string {
  return `${file.filePath}\n${file.driverIndex}`
}

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec']
// Recordings are named <game>_<track>_<session>_<YYYY-MM-DD>T<HH-MM-SS>…
const STAMP = /(\d{4})-(\d{2})-(\d{2})T(\d{2})-(\d{2})/

/** "10 Oct" from a recording's file name, or null when it carries no timestamp. */
export function recordingDateLabel(filename: string | null | undefined): string | null {
  const m = filename ? STAMP.exec(filename) : null
  if (!m) return null
  const month = Number(m[2])
  if (month < 1 || month > 12) return null
  return `${Number(m[3])} ${MONTHS[month - 1]}`
}

/** "00:53" from a recording's file name, or null. */
export function recordingTimeLabel(filename: string | null | undefined): string | null {
  const m = filename ? STAMP.exec(filename) : null
  return m ? `${m[4]}:${m[5]}` : null
}

const SESSION_TOKENS: Array<[RegExp, string]> = [
  [/_sprint_shootout/, 'Sprint Shootout'],
  [/_sprint_/, 'Sprint'],
  [/_qualifying/, 'Qualifying'],
  [/_practice/, 'Practice'],
  [/_time_trial/, 'Time Trial'],
  [/_race/, 'Race'],
]

/** Session name read from a recording's file name ("Race"), or null. */
export function recordingSessionLabel(filename: string | null | undefined): string | null {
  if (!filename) return null
  const name = filename.toLowerCase()
  for (const [pattern, label] of SESSION_TOKENS) {
    if (pattern.test(name)) return label
  }
  return null
}

/**
 * The short labels the title bar shows for the two files. Each file is named by
 * its date; when both are from the same day the time is added so they differ.
 */
export function fileDateLabels(fileA: string | null | undefined, fileB: string | null | undefined): [string | null, string | null] {
  let a = recordingDateLabel(fileA)
  let b = recordingDateLabel(fileB)
  if (a && b && a === b) {
    const ta = recordingTimeLabel(fileA)
    const tb = recordingTimeLabel(fileB)
    if (ta) a = `${a} ${ta}`
    if (tb) b = `${b} ${tb}`
  }
  return [a, b]
}

/**
 * One of file B's laps for one of its cars (-1: the recorded player), holding
 * only the rows in `rowTypeMask`. It comes the way a seek's history does for
 * file A: the engine reads the lap's columns on a worker thread and sends them
 * as V6 column blocks, which are decoded straight into column tables (on the
 * history decode worker when there is one). No JSON is built or parsed.
 *
 * `v6TypeMask` (bit n = V6 type n) limits the read to those V6 fields, the
 * ones the page shows for file A (secondaryLapV6TypeMask); 0 reads every field
 * of the families in `rowTypeMask`.
 *
 * Only a recording without column history (anything older than V6) falls back
 * to the lap as JSON, as file A's playback does for those formats. Resolves
 * null while the engine is still indexing B, or when B has no such lap.
 */
export async function fetchSecondaryLap(
  lapNum: number, rowTypeMask: number, driverIndex: number, v6TypeMask = 0,
): Promise<AnalyzeLapData | null> {
  const lap = await window.analysisBridge.lapHistory(lapNum, rowTypeMask >>> 0, driverIndex, v6TypesOfMask(v6TypeMask))
  if (lap && isV6HistoryPayload(lap.history)) {
    return lapDataFromHistory(lap.history, lap.lapNum, lap.startSessionTime, lap.endSessionTime, lap.rowTypeMask, v6TypeMask)
  }
  return fetchSecondaryLapJson(lapNum, rowTypeMask, driverIndex)
}

/** Column blocks decoded into one lap's tables, as a seek's history is. */
async function lapDataFromHistory(
  bytes: Uint8Array, lapNum: number, start: number, end: number, rowTypeMask: number, v6TypeMask: number,
): Promise<AnalyzeLapData | null> {
  const decoded = await decodeV6History(bytes)
  if (!decoded) return null
  const view = <T extends { session_time: number }>(family: HistoryFamily): ColumnView<T> =>
    (decoded.tables[family]?.frozen() as ColumnView<T> | undefined) ?? emptyView<T>(family)
  return {
    lapNum,
    startSessionTime: start,
    endSessionTime: end,
    telemetry: view<TelemetryRow>('telemetry'),
    motion: view<MotionRow>('motion'),
    motionEx: view<MotionExRow>('motion_ex'),
    statusHistory: view<StatusRow>('status'),
    damageHistory: view<DamageRow>('damage'),
    lapProgress: view<LapProgressPoint>('lap'),
    // Car positions are map data, not column history; the chart pages do not draw B's.
    playerPositions: [],
    rowTypeMask: rowTypeMask >>> 0,
    ...v6TypeMask ? { v6TypeMask: v6TypeMask >>> 0 } : {},
  }
}

/** The JSON lap, for recordings the column path cannot serve (before V6). */
async function fetchSecondaryLapJson(lapNum: number, rowTypeMask: number, driverIndex: number): Promise<AnalyzeLapData | null> {
  const json = await window.analysisBridge.getLapDataJson(lapNum, rowTypeMask >>> 0, 'file2', driverIndex)
  if (!json) return null
  try {
    return parseAnalyzeLapData(JSON.parse(json))
  } catch {
    return null
  }
}

/**
 * The V6 fields file B's lap holds on this page, as a bit mask (bit n = V6
 * type n): the same fields file A is subscribed to, its charts' history fields
 * and its cards' stream fields, plus lap timing (24, the distance axis) and the
 * edge-encoded wing, tyre and brake-bias states (7, 13, 20), which a seek's
 * history always carries because they are only recorded when they change.
 */
export function secondaryLapV6TypeMask(streamTypes: readonly number[], historyTypes: readonly number[]): number {
  let mask = 0
  for (const type of [...streamTypes, ...historyTypes, 7, 13, 20, 24]) {
    if (Number.isInteger(type) && type > 0 && type <= 31) mask |= 1 << type
  }
  return mask >>> 0
}

function v6TypesOfMask(mask: number): number[] {
  const types: number[] = []
  for (let type = 1; type <= 31; type++) if (mask & (1 << type)) types.push(type)
  return types
}

/**
 * True when `lap` holds every row type in `rowTypeMask` and every V6 field in
 * `v6TypeMask` (a lap read without a field list holds them all).
 */
export function lapHasFields(lap: AnalyzeLapData, rowTypeMask: number, v6TypeMask: number): boolean {
  if (!lapHasRows(lap, rowTypeMask)) return false
  return lap.v6TypeMask === undefined || ((lap.v6TypeMask & v6TypeMask) >>> 0) === (v6TypeMask >>> 0)
}

/** True when `lap` already holds every row type in `rowTypeMask`. */
function lapHasRows(lap: AnalyzeLapData, rowTypeMask: number): boolean {
  return (((lap.rowTypeMask ?? 0xFFFFFFFF) & rowTypeMask) >>> 0) === (rowTypeMask >>> 0)
}
