import { useEffect, useSyncExternalStore } from 'react'
import type {
  AllStatusMsg, AnalyzeLapData, DriverLapHistory, DriverLapHistoryLap, FastestLapMsg, LapMeasure, LapRow, ParticipantsMsg, PositionsMsg,
  SecondaryStreamMessage, SessionHistoryFastestMsg, SessionMsg, StatusRow, TimingMsg, TyreSetsMsg,
} from '../types'
import { ColumnTable, decodeV6History, isV6HistoryPayload, tableFromView, type HistoryFamily } from './columnStore'
import { mergeCarPatches, mergeV6Patch } from '../stores/telemetryStore'
import { FastestLapHolder } from './fastestLapHolder'
import { forEachDecodedBinaryRow } from './decodeBinaryBatch'
import { DATA_ROW } from './historyDependencies'

/*
 * File B's rows at its moment on the lap-synced playhead, streamed the way
 * file A's are. The engine's merged clock places B (Engine::setMergedPlayheadCallback);
 * the addon reads B's recording forward to there every 16 ms and pushes the
 * rows (SecondaryStream in addon.cpp). On a jump it first sends a flush: the
 * followed car's history up to B's time as column blocks (the format of A's
 * seek flush) and the newest row of every family.
 *
 * Kept here as A's telemetry store keeps A's: the followed car's history
 * (telemetry, status, damage, lap, motion) from the start of its lap, or, for
 * the families B's longer windows show, as far back as the widest of them
 * needs (time windows, Stint and All Laps, Trends); every car's positions,
 * timing and status, merged from per-car patches; the followed car's newest
 * lap and status rows, as A's store holds them; B's session, participants,
 * tyre sets, fastest-lap holder and completed laps (Trends).
 */

/** File B's Trends inputs: its completed laps and their ERS and fuel use. */
export interface SecondaryTrends {
  laps: DriverLapHistoryLap[]
  stintStartLap: number
  measures: ReadonlyMap<number, LapMeasure>
}

export interface SecondaryState {
  session: SessionMsg | null
  participants: ParticipantsMsg | null
  timing: TimingMsg | null
  /** Every car's status (the Standings tower's tyres). */
  allStatus: AllStatusMsg | null
  /** The followed car's newest lap row (the Standings panel's timing), as A's store's `lap`. */
  lapRow: LapRow | null
  /** The followed car's newest status row, as A's store's `status`. */
  status: StatusRow | null
  /** The lap-times view's car's laps (claimSecondaryLapHistoryCar), as A's store's `driverLapHistory`. */
  driverLapHistory: DriverLapHistory | null
  /** The car holding the fastest lap at B's moment; null before B's stream says. */
  fastestLapCarIdx: number | null
  tyreSets: TyreSetsMsg | null
  trends: SecondaryTrends | null
  /** The followed car's current lap so far (rows up to B's playhead); null before the first flush lands. */
  lap: AnalyzeLapData | null
  /** Everything held, rows up to B's playhead: the longer families as far back as B's windows need. */
  history: AnalyzeLapData | null
}

const EMPTY_STATE: SecondaryState = {
  session: null, participants: null, timing: null, allStatus: null, lapRow: null, status: null, driverLapHistory: null, fastestLapCarIdx: null,
  tyreSets: null, trends: null, lap: null, history: null,
}

// The followed car's tables, by row type.
type LapFamily = Extract<HistoryFamily, 'telemetry' | 'motion' | 'motion_ex' | 'status' | 'damage' | 'lap'>
const LAP_FAMILIES: readonly LapFamily[] = ['telemetry', 'motion', 'motion_ex', 'status', 'damage', 'lap']
const FAMILY_BIT: Record<LapFamily, number> = {
  telemetry: DATA_ROW.telemetry, motion: DATA_ROW.motion, motion_ex: DATA_ROW.motionEx,
  status: DATA_ROW.status, damage: DATA_ROW.damage, lap: DATA_ROW.lap,
}
const LAP_ROW_MASK = LAP_FAMILIES.reduce((mask, family) => mask | FAMILY_BIT[family], 0) >>> 0
const isLapFamily = (type: string): type is LapFamily => (LAP_FAMILIES as readonly string[]).includes(type)

// ── What B's visible windows need ────────────────────────────────────────────

/** A longer history one of B's windows shows: its families, and seconds back (-1: the whole race). */
export interface SecondaryHistoryNeed {
  mask: number
  seconds: number
}

const needs = new Map<symbol, SecondaryHistoryNeed>()
let enabled = false
let driverIndex = -1
// The longer history in force: its families and how far back (0: none).
let longMask = 0
let longSeconds = 0

function unionNeeds(): SecondaryHistoryNeed {
  let mask = 0
  let seconds = 0
  for (const need of needs.values()) {
    if (need.mask === 0 || need.seconds === 0) continue
    mask |= need.mask
    seconds = seconds < 0 || need.seconds < 0 ? -1 : Math.max(seconds, need.seconds)
  }
  mask &= LAP_ROW_MASK
  return mask === 0 ? { mask: 0, seconds: 0 } : { mask, seconds }
}

/** Tells the addon what to stream; a wider history arrives with a flush. */
function configure(): void {
  // Until the stream runs, needs are only collected; turning it on sends them.
  if (!enabled) return
  const union = unionNeeds()
  if (union.mask === longMask && union.seconds === longSeconds) return
  longMask = union.mask
  longSeconds = union.seconds
  window.analysisBridge.setStreaming(enabled, driverIndex, longMask, longSeconds)
}

// ── The store ────────────────────────────────────────────────────────────────

// What readers see, published once a frame from `pending` (the rows applied
// since), with the tables' views rebuilt then if they changed.
let state: SecondaryState = EMPTY_STATE
let pending: SecondaryState = EMPTY_STATE
let tablesDirty = false
let positions: PositionsMsg | null = null
const fastestLapHolder = new FastestLapHolder()
let tables = freshTables()
let lapNum = 0
let lapStart = 0
// A flush's history decoding; later batches wait behind it, as A's rows wait
// behind its seek flush.
let flushToken = 0
let decoding = false
let queued: SecondaryStreamMessage[] = []

const listeners = new Set<() => void>()
let notifyFrame = 0

function freshTables(): Record<LapFamily, ColumnTable> {
  return Object.fromEntries(LAP_FAMILIES.map(family => [family, new ColumnTable(family)])) as Record<LapFamily, ColumnTable>
}

const isLong = (family: LapFamily) => (longMask & FAMILY_BIT[family]) !== 0

/** Publishes what changed, once per frame at most. */
function changed(tablesChanged: boolean): void {
  tablesDirty ||= tablesChanged
  if (notifyFrame) return
  notifyFrame = requestAnimationFrame(() => {
    notifyFrame = 0
    if (tablesDirty) pending = { ...pending, ...tableViews() }
    tablesDirty = false
    if (pending === state) return
    state = pending
    for (const listener of listeners) listener()
  })
}

function tableViews(): Pick<SecondaryState, 'lap' | 'history'> {
  const view = (family: LapFamily, from: number) => {
    const table = tables[family]
    return table.frozen(from > -Infinity ? table.lowerBound(from, true) : 0)
  }
  const build = (from: number, start: number): AnalyzeLapData => ({
    lapNum,
    startSessionTime: start,
    endSessionTime: Math.max(start, tables.telemetry.lastTime() ?? start),
    telemetry: view('telemetry', from) as AnalyzeLapData['telemetry'],
    motion: view('motion', from) as AnalyzeLapData['motion'],
    motionEx: view('motion_ex', from) as AnalyzeLapData['motionEx'],
    statusHistory: view('status', from) as AnalyzeLapData['statusHistory'],
    damageHistory: view('damage', from) as AnalyzeLapData['damageHistory'],
    lapProgress: view('lap', from) as AnalyzeLapData['lapProgress'],
    playerPositions: [],
    rowTypeMask: LAP_ROW_MASK,
  })
  const earliest = LAP_FAMILIES.reduce((min, family) => Math.min(min, tables[family].firstTime() ?? Infinity), Infinity)
  return {
    lap: build(lapStart, lapStart),
    history: build(-Infinity, Number.isFinite(earliest) ? Math.min(earliest, lapStart) : lapStart),
  }
}

/** A row onto its table: V6 patches carry the previous row's other fields (as A's store does). */
function appendRow(family: LapFamily, row: Record<string, unknown> & { session_time: number }): void {
  const table = tables[family]
  const last = table.lastTime()
  // A late row from before what the table holds (a reordered packet) is dropped.
  if (last !== undefined && row.session_time < last) return
  if (Number.isInteger(Number(row._v6_type))) table.appendPatch(row)
  else table.append(row)
}

/** Drops what no window shows any more: before the lap, or before the longer windows' reach. */
function trim(now: number): void {
  for (const family of LAP_FAMILIES) {
    if (!isLong(family)) tables[family].trimBefore(lapStart)
    else if (longSeconds > 0) tables[family].trimBefore(Math.min(lapStart, now - longSeconds))
  }
}

function applyParticipants(incoming: ParticipantsMsg): void {
  const previous = pending.participants
  if (!previous) {
    pending = { ...pending, participants: incoming }
    return
  }
  // As A's store: keep known identities by car index, apply what changed.
  const drivers = new Map(previous.drivers.map(driver => [driver.idx, driver]))
  for (const driver of incoming.drivers) drivers.set(driver.idx, driver)
  pending = { ...pending, participants: { ...previous, ...incoming, drivers: [...drivers.values()].sort((a, b) => a.idx - b.idx) } }
}

interface LapTrendsRow {
  history?: { laps?: DriverLapHistoryLap[]; stint_start_lap?: number }
  measures?: { laps?: LapMeasure[] }
}

function applyTrends(row: LapTrendsRow): void {
  pending = {
    ...pending,
    trends: {
      laps: row.history?.laps ?? [],
      stintStartLap: row.history?.stint_start_lap || 1,
      measures: new Map((row.measures?.laps ?? []).map(lap => [lap.lap_num, lap])),
    },
  }
}

/** One batch's JSON rows. `flushRows`: a flush's newest-of-each rows, whose families its history already holds. */
function applyRows(json: string, flushRows: boolean): boolean {
  let tablesTouched = false
  let start = 0
  while (start < json.length) {
    let end = json.indexOf('\n', start)
    if (end === -1) end = json.length
    if (end > start) {
      try {
        const row = JSON.parse(json.slice(start, end)) as Record<string, unknown> & { type?: string; session_time: number }
        const type = row.type ?? ''
        if (isLapFamily(type)) {
          // The newest lap and status rows, merged as A's store merges its own.
          if (type === 'status') pending = { ...pending, status: mergeV6Patch(tables.status.last() as StatusRow | null ?? undefined, row as unknown as StatusRow) }
          else if (type === 'lap') pending = { ...pending, lapRow: (pending.lapRow ? { ...pending.lapRow, ...row } : row) as unknown as LapRow }
          if (!flushRows || tables[type].length === 0) {
            appendRow(type, row)
            tablesTouched = true
          }
        } else if (type === 'positions') positions = mergeCarPatches(positions, row as unknown as PositionsMsg)
        else if (type === 'timing') pending = { ...pending, timing: mergeCarPatches(pending.timing, row as unknown as TimingMsg) }
        else if (type === 'all_status') pending = { ...pending, allStatus: mergeCarPatches(pending.allStatus, row as unknown as AllStatusMsg) }
        else if (type === 'fastest_lap') pending = { ...pending, fastestLapCarIdx: fastestLapHolder.fastestLap(row as unknown as FastestLapMsg) }
        else if (type === 'driver_lap_history') pending = { ...pending, driverLapHistory: row as unknown as DriverLapHistory }
        else if (type === 'session_history_fastest') {
          const holder = fastestLapHolder.sessionHistory(row as unknown as SessionHistoryFastestMsg)
          if (holder !== undefined && holder !== pending.fastestLapCarIdx) pending = { ...pending, fastestLapCarIdx: holder }
        }
        else if (type === 'session') pending = { ...pending, session: row as unknown as SessionMsg }
        else if (type === 'participants') applyParticipants(row as unknown as ParticipantsMsg)
        else if (type === 'tyre_sets') pending = { ...pending, tyreSets: row as unknown as TyreSetsMsg }
        else if (type === 'lap_trends') applyTrends(row as LapTrendsRow)
      } catch { /* a malformed row is skipped */ }
    }
    start = end + 1
  }
  return tablesTouched
}

/**
 * Packed hot rows (telemetry, motion, positions): what playback of a
 * recording before V6 sends this way, as A's binary batches.
 */
function applyBinary(bytes: Uint8Array | undefined): boolean {
  if (!bytes || bytes.byteLength === 0) return false
  let tablesTouched = false
  forEachDecodedBinaryRow(bytes, row => {
    if (row.type === 'positions') positions = mergeCarPatches(positions, row)
    else if (isLapFamily(row.type)) {
      appendRow(row.type, row as unknown as Record<string, unknown> & { session_time: number })
      tablesTouched = true
    }
  })
  return tablesTouched
}

/** A stream batch after any flush has landed. */
function applyBatch(message: SecondaryStreamMessage): void {
  let tablesChanged = applyBinary(message.binary)
  tablesChanged = applyRows(message.json, false) || tablesChanged
  // B crossed its line: the current lap starts over, as A's does.
  if (message.lapNum > 0 && message.lapNum !== lapNum) {
    lapNum = message.lapNum
    lapStart = message.lapStart
    tablesChanged = true
  }
  if (tablesChanged) trim(message.time)
  changed(tablesChanged)
}

async function decodeTables(bytes: Uint8Array | undefined): Promise<Partial<Record<HistoryFamily, ColumnTable>> | null> {
  if (!bytes || !isV6HistoryPayload(bytes)) return null
  try {
    return (await decodeV6History(bytes))?.tables ?? null
  } catch {
    return null
  }
}

function applyFlush(message: SecondaryStreamMessage): void {
  const token = ++flushToken
  lapNum = message.lapNum
  lapStart = message.lapStart
  queued = []
  decoding = true
  void Promise.all([decodeTables(message.history), decodeTables(message.longHistory)]).then(([lap, long]) => {
    if (token !== flushToken) return
    tables = freshTables()
    for (const family of LAP_FAMILIES) {
      const table = (isLong(family) ? long : lap)?.[family]
      if (table && table.length > 0) tables[family] = tableFromView(table.frozen())
    }
    // Before V6 the history comes as A's seek flush does for those formats.
    applyBinary(message.historyBinary)
    if (message.historyJson) applyRows(message.historyJson, false)
    applyRows(message.json, true)
    decoding = false
    const waiting = queued
    queued = []
    for (const next of waiting) applyBatch(next)
    changed(true)
  })
}

function onMessage(message: SecondaryStreamMessage): void {
  if (message.flush) applyFlush(message)
  else if (decoding) queued.push(message)
  else applyBatch(message)
}

function reset(): void {
  ++flushToken
  decoding = false
  queued = []
  pending = EMPTY_STATE
  tablesDirty = false
  positions = null
  fastestLapHolder.clear()
  tables = freshTables()
  lapNum = 0
  lapStart = 0
  changed(false)
}

/**
 * Runs file B's stream while a second file is open (called once, by the
 * shell), following car `driver` (-1: B's recorded player).
 */
export function useSecondaryStreamControl(on: boolean, driver: number): void {
  useEffect(() => {
    if (!on) return
    const off = window.analysisBridge.onBatch(onMessage)
    return () => {
      off()
      enabled = false
      longMask = 0
      longSeconds = 0
      window.analysisBridge.setStreaming(false, -1)
      reset()
    }
  }, [on])
  useEffect(() => {
    if (!on) return
    // Another car starts over from a flush for it.
    reset()
    enabled = true
    driverIndex = driver
    longMask = -1   // forces the next configure() through
    configure()
  }, [on, driver])
}

/**
 * Asks B's stream to hold `need` (families and seconds back; -1 the whole
 * race) while the caller is mounted: a window of B's longer than its lap.
 * null asks for nothing.
 */
export function useSecondaryHistoryNeed(need: SecondaryHistoryNeed | null): void {
  const mask = need?.mask ?? 0
  const seconds = need?.seconds ?? 0
  useEffect(() => {
    if (mask === 0 || seconds === 0) return
    const key = Symbol('need')
    needs.set(key, { mask, seconds })
    configure()
    return () => {
      needs.delete(key)
      configure()
    }
  }, [mask, seconds])
}

function subscribe(listener: () => void): () => void {
  listeners.add(listener)
  return () => { listeners.delete(listener) }
}

/** Forgets B's pushed lap history: a new claim may name another car (lib/lapHistoryCar.ts). */
export function clearSecondaryLapHistory(): void {
  if (!pending.driverLapHistory) return
  pending = { ...pending, driverLapHistory: null }
  changed(false)
}

/** File B's state at its moment on the playhead; re-renders at most once a frame. */
export function useSecondaryState(): SecondaryState {
  return useSyncExternalStore(subscribe, () => state)
}

/** B's cars on its map, read every frame without re-rendering (as A's map reads its own). */
export function secondaryCars(): PositionsMsg['cars'] | null {
  return positions?.cars ?? null
}
