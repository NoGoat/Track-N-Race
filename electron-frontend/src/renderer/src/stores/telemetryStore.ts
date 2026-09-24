import { create } from 'zustand'
import type {
  TelemetryRow, MotionRow, MotionExRow, LapRow, StatusRow, DamageRow, TimingMsg,
  ParticipantsMsg, AllStatusMsg, RaceEventMsg, SessionMsg, TyreSetsMsg, GatewayMsg,
  LapProgressPoint, SessionHistoryFastestMsg, ProtocolStatusMsg, ProtocolWarningMsg,
  AnalyzeLapData, AnalysisDriverLapCatalog, PlaybackLapDataMsg,
  StrategySnapshotMsg,
} from '../types'
import { decodeBinaryBatchRange, forEachDecodedBinaryRow } from '../lib/decodeBinaryBatch'
import { scheduleCooperativeTask, yieldToMainThread } from '../lib/cooperativeTask'
import { playbackDebug } from '../lib/playbackDebug'
import { HISTORY_ROW } from '../lib/historyDependencies'
import { mergeAnalyzeLapData } from '../lib/analyzeLapData'
import { getTelemetryChartRetentionDiagnostics } from '../diagnostics/telemetryRetention'
import { getDebugSettings, subscribeDebugSettings } from '../lib/debugSettings'
import {
  ColumnTable, V6_PATCH_FIELDS, concatAfter, decodeV6History, emptyView, installHistory,
  isV6HistoryPayload, viewOfRows, type ColumnView, type HistoryFamily,
} from '../lib/columnStore'

// ─────────────────────────────────────────────────────────────────────────────
// Telemetry store.
//
// This is a straight relocation of the old useTelemetry hook. The reason it is a
// module-level store rather than a hook living in <App> is performance: the hot
// 60–120 Hz stream used to bump state at the App root, re-rendering the entire
// component tree ~120×/s. Here the IPC subscription writes to the store OUTSIDE
// React, and components subscribe to only the slices they read — so a telemetry
// frame re-renders just the handful of leaves that display it, never all of App.
//
// The buffer/window/lap/playback logic below is IDENTICAL to the old hook; only
// the plumbing (refs → module vars, useState → store set()) changed.
// ─────────────────────────────────────────────────────────────────────────────

const MAX_ROWS = 750000
const RETENTION_S = 600 // reconciliation window used only after a clock reversal
const MAX_RACE_EVENTS = 1000
const MAX_ANALYZE_LAP_CACHE = 6

// Append a row to a column table — O(1) amortized. In ordinary live mode the
// sources are trimmed at lap boundaries to Current + Previous + Previous-previous.
// All Laps obtains older requested families from the native compressed store.
// A session_time reversal rebuilds the table.
function appendRow<T extends { session_time: number }>(table: ColumnTable<T>, msg: T, maxRows: number): void {
  reconcileReversal(table, msg.session_time)
  table.append(msg as T & Record<string, unknown>, maxRows)
}

function reconcileReversal(table: ColumnTable<any>, sessionTime: number): void {
  const last = table.lastTime()
  if (last === undefined || sessionTime >= last) return
  // A rapid seek can leave a few superseded future rows in flight. AL owns the
  // full prefix, so reconcile by truncating only that future tail; applying
  // the ordinary 600-second recovery window here destroys laps.
  if (allLapsMode) table.retainRange(-Infinity, sessionTime)
  else table.retainRange(sessionTime - RETENTION_S, sessionTime)
}

function mergePlaybackPatch<T extends Record<string, any>>(previous: T | undefined, patch: T): T {
  const v6Type = Number(patch._v6_type)
  if (!isPlaybackFlag || !Number.isInteger(v6Type)) return patch
  const merged: Record<string, any> = { ...previous, ...patch }
  if (patch.available === false) {
    const dropped = V6_PATCH_FIELDS[v6Type] ?? []
    playbackDebug('patch-availability-wipe', {
      v6Type,
      rowType: patch.type ?? null,
      sessionTime: patch.session_time ?? null,
      droppedFields: dropped,
      hadSlmBefore: merged.slm !== undefined,
    })
    for (const field of dropped) {
      delete merged[field]
    }
  }
  delete merged.available
  return merged as T
}

// Playback V6 patches carry forward the previous row's other fields; every
// other row is appended as it stands.
function appendPlaybackPatch<T extends { session_time: number }>(
  table: ColumnTable<T>, patch: T, maxRows: number,
): void {
  const row = patch as T & Record<string, unknown>
  if (isPlaybackFlag && Number.isInteger(Number(row._v6_type))) {
    reconcileReversal(table, patch.session_time)
    table.appendPatch(row, maxRows)
  } else {
    appendRow(table, patch, maxRows)
  }
}

function mergeCarPatches<T extends { cars: Array<Record<string, any>> }>(previous: T | null, patch: T): T {
  if (!previous || !Number.isInteger(Number((patch as any)._v6_type))) return patch
  const v6Type = Number((patch as any)._v6_type)
  const cars = new Map(previous.cars.map(car => [Number(car.idx), car]))
  for (const carPatch of patch.cars) {
    const prior = cars.get(Number(carPatch.idx))
    const merged = { ...prior, ...carPatch }
    if (carPatch.available === false)
      for (const field of V6_PATCH_FIELDS[v6Type] ?? []) delete merged[field]
    delete merged.available
    cars.set(Number(carPatch.idx), merged)
  }
  return {
    ...previous,
    ...patch,
    cars: [...cars.values()].sort((left, right) => Number(left.idx) - Number(right.idx)),
  }
}

declare global {
  interface Window {
    telemetryBridge: {
      on: (callback: (row: unknown) => void) => (() => void)
      onBatch: (callback: (batch: string) => void) => (() => void)
      onBinary: (callback: (batch: Uint8Array) => void) => (() => void)
      onResume: (callback: (payload: { binary: Uint8Array; coldJson: string }) => void) => (() => void)
      reportRetention: (snapshot: unknown) => void
    }
  }
}

// The reactive, published view. Components select from this. Everything else is
// working state kept in the module vars below (the store never publishes it).
export interface TelemetryStoreState {
  telemetry: ColumnView<TelemetryRow>
  motion: ColumnView<MotionRow>
  motionEx: ColumnView<MotionExRow>
  status: StatusRow | null
  statusHistory: ColumnView<StatusRow>
  damage: DamageRow | null
  damageHistory: ColumnView<DamageRow>
  lap: LapRow | null
  timing: TimingMsg | null
  participants: ParticipantsMsg | null
  allStatus: AllStatusMsg | null
  fastestLapCarIdx: number | null
  raceEvents: RaceEventMsg[]
  session: SessionMsg | null
  tyreSets: TyreSetsMsg | null
  strategy: StrategySnapshotMsg | null
  latest: TelemetryRow | null
  fastestLapNum: number | null
  analyzeLapTelemetry: ColumnView<TelemetryRow>
  analyzeLapMotion: ColumnView<MotionRow>
  analyzeLapMotionEx: ColumnView<MotionExRow>
  analyzeLapStatusHistory: ColumnView<StatusRow>
  analyzeLapDamageHistory: ColumnView<DamageRow>
  analyzeLapProgress: ColumnView<LapProgressPoint>
  analyzeLapStartTime: number
  analyzeLapRevision: number
  analyzeDeltaAvailable: boolean
  analyzeTrackLengthM: number
  playbackTnrdVersion: string | null
  playbackAnalysisDrivers: AnalysisDriverLapCatalog[]
  playbackDriverIndex: number | null
  playbackTrackId: number | null
  playbackTrackName: string | null
  playbackLapDataCache: Record<number, AnalyzeLapData>
  livePreviousLapData: AnalyzeLapData | null
  liveFastestLapData: AnalyzeLapData | null
  lapTimesByNum: Record<number, number>
  speedRpmBlocks: any[] | null
  isConnected: boolean
  error: string | null
  protocolStatus: ProtocolStatusMsg | null
  protocolWarning: ProtocolWarningMsg | null
  fuelUpperLimit: number | null
  seconds: number
  lapBoundaries: Array<{ lapNum: number; sessionTime: number }>
  allLapsLapBoundaries: Array<{ lapNum: number; sessionTime: number }>
  currentStintStartTime: number
  // True from a playback seek start until its data is installed (or fails).
  seekPending: boolean
  // True from a playback seek start until Strategy's rebuilt snapshot arrives.
  // Strategy reconstructs asynchronously, after the seek itself installs.
  strategyRebuilding: boolean
}

// Shared empty publications, so "nothing to show" keeps a stable identity.
const EMPTY = {
  telemetry: emptyView<TelemetryRow>('telemetry'),
  motion: emptyView<MotionRow>('motion'),
  motionEx: emptyView<MotionExRow>('motion_ex'),
  status: emptyView<StatusRow>('status'),
  damage: emptyView<DamageRow>('damage'),
  lap: emptyView<LapProgressPoint>('lap'),
}
const EMPTY_ANALYZE_SLICES = {
  analyzeLapTelemetry: EMPTY.telemetry, analyzeLapMotion: EMPTY.motion, analyzeLapMotionEx: EMPTY.motionEx,
  analyzeLapStatusHistory: EMPTY.status, analyzeLapDamageHistory: EMPTY.damage, analyzeLapProgress: EMPTY.lap,
}

export const useTelemetryStore = create<TelemetryStoreState>()(() => ({
  telemetry: EMPTY.telemetry, motion: EMPTY.motion, motionEx: EMPTY.motionEx,
  status: null, statusHistory: EMPTY.status, damage: null, damageHistory: EMPTY.damage,
  lap: null, timing: null, participants: null, allStatus: null, strategy: null,
  fastestLapCarIdx: null, raceEvents: [], session: null, tyreSets: null,
  latest: null, fastestLapNum: null,
  ...EMPTY_ANALYZE_SLICES, analyzeLapStartTime: 0,
  analyzeLapRevision: 0,
  analyzeDeltaAvailable: false, analyzeTrackLengthM: 0, playbackTnrdVersion: null,
  playbackAnalysisDrivers: [], playbackDriverIndex: null,
  playbackTrackId: null, playbackTrackName: null,
  playbackLapDataCache: {},
  livePreviousLapData: null,
  liveFastestLapData: null,
  lapTimesByNum: {}, speedRpmBlocks: null, isConnected: true, error: null,
  protocolStatus: null, protocolWarning: null, fuelUpperLimit: null, seconds: 30,
  lapBoundaries: [],
  allLapsLapBoundaries: [],
  currentStintStartTime: -Infinity,
  seekPending: false,
  strategyRebuilding: false,
}))

const set = useTelemetryStore.setState

// ── Working state (never published; the source of truth for computation) ──────
// Typed column tables (lib/columnStore.ts). Publications are views onto them:
// a frozen range for finite windows and lap slices, the table's live view for
// the full-session All Laps publication.
const telTable   = new ColumnTable<TelemetryRow>('telemetry')
const motTable   = new ColumnTable<MotionRow>('motion')
const motExTable = new ColumnTable<MotionExRow>('motion_ex')
const dmgTable   = new ColumnTable<DamageRow>('damage')
const stsTable   = new ColumnTable<StatusRow>('status')
const lapProgressTable = new ColumnTable<LapRow>('lap')
const TABLE_OF_FAMILY: Record<HistoryFamily, ColumnTable<any>> = {
  telemetry: telTable, motion: motTable, motion_ex: motExTable,
  status: stsTable, damage: dmgTable, lap: lapProgressTable,
}

const raceEventListeners = new Set<(e: RaceEventMsg) => void>()
const allLapsDataListeners = new Map<() => void, number>()
let allLapsNotificationRunning = false
let allLapsNotificationPendingMask = 0

function scheduleAllLapsDataNotification(mask: number): void {
  allLapsNotificationPendingMask |= mask
  if (allLapsNotificationRunning) return
  allLapsNotificationRunning = true
  const flush = (): void => {
    const pendingMask = allLapsNotificationPendingMask
    allLapsNotificationPendingMask = 0
    for (const [listener, sourceMask] of allLapsDataListeners) {
      if (sourceMask & pendingMask) listener()
    }
    if (allLapsNotificationPendingMask !== 0) {
      scheduleCooperativeTask(flush)
    } else {
      allLapsNotificationRunning = false
    }
  }
  scheduleCooperativeTask(flush)
}

let lapState: LapRow | null = null
let lapNum: number | null = null
let lapStartTime = 0
let lapTrackingActive = false
let fastestLapTime = Infinity
let fastestRecoveryTimer: ReturnType<typeof setTimeout> | null = null
let fastestRecoveryGeneration = 0
let fastestRecoveryPending = false
let fastestRecoveryDeadline: number | null = null

function cancelFastestRecovery(): void {
  if (fastestRecoveryTimer !== null) clearTimeout(fastestRecoveryTimer)
  fastestRecoveryTimer = null
  fastestRecoveryPending = false
  fastestRecoveryDeadline = null
  fastestRecoveryGeneration++
}

function scheduleFastestRecovery(): void {
  const deadline = fastestRecoveryDeadline ?? Date.now() + 30_000
  cancelFastestRecovery()
  fastestRecoveryPending = true
  fastestRecoveryDeadline = deadline
  const requestId = fastestRecoveryGeneration
  fastestRecoveryTimer = setTimeout(() => {
    fastestRecoveryTimer = null
    if (!isPlaybackFlag && !useTelemetryStore.getState().liveFastestLapData) {
      window.playerBridge.getLiveFastestLap(requestId)
    }
  }, Math.max(0, deadline - Date.now()))
}
let fastestLapSet = false
const sessionHistoryBest = new Map<number, number>()
// Live Tyre Sets packets cycle through every car (one car per packet), so
// only the player's latest set is published to the Tyres page.
const liveTyreSetsByCar = new Map<number, TyreSetsMsg>()
let isPlaybackFlag = false
let fuelMaxReceived = -Infinity

let raceEventsArr: RaceEventMsg[] = []
let speedRpmBlocksVal: any[] | null = null
let playbackFastestLapNum = 0
let playbackEvents: RaceEventMsg[] = []
let playbackLapTimes: Record<number, number> = {}
let liveLapTimes: Record<number, number> = {}
let playbackLapCacheOrder: number[] = []
let analyzeLapRevisionVal = 0
let pendingAnalyzeLapReset = false
let liveLapBoundaries: Array<{ lapNum: number; sessionTime: number }> = []
let allLapsLapBoundaries: Array<{ lapNum: number; sessionTime: number }> = []
let currentStintStartTime = -Infinity

let secondsVal = 30
let allLapsMode = false
let analyzeLapEnabled = true
let finiteWindowBackfillEnabled = true
let waitingForAllLapsHistory = false
let historyRowMask = 0xFFFFFFFF
let fullSessionHistoryRowMask = 0xFFFFFFFF
let secondaryFiniteHistoryRowMask = 0
let secondaryLapHistoryRowMask = 0
let secondaryHistoryWindowSeconds = 0
let requestedHistoryRowMask = 0
const historyV6Types = new Set<number>()
let pendingV6HistoryBackfillMask = 0
let seekTimelineGeneration = 0
let seekRendererPending = false

// All Laps history already decoded this session, per playback driver. A driver
// switch always re-seeks with full history; installing the cached copy while
// that seek runs makes switching back to a driver immediate, and the seek's
// authoritative flush then replaces it. Each entry holds a full race of
// columns for the requested families, so only a few drivers are kept.
const MAX_CACHED_DRIVERS = 3
type FamilyViews = Record<HistoryFamily, ColumnView<any>>
const allLapsDriverCache = new Map<number, FamilyViews>()
// The complete AL history captured at a seek start, before it is cleared.
let allLapsSnapshot: { driver: number | null; views: FamilyViews } | null = null
let currentPlaybackDriver: number | null = null
// True while the tables hold a cached driver's history rather than rows from
// the pending seek; its authoritative flush then starts from empty tables.
let tablesFromDriverCache = false

function snapshotTables(): FamilyViews {
  const views = {} as FamilyViews
  for (const family of Object.keys(TABLE_OF_FAMILY) as HistoryFamily[]) views[family] = TABLE_OF_FAMILY[family].frozen()
  return views
}

function rememberDriverHistory(driver: number, views: FamilyViews): void {
  allLapsDriverCache.delete(driver)
  allLapsDriverCache.set(driver, views)
  while (allLapsDriverCache.size > MAX_CACHED_DRIVERS) {
    const oldest = allLapsDriverCache.keys().next().value
    if (oldest === undefined) break
    allLapsDriverCache.delete(oldest)
  }
}
// The batch gate reads the module flag; the store copy only drives the seek
// loading overlay, so it is published once per transition, never per batch.
function setSeekPending(pending: boolean): void {
  if (seekRendererPending === pending) return
  seekRendererPending = pending
  set({ seekPending: pending })
}
// Rows Electron main deliberately forwards across a pending seek. They describe
// the loaded recording or the selected driver rather than the playhead, so the
// pending-seek gate below must keep them: a V6 driver change emits its lap
// catalog immediately before AppShell re-seeks to the same progress, and
// dropping that batch leaves the previous driver's lap list, coverage and
// history-request state in place until some later seek happens to arrive in a
// batch this gate admits.
const SEEK_PENDING_ROW_TYPES = new Set([
  'playback_close', 'playback_loaded', 'playback_lap_data', 'playback_lap_blocks',
])
const SEEK_PENDING_ROW_MARKERS = [...SEEK_PENDING_ROW_TYPES].map(type => `"type":"${type}"`)
let authoritativeLapStatusStart = -Infinity
let authoritativeLapStatusPrefix: ColumnView<StatusRow> = EMPTY.status
let activeSeekDecodeRetention: {
  binaryBytes: number
  coldJsonChars: number
  decodedTelemetryRows: number
  decodedMotionRows: number
  decodedMotionExRows: number
  decodedStatusRows: number
  decodedDamageRows: number
  decodedLapRows: number
} | null = null
const historyCoverageStart = new Map<number, number>()
const HISTORY_ROW_BITS = [
  HISTORY_ROW.telemetry,
  HISTORY_ROW.status,
  HISTORY_ROW.damage,
  HISTORY_ROW.lap,
  HISTORY_ROW.raceEvent,
  HISTORY_ROW.motion,
  HISTORY_ROW.motionEx,
]

interface RowRetentionEstimate {
  rows: number
  sampled_rows: number
  estimated_serialized_bytes: number
  estimated_array_reference_bytes: number
}

function estimateRows(rows: readonly unknown[]): RowRetentionEstimate {
  if (rows.length === 0) {
    return { rows: 0, sampled_rows: 0, estimated_serialized_bytes: 0, estimated_array_reference_bytes: 0 }
  }
  const sampleIndices = [...new Set([0, Math.floor(rows.length / 2), rows.length - 1])]
  let sampleBytes = 0
  let sampledRows = 0
  for (const index of sampleIndices) {
    try {
      sampleBytes += JSON.stringify(rows[index]).length
      sampledRows++
    } catch {
      // A malformed diagnostic sample must never affect telemetry ingest.
    }
  }
  return {
    rows: rows.length,
    sampled_rows: sampledRows,
    estimated_serialized_bytes: sampledRows > 0
      ? Math.round(sampleBytes / sampledRows * rows.length)
      : 0,
    // V8 may use pointer compression, so this is deliberately an estimate.
    estimated_array_reference_bytes: rows.length * 8,
  }
}

function sumRowEstimates(estimates: readonly RowRetentionEstimate[]): RowRetentionEstimate {
  return estimates.reduce<RowRetentionEstimate>((total, estimate) => ({
    rows: total.rows + estimate.rows,
    sampled_rows: total.sampled_rows + estimate.sampled_rows,
    estimated_serialized_bytes: total.estimated_serialized_bytes + estimate.estimated_serialized_bytes,
    estimated_array_reference_bytes: total.estimated_array_reference_bytes + estimate.estimated_array_reference_bytes,
  }), { rows: 0, sampled_rows: 0, estimated_serialized_bytes: 0, estimated_array_reference_bytes: 0 })
}

// Columns are 8 bytes per field per row; one materialised row gives the width.
function estimateView(view: ColumnView<any>): RowRetentionEstimate {
  if (view.length === 0) {
    return { rows: 0, sampled_rows: 0, estimated_serialized_bytes: 0, estimated_array_reference_bytes: 0 }
  }
  const fields = Object.keys(view.row(view.length - 1)).length
  return {
    rows: view.length,
    sampled_rows: 1,
    estimated_serialized_bytes: view.length * fields * 8,
    estimated_array_reference_bytes: 0,
  }
}

function estimateLapData(data: AnalyzeLapData): RowRetentionEstimate {
  return sumRowEstimates([
    estimateView(data.telemetry),
    estimateView(data.motion),
    estimateView(data.motionEx),
    estimateView(data.statusHistory),
    estimateView(data.damageHistory),
    estimateView(data.lapProgress),
    estimateRows(data.playerPositions),
  ])
}

function lapDataRowCount(data: AnalyzeLapData): number {
  return data.telemetry.length + data.motion.length + data.motionEx.length +
    data.statusHistory.length + data.damageHistory.length +
    data.lapProgress.length + data.playerPositions.length
}

function getRendererTelemetryRetentionDiagnostics(): Record<string, unknown> {
  const state = useTelemetryStore.getState()
  const workingCollections = {
    telemetry: estimateView(telTable.frozen()),
    motion: estimateView(motTable.frozen()),
    motion_ex: estimateView(motExTable.frozen()),
    status: estimateView(stsTable.frozen()),
    damage: estimateView(dmgTable.frozen()),
    lap_progress: estimateView(lapProgressTable.frozen()),
  }
  const working = sumRowEstimates(Object.values(workingCollections))

  // Published views share the working tables' columns; they hold no rows.
  const publishedViews = [
    state.telemetry, state.motion, state.motionEx, state.statusHistory, state.damageHistory,
    state.analyzeLapTelemetry, state.analyzeLapMotion, state.analyzeLapMotionEx,
    state.analyzeLapStatusHistory, state.analyzeLapDamageHistory, state.analyzeLapProgress,
  ]
  const publishedViewReferences = publishedViews.reduce((total, view) => total + view.length, 0)

  const playbackLapCollections = Object.values(state.playbackLapDataCache).map(estimateLapData)
  const playbackLapCache = sumRowEstimates(playbackLapCollections)
  const liveLapSnapshots = [...new Set(
    [state.livePreviousLapData, state.liveFastestLapData].filter(
      (value): value is AnalyzeLapData => value !== null,
    ),
  )]
  const liveLapSnapshotReferences = liveLapSnapshots.reduce(
    (total, data) => total + lapDataRowCount(data),
    0,
  )

  const playbackBlocks = speedRpmBlocksVal ?? []
  const blockRowEstimates = playbackBlocks.flatMap(block => [
    estimateRows(Array.isArray(block?.telemetry) ? block.telemetry : []),
    estimateRows(Array.isArray(block?.statusHistory) ? block.statusHistory : []),
  ])
  const playbackBlockRows = sumRowEstimates(blockRowEstimates)
  const playbackBlockMetadata = estimateRows(playbackBlocks.map(block => ({
    lapNum: block?.lapNum,
    startSessionTime: block?.startSessionTime,
    endSessionTime: block?.endSessionTime,
    sector1EndDistanceM: block?.sector1EndDistanceM,
    sector2EndDistanceM: block?.sector2EndDistanceM,
  })))
  const playbackBlockRetention = sumRowEstimates([playbackBlockRows, playbackBlockMetadata])
  const raceEvents = estimateRows(isPlaybackFlag ? playbackEvents : raceEventsArr)
  const lapBoundaries = estimateRows(liveLapBoundaries)
  const currentStateValues = [
    state.timing, state.participants, state.allStatus, state.session,
    state.tyreSets, state.strategy,
  ].filter(value => value !== null)
  const currentState = estimateRows(currentStateValues)
  const charts = getTelemetryChartRetentionDiagnostics()

  const seekDecode = activeSeekDecodeRetention
  const seekDecodedRows = seekDecode
    ? seekDecode.decodedTelemetryRows + seekDecode.decodedMotionRows +
      seekDecode.decodedMotionExRows + seekDecode.decodedStatusRows +
      seekDecode.decodedDamageRows + seekDecode.decodedLapRows
    : 0
  const seekRawBytes = seekDecode
    ? seekDecode.binaryBytes + seekDecode.coldJsonChars * 2
    : 0
  const averageWorkingRowBytes = working.rows > 0
    ? working.estimated_serialized_bytes / working.rows
    : 128
  const seekDecodedBytes = Math.round(
    seekDecodedRows * (averageWorkingRowBytes + 8),
  )

  const estimatedRetainedBytes =
    working.estimated_serialized_bytes + working.estimated_array_reference_bytes +
    playbackLapCache.estimated_serialized_bytes + playbackLapCache.estimated_array_reference_bytes +
    playbackBlockRetention.estimated_serialized_bytes + playbackBlockRetention.estimated_array_reference_bytes +
    raceEvents.estimated_serialized_bytes + raceEvents.estimated_array_reference_bytes +
    lapBoundaries.estimated_serialized_bytes + lapBoundaries.estimated_array_reference_bytes +
    currentState.estimated_serialized_bytes + currentState.estimated_array_reference_bytes +
    liveLapSnapshotReferences * 8 +
    charts.cpuBytes + charts.gpuTextureBytes + seekRawBytes + seekDecodedBytes

  return {
    sampled_at: new Date().toISOString(),
    mode: isPlaybackFlag ? 'playback' : 'realtime',
    estimated_retained_bytes: estimatedRetainedBytes,
    estimate_basis: 'history columns at 8 bytes per field per row; other state is sampled JSON size; GPU allocations are exact',
    working_source_buffers: {
      ...working,
      collections: workingCollections,
    },
    published_window_views: {
      arrays: publishedViews.length,
      row_references: publishedViewReferences,
      estimated_reference_bytes: 0,
    },
    playback_lap_cache: {
      laps: playbackLapCollections.length,
      ...playbackLapCache,
    },
    live_lap_snapshots: {
      snapshots: liveLapSnapshots.length,
      row_references: liveLapSnapshotReferences,
      estimated_reference_bytes: liveLapSnapshotReferences * 8,
    },
    playback_lap_blocks: {
      blocks: playbackBlocks.length,
      estimated_serialized_bytes: playbackBlockRetention.estimated_serialized_bytes,
      estimated_array_reference_bytes: playbackBlockRetention.estimated_array_reference_bytes,
      telemetry_rows: playbackBlockRows.rows,
      metadata: playbackBlockMetadata,
    },
    race_events: raceEvents,
    lap_boundaries: lapBoundaries,
    current_state: currentState,
    chart_buffers: {
      buffer_count: charts.bufferCount,
      rows: charts.rows,
      channels: charts.channels,
      allocated_pages: charts.allocatedPages,
      cpu_bytes: charts.cpuBytes,
      gpu_pages: charts.gpuPageCount,
      gpu_texture_bytes: charts.gpuTextureBytes,
    },
    seek_decode: seekDecode ? {
      active: true,
      binary_bytes: seekDecode.binaryBytes,
      cold_json_chars: seekDecode.coldJsonChars,
      estimated_raw_bytes: seekRawBytes,
      estimated_decoded_bytes: seekDecodedBytes,
      decoded_rows: seekDecodedRows,
      telemetry_rows: seekDecode.decodedTelemetryRows,
      motion_rows: seekDecode.decodedMotionRows,
      motion_ex_rows: seekDecode.decodedMotionExRows,
      status_rows: seekDecode.decodedStatusRows,
      damage_rows: seekDecode.decodedDamageRows,
      lap_rows: seekDecode.decodedLapRows,
    } : { active: false },
  }
}

function markHistoryCoverage(maskValue: unknown, startValue: unknown): void {
  const mask = Number(maskValue) >>> 0
  const start = Number(startValue)
  if (!Number.isFinite(start)) return
  for (const bit of HISTORY_ROW_BITS) {
    if (mask & bit) historyCoverageStart.set(bit, Math.min(historyCoverageStart.get(bit) ?? Infinity, start))
  }
}

function historyRowMaskForV6Type(type: number): number {
  if (type >= 1 && type <= 11) return HISTORY_ROW.telemetry
  if (type === 12 || type === 14) return HISTORY_ROW.damage
  if (type >= 13 && type <= 20) return HISTORY_ROW.status
  if (type === 21) return HISTORY_ROW.motion
  if (type === 22) return HISTORY_ROW.motionEx
  if (type === 24) return HISTORY_ROW.lap
  return 0
}

function historyCovers(bit: number, requiredStart: number): boolean {
  return (historyCoverageStart.get(bit) ?? Infinity) <= requiredStart + 1
}

function invalidateHistoryCoverage(mask: number): void {
  for (const bit of HISTORY_ROW_BITS) {
    if (mask & bit) historyCoverageStart.delete(bit)
  }
}

const RENDERER_DIAGNOSTIC_INTERVAL_MS = 10_000
interface RendererDiagnostics {
  startedAt: number
  jsonBatches: number
  jsonChars: number
  jsonRows: number
  jsonParseErrors: number
  singleRows: number
  binaryBatches: number
  binaryBytes: number
  binaryRows: number
  binaryDecodeErrors: number
  resumePayloads: number
  resumeBinaryBytes: number
  resumeJsonChars: number
  recomputes: number
  rowTypes: Record<string, number>
  rowSources: Record<string, number>
  lastAnyRowAt: number | null
  lastTelemetryAt: number | null
  lastTelemetrySessionTime: number | null
  lastTelemetrySpeedKph: number | null
  lastTelemetryRpm: number | null
}

function freshRendererDiagnostics(): RendererDiagnostics {
  return {
    startedAt: Date.now(),
    jsonBatches: 0, jsonChars: 0, jsonRows: 0, jsonParseErrors: 0, singleRows: 0,
    binaryBatches: 0, binaryBytes: 0, binaryRows: 0, binaryDecodeErrors: 0,
    resumePayloads: 0, resumeBinaryBytes: 0, resumeJsonChars: 0, recomputes: 0,
    rowTypes: {}, rowSources: {}, lastAnyRowAt: null, lastTelemetryAt: null,
    lastTelemetrySessionTime: null, lastTelemetrySpeedKph: null, lastTelemetryRpm: null,
  }
}

const rendererDiagnostics = freshRendererDiagnostics()
const firstSeenRowTypes = new Set<string>()
let warnedRendererNoTelemetry = false
let additionalLoggingEnabled = false
let memoryLogEnabled = false
let rendererDiagnosticTimer: number | null = null
let memoryLogTimer: number | null = null

function incrementDiagnostic(map: Record<string, number>, key: string): void {
  map[key] = (map[key] ?? 0) + 1
}

function rowDiagnosticSummary(msg: GatewayMsg): Record<string, unknown> {
  const row = msg as unknown as Record<string, unknown>
  return {
    type: msg.type,
    sessionTime: typeof row.session_time === 'number' ? row.session_time : null,
    speedKph: typeof row.speed_kph === 'number' ? row.speed_kph : null,
    rpm: typeof row.rpm === 'number' ? row.rpm : null,
    detectedFormat: row.detected_format ?? null,
    activeFormat: row.active_format ?? null,
    override: row.override ?? null,
    playerIndex: row.player_idx ?? null,
    carCount: Array.isArray(row.cars) ? row.cars.length : null,
  }
}

function observeRendererRow(msg: GatewayMsg, source: string): void {
  const now = Date.now()
  rendererDiagnostics.lastAnyRowAt = now
  incrementDiagnostic(rendererDiagnostics.rowTypes, msg.type || '<missing-type>')
  incrementDiagnostic(rendererDiagnostics.rowSources, source)
  if (msg.type === 'telemetry') {
    const telemetry = msg as TelemetryRow
    rendererDiagnostics.lastTelemetryAt = now
    rendererDiagnostics.lastTelemetrySessionTime = telemetry.session_time
    rendererDiagnostics.lastTelemetrySpeedKph = telemetry.speed_kph
    rendererDiagnostics.lastTelemetryRpm = telemetry.rpm
  }
  if (!firstSeenRowTypes.has(msg.type)) {
    firstSeenRowTypes.add(msg.type)
    console.info(`[telemetry-diagnostics][renderer] first ${msg.type} row via ${source}: ${JSON.stringify(rowDiagnosticSummary(msg))}`)
  }
}

function binaryPreview(batch: Uint8Array, limit = 32): string {
  return Array.from(batch.subarray(0, Math.min(limit, batch.byteLength)))
    .map(value => value.toString(16).padStart(2, '0')).join(' ')
}

function logRendererHealth(): void {
  if (!additionalLoggingEnabled) return
  const now = Date.now()
  const state = useTelemetryStore.getState()
  const snapshot = {
    elapsedMs: now - rendererDiagnostics.startedAt,
    document: {
      readyState: document.readyState,
      visibilityState: document.visibilityState,
      hasFocus: document.hasFocus(),
      online: navigator.onLine,
      platform: window.platform,
    },
    bridgeAvailable: {
      telemetry: Boolean(window.telemetryBridge),
      player: Boolean(window.playerBridge),
    },
    counters: {
      ...rendererDiagnostics,
      lastAnyRowAgeMs: rendererDiagnostics.lastAnyRowAt == null ? null : now - rendererDiagnostics.lastAnyRowAt,
      lastTelemetryAgeMs: rendererDiagnostics.lastTelemetryAt == null ? null : now - rendererDiagnostics.lastTelemetryAt,
    },
    workingBuffers: {
      telemetry: telTable.length,
      motion: motTable.length,
      motionEx: motExTable.length,
      status: stsTable.length,
      damage: dmgTable.length,
      lapProgress: lapProgressTable.length,
    },
    publishedStore: {
      telemetry: state.telemetry.length,
      motion: state.motion.length,
      motionEx: state.motionEx.length,
      statusHistory: state.statusHistory.length,
      damageHistory: state.damageHistory.length,
      hasLatest: state.latest !== null,
      hasStatus: state.status !== null,
      hasLap: state.lap !== null,
      hasSession: state.session !== null,
      connected: state.isConnected,
      error: state.error,
      protocol: state.protocolStatus ? {
        override: state.protocolStatus.override,
        detected: state.protocolStatus.detected_format,
        active: state.protocolStatus.active_format,
      } : null,
    },
    selection: {
      seconds: secondsVal,
      allLapsMode,
      finiteWindowBackfillEnabled,
      waitingForAllLapsHistory,
      historyRowMask,
      historyRowMaskHex: `0x${historyRowMask.toString(16).padStart(8, '0')}`,
      requestedHistoryRowMask,
      isPlayback: isPlaybackFlag,
    },
  }
  console.info(`[telemetry-diagnostics][renderer] health: ${JSON.stringify(snapshot)}`)
  if (!warnedRendererNoTelemetry && now - rendererDiagnostics.startedAt >= RENDERER_DIAGNOSTIC_INTERVAL_MS && rendererDiagnostics.lastTelemetryAt === null) {
    warnedRendererNoTelemetry = true
    console.warn(`[telemetry-diagnostics][renderer] No telemetry row has reached the renderer after ${Math.round((now - rendererDiagnostics.startedAt) / 1000)} seconds. Compare preload delivery markers and main/native health snapshots to locate the stopped stage.`)
  }
}

function configureRendererDiagnostics(enabled: boolean): void {
  additionalLoggingEnabled = enabled
  if (rendererDiagnosticTimer !== null) {
    window.clearInterval(rendererDiagnosticTimer)
    rendererDiagnosticTimer = null
  }
  if (!enabled) return
  Object.assign(rendererDiagnostics, freshRendererDiagnostics())
  firstSeenRowTypes.clear()
  warnedRendererNoTelemetry = false
  console.info(`[telemetry-diagnostics][renderer] additional logging enabled: ${JSON.stringify({
    telemetryBridge: Boolean(window.telemetryBridge),
    playerBridge: Boolean(window.playerBridge),
    readyState: document.readyState,
    visibilityState: document.visibilityState,
  })}`)
  rendererDiagnosticTimer = window.setInterval(logRendererHealth, RENDERER_DIAGNOSTIC_INTERVAL_MS)
}

function reportRetention(): void {
  if (!memoryLogEnabled) return
  try {
    window.telemetryBridge.reportRetention(getRendererTelemetryRetentionDiagnostics())
  } catch {
    // Diagnostics must remain invisible to the telemetry hot path.
  }
}

function configureMemoryLog(enabled: boolean): void {
  memoryLogEnabled = enabled
  if (memoryLogTimer !== null) {
    window.clearInterval(memoryLogTimer)
    memoryLogTimer = null
  }
  if (!enabled) return
  reportRetention()
  memoryLogTimer = window.setInterval(reportRetention, 1000)
}

function resetSession(): void {
  // Invalidate a seek payload that is still being cooperatively decoded.
  seekTimelineGeneration++
  allLapsDriverCache.clear()
  allLapsSnapshot = null
  currentPlaybackDriver = null
  tablesFromDriverCache = false
  historyCoverageStart.clear()
  historyV6Types.clear()
  pendingV6HistoryBackfillMask = 0
  setSeekPending(false)
  authoritativeLapStatusStart = -Infinity
  authoritativeLapStatusPrefix = EMPTY.status
  analyzeLapRevisionVal++
  // Clearing replaces each table's storage, so views still held by a chart
  // until React publishes the empty views below keep only their own snapshot.
  for (const table of Object.values(TABLE_OF_FAMILY)) table.clear()
  lapState = null; lapNum = null; lapStartTime = 0; lapTrackingActive = false
  fastestLapTime = Infinity; fastestLapSet = false
  cancelFastestRecovery()
  sessionHistoryBest.clear()
  liveTyreSetsByCar.clear()
  raceEventsArr = []
  speedRpmBlocksVal = null; playbackFastestLapNum = 0
  playbackEvents = []; playbackLapTimes = {}; liveLapTimes = {}
  playbackLapCacheOrder = []
  liveLapBoundaries = []
  allLapsLapBoundaries = []
  currentStintStartTime = -Infinity
  pendingAnalyzeLapReset = false
  waitingForAllLapsHistory = false
  requestedHistoryRowMask = 0
  fuelMaxReceived = -Infinity
  set({
    // Clear every published hot/history view immediately. recompute() normally
    // republishes these from the working buffers, but requirement masks can
    // deliberately skip a family; relying on that pass leaves its last array
    // visible after playback_close.
    telemetry: EMPTY.telemetry, motion: EMPTY.motion, motionEx: EMPTY.motionEx, latest: null,
    statusHistory: EMPTY.status, damageHistory: EMPTY.damage,
    status: null, damage: null, lap: null, timing: null, allStatus: null,
    participants: null, session: null, fastestLapCarIdx: null, tyreSets: null, strategy: null,
    strategyRebuilding: false,
    fastestLapNum: null, speedRpmBlocks: null, raceEvents: [], fuelUpperLimit: null,
    ...EMPTY_ANALYZE_SLICES, analyzeLapStartTime: 0,
    analyzeLapRevision: analyzeLapRevisionVal,
    analyzeDeltaAvailable: false, analyzeTrackLengthM: 0, playbackTnrdVersion: null,
    playbackAnalysisDrivers: [], playbackDriverIndex: null,
    playbackTrackId: null, playbackTrackName: null,
    playbackLapDataCache: {},
    livePreviousLapData: null,
    liveFastestLapData: null,
    lapBoundaries: [],
    allLapsLapBoundaries: [],
    currentStintStartTime: -Infinity,
  })
}

interface TyreSample { tyre_compound: number; visual_compound: number; tyre_age_laps: number; session_time: number }

function isNewTyreStint(previous: TyreSample, current: TyreSample): boolean {
  const validCompounds = previous.tyre_compound > 0 && current.tyre_compound > 0
  const compoundChanged = validCompounds && (
    current.tyre_compound !== previous.tyre_compound ||
    current.visual_compound !== previous.visual_compound
  )
  const ageDelta = current.tyre_age_laps - previous.tyre_age_laps
  // A larger age can mean a used set was fitted. Only treat a short-interval
  // jump as a stop; a long gap can legitimately span several completed laps
  // after the renderer was hidden or status streaming was interrupted.
  const usedSetFitted = ageDelta > 1 && current.session_time - previous.session_time < 30
  return compoundChanged || ageDelta < 0 || usedSetFitted
}

function tyreSampleAt(rows: ColumnView<StatusRow>, i: number, out: TyreSample): TyreSample {
  out.tyre_compound = rows.num('tyre_compound', i)
  out.visual_compound = rows.num('visual_compound', i)
  out.tyre_age_laps = rows.num('tyre_age_laps', i)
  out.session_time = rows.time(i)
  return out
}

function findCurrentStintStart(rows: ColumnView<StatusRow>): number {
  if (rows.length === 0) return -Infinity
  let start = rows.time(0)
  let previous = tyreSampleAt(rows, 0, { tyre_compound: 0, visual_compound: 0, tyre_age_laps: 0, session_time: 0 })
  let current: TyreSample = { tyre_compound: 0, visual_compound: 0, tyre_age_laps: 0, session_time: 0 }
  for (let i = 1; i < rows.length; i++) {
    tyreSampleAt(rows, i, current)
    if (isNewTyreStint(previous, current)) start = current.session_time
    const swap = previous; previous = current; current = swap
  }
  return start
}

type LapBoundary = { lapNum: number; sessionTime: number }

// Rebuild the lightweight All/ Stint Laps axis only from requested lap
// history. This deliberately mirrors onLap's garage/attempt handling without
// invoking its snapshot, fastest-lap, or rewind side effects.
function reconstructLapBoundaries(rows: ColumnView<LapRow>): LapBoundary[] {
  const boundaries: LapBoundary[] = []
  const sessionType = useTelemetryStore.getState().session?.session_type
  const timedSession = sessionType != null && sessionType >= 1 && sessionType <= 14
  let trackedLap: number | null = null
  let tracking = false

  const removeLap = (lap: number): void => {
    const index = boundaries.findIndex(boundary => boundary.lapNum === lap)
    if (index !== -1) boundaries.splice(index, 1)
  }
  const addLap = (lapNumber: number, sessionTime: number, currentLapMs: number): void => {
    const start = sessionTime - Math.max(0, currentLapMs) / 1000
    if (!Number.isFinite(start)) return
    removeLap(lapNumber)
    boundaries.push({ lapNum: lapNumber, sessionTime: start })
  }

  for (let i = 0; i < rows.length; i++) {
    const lapNumber = rows.num('lap_num', i)
    const sessionTime = rows.time(i)
    const currentLapMs = rows.num('current_lap_ms', i)
    if (!Number.isFinite(lapNumber) || !Number.isFinite(sessionTime) ||
        !Number.isFinite(currentLapMs)) continue
    const driverStatus = rows.num('driver_status', i)
    const hasDriverStatus = Number.isFinite(driverStatus) && driverStatus >= 0
    const garageAware = timedSession && hasDriverStatus
    if (garageAware && driverStatus !== 1) {
      if (tracking && trackedLap !== null) removeLap(trackedLap)
      trackedLap = null
      tracking = false
      continue
    }
    if (!tracking) {
      trackedLap = lapNumber
      tracking = true
      addLap(lapNumber, sessionTime, currentLapMs)
      continue
    }
    if (trackedLap !== null && lapNumber < trackedLap) continue
    if (lapNumber === trackedLap) continue
    trackedLap = lapNumber
    addLap(lapNumber, sessionTime, currentLapMs)
  }
  return boundaries.sort((a, b) => a.sessionTime - b.sessionTime)
}

function upsertAllLapsBoundary(boundary: LapBoundary): void {
  if (!allLapsMode) return
  allLapsLapBoundaries = [
    ...allLapsLapBoundaries.filter(item => item.lapNum !== boundary.lapNum),
    boundary,
  ].sort((a, b) => a.sessionTime - b.sessionTime)
}

function raceEventIdentity(event: RaceEventMsg): string {
  return JSON.stringify(Object.entries(event).sort(([left], [right]) => left.localeCompare(right)))
}

function isRetirementEvent(event: RaceEventMsg): boolean {
  return event.code === 'RTMT' || (event.code === 'PENA' && event.penalty_type === 16)
}

function mergeRaceEventHistory(history: RaceEventMsg[], trailing: RaceEventMsg[]): RaceEventMsg[] {
  const seen = new Set<string>()
  const retiredCars = new Map<number, number>()
  const merged: RaceEventMsg[] = []
  for (const event of [...history, ...trailing]) {
    if (isRetirementEvent(event) && event.car_idx != null) {
      const previousIndex = retiredCars.get(event.car_idx)
      if (previousIndex !== undefined) {
        // PENA 16 carries the retirement reason. Let it enrich an earlier
        // plain RTMT without creating a second event for the same car.
        if (merged[previousIndex].code === 'RTMT' && event.code === 'PENA') {
          seen.delete(raceEventIdentity(merged[previousIndex]))
          merged[previousIndex] = event
          seen.add(raceEventIdentity(event))
        }
        continue
      }
      retiredCars.set(event.car_idx, merged.length)
    }
    const identity = raceEventIdentity(event)
    if (seen.has(identity)) continue
    seen.add(identity)
    merged.push(event)
  }
  merged.sort((left, right) => (left.session_time ?? 0) - (right.session_time ?? 0))
  return merged.length > MAX_RACE_EVENTS ? merged.slice(-MAX_RACE_EVENTS) : merged
}

function liveLapData(
  boundary: { lapNum: number; sessionTime: number } | undefined,
  endSessionTime: number,
): AnalyzeLapData | null {
  if (!boundary || endSessionTime < boundary.sessionTime) return null
  const range = <T extends { session_time: number }>(table: ColumnTable<T>): ColumnView<T> => table.frozen(
    table.lowerBound(boundary.sessionTime, true),
    table.lowerBound(endSessionTime, false),
  )
  return {
    lapNum: boundary.lapNum,
    startSessionTime: boundary.sessionTime,
    endSessionTime,
    telemetry: range(telTable),
    motion: range(motTable),
    motionEx: range(motExTable),
    statusHistory: range(stsTable),
    damageHistory: range(dmgTable),
    lapProgress: range(lapProgressTable),
    playerPositions: [],
  }
}

// Fastest and Previous are immutable lap snapshots in Zustand, so the mutable
// ingest buffers only need the current lap and two completed predecessors. The
// extra predecessor is the rewind cushion: after a short rewind crosses one lap
// boundary it becomes Previous without decoding N-3.
function trimLiveWorkingSet(): void {
  if (isPlaybackFlag || allLapsMode || liveLapBoundaries.length < 3) return
  const cutoff = liveLapBoundaries[liveLapBoundaries.length - 3].sessionTime
  telTable.trimBefore(cutoff)
  motTable.trimBefore(cutoff)
  motExTable.trimBefore(cutoff)
  lapProgressTable.trimBefore(cutoff)
  // State histories need the immediately preceding value so a lap/window that
  // starts between sparse packets can reconstruct its initial state.
  stsTable.trimBefore(cutoff, true)
  dmgTable.trimBefore(cutoff, true)
  if (liveLapBoundaries.length > 3) {
    liveLapBoundaries = liveLapBoundaries.slice(-3)
    set({ lapBoundaries: liveLapBoundaries })
  }
  invalidateHistoryCoverage(historyRowMask)
}

// Playback All Laps backfills install the complete requested prefix in the
// renderer working buffers. Leaving the mode must release that prefix without
// waiting for a seek: retain only the union required by the remaining finite
// time-window and current/comparison-lap views.
function trimPlaybackWorkingSet(): void {
  if (!isPlaybackFlag || allLapsMode) return
  const latestSessionTime = Math.max(
    ...Object.values(TABLE_OF_FAMILY).map(table => table.lastTime() ?? -Infinity),
  )
  if (!Number.isFinite(latestSessionTime)) return

  const finiteWindowStart = finiteWindowBackfillEnabled && Number.isFinite(secondsVal) && secondsVal > 0
    ? latestSessionTime - secondsVal
    : Infinity
  const currentLapStart = analyzeLapEnabled && lapTrackingActive ? lapStartTime : Infinity
  const cutoff = Math.min(finiteWindowStart, currentLapStart)
  if (!Number.isFinite(cutoff)) return

  telTable.trimBefore(cutoff)
  motTable.trimBefore(cutoff)
  motExTable.trimBefore(cutoff)
  lapProgressTable.trimBefore(cutoff)
  stsTable.trimBefore(cutoff, true)
  dmgTable.trimBefore(cutoff, true)
  invalidateHistoryCoverage(historyRowMask)
}

function applyLiveRewind(target: number): void {
  if (isPlaybackFlag || !Number.isFinite(target) || target < 0) return
  const before = useTelemetryStore.getState()
  const previousLapNum = liveLapBoundaries[liveLapBoundaries.length - 2]?.lapNum
  const oldCurrentLapNum = lapNum

  for (const table of Object.values(TABLE_OF_FAMILY)) table.truncateAt(target)
  raceEventsArr = raceEventsArr.filter(e => e.session_time == null || e.session_time <= target)
  liveLapBoundaries = liveLapBoundaries.filter(boundary => boundary.sessionTime <= target)
  allLapsLapBoundaries = allLapsLapBoundaries.filter(boundary => boundary.sessionTime <= target)
  if (allLapsMode && allLapsLapBoundaries.length > 0) {
    liveLapBoundaries = allLapsLapBoundaries.slice(-3)
  }

  const currentIndex = liveLapBoundaries.length - 1
  const currentBoundary = liveLapBoundaries[currentIndex]
  const previousBoundary = liveLapBoundaries[currentIndex - 1]
  lapNum = currentBoundary?.lapNum ?? null
  lapStartTime = currentBoundary?.sessionTime ?? target
  const lastLap = lapProgressTable.last() ?? undefined
  lapState = lastLap ?? null
  const sessionType = useTelemetryStore.getState().session?.session_type
  const garageAware = sessionType != null && sessionType >= 1 && sessionType <= 14 &&
    lastLap?.driver_status != null && lastLap.driver_status >= 0
  lapTrackingActive = !garageAware || lastLap?.driver_status === 1
  if (!lapTrackingActive) lapNum = null

  const survivingTimes: Record<number, number> = {}
  for (let i = 0; i + 1 < liveLapBoundaries.length; i++) {
    const n = liveLapBoundaries[i].lapNum
    if (liveLapTimes[n] != null) survivingTimes[n] = liveLapTimes[n]
  }
  liveLapTimes = survivingTimes
  // A flashback only invalidates Fastest when it was Previous and we crossed
  // back into that lap. Older fastest snapshots must survive buffer trimming.
  const invalidatesFastest = before.liveFastestLapData !== null &&
    before.liveFastestLapData.lapNum === previousLapNum &&
    oldCurrentLapNum !== null && currentBoundary !== undefined &&
    currentBoundary.lapNum < oldCurrentLapNum
  if (invalidatesFastest) fastestLapTime = Infinity

  const previousData = liveLapData(previousBoundary, currentBoundary?.sessionTime ?? target)
  currentStintStartTime = findCurrentStintStart(stsTable.frozen())
  analyzeLapRevisionVal++
  pendingAnalyzeLapReset = false

  set({
    lap: lapState,
    status: stsTable.last(),
    damage: dmgTable.last(),
    lapBoundaries: liveLapBoundaries,
    allLapsLapBoundaries,
    livePreviousLapData: previousData,
    ...(invalidatesFastest ? { liveFastestLapData: null, fastestLapNum: null } : {}),
    lapTimesByNum: liveLapTimes,
    raceEvents: raceEventsArr,
    currentStintStartTime,
    analyzeLapRevision: analyzeLapRevisionVal,
  })
  // Supersede an in-flight native result after any further flashback.
  if (invalidatesFastest || fastestRecoveryPending) {
    scheduleFastestRecovery()
  }
}

// The old useEffect([lap]): on a lap-number change, snapshot the completed lap
// and update live lap times / fastest lap. Runs in the 'lap' handler now.
function onLap(lap: LapRow): void {
  if (Number.isFinite(lap.lap_distance_m)) appendRow(lapProgressTable, lap, MAX_ROWS)
  lapState = lap
  set({ lap })
  const packetLapStart = lap.session_time - Math.max(0, lap.current_lap_ms) / 1000
  const sessionType = useTelemetryStore.getState().session?.session_type
  const timedSession = sessionType != null && sessionType >= 1 && sessionType <= 14
  const hasDriverStatus = lap.driver_status != null && lap.driver_status >= 0
  const garageAware = timedSession && hasDriverStatus

  if (garageAware && lap.driver_status !== 1) {
    if (lapTrackingActive) {
      liveLapBoundaries = liveLapBoundaries.filter(boundary => boundary.lapNum !== lapNum)
      if (allLapsMode && lapNum !== null) {
        allLapsLapBoundaries = allLapsLapBoundaries.filter(boundary => boundary.lapNum !== lapNum)
      }
      lapNum = null
      lapStartTime = lap.session_time
      lapTrackingActive = false
      pendingAnalyzeLapReset = false
      analyzeLapRevisionVal++
      set({
        lapBoundaries: liveLapBoundaries,
        allLapsLapBoundaries,
        ...EMPTY_ANALYZE_SLICES,
        analyzeLapStartTime: lapStartTime,
        analyzeLapRevision: analyzeLapRevisionVal,
      })
    }
    return
  }

  if (garageAware && !lapTrackingActive) {
    lapNum = lap.lap_num
    lapStartTime = packetLapStart
    lapTrackingActive = true
    liveLapBoundaries = [
      ...liveLapBoundaries.filter(boundary => boundary.lapNum !== lap.lap_num),
      { lapNum: lap.lap_num, sessionTime: packetLapStart },
    ].sort((a, b) => a.sessionTime - b.sessionTime)
    upsertAllLapsBoundary({ lapNum: lap.lap_num, sessionTime: packetLapStart })
    analyzeLapRevisionVal++
    pendingAnalyzeLapReset = true
    set({
      lapBoundaries: liveLapBoundaries,
      allLapsLapBoundaries,
      ...EMPTY_ANALYZE_SLICES,
      analyzeLapStartTime: lapStartTime,
      analyzeLapRevision: analyzeLapRevisionVal,
    })
    return
  }

  const prevLapNum = lapNum
  // A replay opened without FLBK can emit one stale lap snapshot after the
  // timeline has already advanced. Explicit rewind handling lowers lapNum
  // before packets resume; an otherwise decreasing number is just stale and
  // must not create a backwards chart boundary.
  if (prevLapNum !== null && lap.lap_num < prevLapNum) return
  lapNum = lap.lap_num
  lapTrackingActive = true

  if (prevLapNum === null) {
    lapStartTime = packetLapStart
    liveLapBoundaries = [{ lapNum: lap.lap_num, sessionTime: packetLapStart }]
    upsertAllLapsBoundary(liveLapBoundaries[0])
    set({ lapBoundaries: liveLapBoundaries, allLapsLapBoundaries })
    // Initial lap metadata (including metadata re-emitted after a playback
    // backfill) establishes the lap origin; it is not a chart reset boundary.
    // Recording load/seek/session reset already publish an explicit revision.
    return
  }
  if (lap.lap_num === prevLapNum) return

  liveLapBoundaries = [
    ...liveLapBoundaries.filter(boundary => boundary.lapNum !== lap.lap_num),
    { lapNum: lap.lap_num, sessionTime: packetLapStart },
  ].sort((a, b) => a.sessionTime - b.sessionTime)
  upsertAllLapsBoundary({ lapNum: lap.lap_num, sessionTime: packetLapStart })
  set({ lapBoundaries: liveLapBoundaries, allLapsLapBoundaries })

  let completedLapData: AnalyzeLapData | null = null
  if (!isPlaybackFlag) {
    const completed = useTelemetryStore.getState()
    // Published lap slices are frozen column views, so the snapshot can hold
    // them directly; later appends and trims never rewrite their rows.
    completedLapData = {
      lapNum: prevLapNum,
      startSessionTime: lapStartTime,
      endSessionTime: packetLapStart,
      telemetry: completed.analyzeLapTelemetry,
      motion: completed.analyzeLapMotion,
      motionEx: completed.analyzeLapMotionEx,
      statusHistory: completed.analyzeLapStatusHistory,
      damageHistory: completed.analyzeLapDamageHistory,
      lapProgress: completed.analyzeLapProgress,
      playerPositions: [],
    }
    set({ livePreviousLapData: completedLapData })
  }

  analyzeLapRevisionVal++
  pendingAnalyzeLapReset = true

  // Playback has an authoritative session-wide fastest lap from its index.
  // Never let whichever lap happens to cross a boundary after a seek replace it.
  if (!isPlaybackFlag && completedLapData) {
    const lapTimeMs = lap.last_lap_ms
    if (lapTimeMs > 0 && lapTimeMs < 300_000) {
      if (liveLapTimes[prevLapNum] !== lapTimeMs) liveLapTimes = { ...liveLapTimes, [prevLapNum]: lapTimeMs }
    }
    if (lapTimeMs > 0 && lapTimeMs < 300_000 && lapTimeMs < fastestLapTime &&
        completedLapData.telemetry.length > 0 && completedLapData.lapProgress.length > 0) {
      fastestLapTime = lapTimeMs
      cancelFastestRecovery()
      set({ fastestLapNum: prevLapNum, liveFastestLapData: completedLapData })
    }
  }
  lapStartTime = packetLapStart
  trimLiveWorkingSet()
}

function handleMsg(msg: GatewayMsg): void {
  switch (msg.type) {
    case 'playback_loaded': {
      set({
        playbackTrackId: msg.ok ? msg.header?.track_id ?? null : null,
        playbackTrackName: msg.ok ? msg.header?.track_name ?? null : null,
      })
      break
    }
    case 'playback_close': {
      isPlaybackFlag = false
      resetSession()
      break
    }
    case 'telemetry': {
      const lastTime = telTable.lastTime()
      if (lastTime !== undefined && msg.session_time < lastTime && !isPlaybackFlag) {
        // Playback can deliver a slightly older hot row around a seek/backfill
        // boundary. appendRow reconciles the renderer history below, but this
        // must not clear the Analyze GPU buffers. Explicit seek flushes carry
        // the revision that identifies a real timeline reset.
        applyLiveRewind(msg.session_time)
      }
      appendPlaybackPatch(telTable, msg as TelemetryRow, MAX_ROWS)
      if (isPlaybackFlag && !(historyRowMask & HISTORY_ROW.telemetry)) telTable.keepLastOnly()
      break
    }
    case 'motion': {
      appendPlaybackPatch(motTable, msg as MotionRow, MAX_ROWS)
      break
    }
    case 'motion_ex': {
      appendPlaybackPatch(motExTable, msg as MotionExRow, MAX_ROWS)
      break
    }
    case 'status': {
      const previous = stsTable.last() ?? undefined
      const merged = mergePlaybackPatch(previous, msg as StatusRow)
      const next: Partial<TelemetryStoreState> = { status: merged }
      const previousStintStartTime = currentStintStartTime
      if (!previous || merged.session_time < previous.session_time) {
        currentStintStartTime = merged.session_time
      } else if (isNewTyreStint(previous, merged)) {
        currentStintStartTime = merged.session_time
      }
      if (previousStintStartTime !== currentStintStartTime) next.currentStintStartTime = currentStintStartTime
      if (!isPlaybackFlag && Number.isFinite(msg.fuel_kg) && msg.fuel_kg >= 0 && msg.fuel_kg > fuelMaxReceived) {
        fuelMaxReceived = msg.fuel_kg
        // Keep exactly one kilogram of breathing room above the highest value
        // received in this live session.
        next.fuelUpperLimit = fuelMaxReceived + 1
      }
      set(next)
      appendPlaybackPatch(stsTable, msg as StatusRow, MAX_ROWS)
      if (isPlaybackFlag && !(historyRowMask & HISTORY_ROW.status)) stsTable.keepLastOnly()
      break
    }
    case 'damage': {
      const previous = dmgTable.last() ?? undefined
      const merged = mergePlaybackPatch(previous, msg as DamageRow)
      set({ damage: merged })
      appendPlaybackPatch(dmgTable, msg as DamageRow, MAX_ROWS)
      if (isPlaybackFlag && !(historyRowMask & HISTORY_ROW.damage)) dmgTable.keepLastOnly()
      break
    }
    case 'lap': {
      const previous = useTelemetryStore.getState().lap
      onLap((isPlaybackFlag && previous ? { ...previous, ...msg } : msg) as unknown as LapRow)
      break
    }
    case 'timing': {
      set(state => ({ timing: mergeCarPatches(state.timing, msg as TimingMsg) as TimingMsg }))
      // The player's Tyre Sets packet may have arrived before the player index.
      const { timing, tyreSets } = useTelemetryStore.getState()
      if (!isPlaybackFlag && timing) {
        const playerSets = liveTyreSetsByCar.get(timing.player_idx)
        if (playerSets && playerSets !== tyreSets) set({ tyreSets: playerSets })
      }
      break
    }
    case 'participants': {
      const incoming = msg as ParticipantsMsg
      const previous = useTelemetryStore.getState().participants
      if (!previous) {
        set({ participants: incoming })
        break
      }
      // Participant packets can briefly report fewer active slots while timing
      // still references the established grid. Retain known identities by car
      // index and apply incoming changes instead of replacing the whole roster.
      const drivers = new Map(previous.drivers.map(driver => [driver.idx, driver]))
      for (const driver of incoming.drivers) drivers.set(driver.idx, driver)
      set({ participants: {
        ...previous,
        ...incoming,
        drivers: [...drivers.values()].sort((left, right) => left.idx - right.idx),
      } })
      break
    }
    case 'all_status':   set(state => ({ allStatus: mergeCarPatches(state.allStatus, msg as AllStatusMsg) as AllStatusMsg })); break
    case 'fastest_lap':
      set({ fastestLapCarIdx: (msg as any).car_idx })
      fastestLapSet = true
      break
    case 'session_history_fastest': {
      if (fastestLapSet) break
      const shMsg = msg as SessionHistoryFastestMsg
      sessionHistoryBest.set(shMsg.car_idx, shMsg.best_lap_time_ms)
      let minMs = Infinity
      let minIdx: number | null = null
      for (const [idx, ms] of sessionHistoryBest) {
        if (ms < minMs) { minMs = ms; minIdx = idx }
      }
      if (useTelemetryStore.getState().fastestLapCarIdx !== minIdx) set({ fastestLapCarIdx: minIdx })
      break
    }
    case 'tyre_sets': {
      // Playback already projects only the selected driver's sets, and older
      // rows without car_idx were filtered to the player natively.
      if (isPlaybackFlag || msg.car_idx == null) {
        set({ tyreSets: msg })
        break
      }
      liveTyreSetsByCar.set(msg.car_idx, msg)
      if (msg.car_idx === useTelemetryStore.getState().timing?.player_idx) set({ tyreSets: msg })
      break
    }
    // The seek gate drops Strategy rows until the seek installs, and the engine
    // withholds tick snapshots until its rebuild commits, so the first one
    // after a seek is the rebuilt result.
    case 'strategy':     set({ strategy: msg as StrategySnapshotMsg, strategyRebuilding: false }); break
    case 'race_event':
      if ((msg as RaceEventMsg).code === 'FLBK') {
        const target = Number((msg as RaceEventMsg).flashback_session_time)
        if (Number.isFinite(target) && target >= 0) applyLiveRewind(target)
      }
      const raceEvent = msg as RaceEventMsg
      if (isRetirementEvent(raceEvent) && raceEvent.car_idx != null) {
        const priorRetirement = raceEventsArr.findIndex(event =>
          isRetirementEvent(event) && event.car_idx === raceEvent.car_idx)
        if (priorRetirement !== -1) {
          if (raceEventsArr[priorRetirement].code === 'RTMT' && raceEvent.code === 'PENA') {
            raceEventsArr = [
              ...raceEventsArr.slice(0, priorRetirement),
              ...raceEventsArr.slice(priorRetirement + 1),
              raceEvent,
            ]
          }
          break
        }
      }
      for (const cb of raceEventListeners) cb(raceEvent)
      raceEventsArr = [...raceEventsArr, raceEvent]
      if (raceEventsArr.length > MAX_RACE_EVENTS) raceEventsArr = raceEventsArr.slice(-MAX_RACE_EVENTS)
      if ((msg as any).code === 'SEND' && !isPlaybackFlag) {
        resetSession()
      }
      break
    case 'session':
      set({
        session: msg,
        ...(!isPlaybackFlag && Number.isFinite(msg.track_length_m) && msg.track_length_m > 0
          ? { analyzeTrackLengthM: msg.track_length_m }
          : {}),
      })
      break
    case 'protocol_status': set({ protocolStatus: msg }); break
    case 'protocol_warning': {
      const pw = msg as ProtocolWarningMsg
      set({ protocolWarning: (pw.detected_format === null || pw.detected_format === undefined) ? null : pw })
      break
    }
    case 'live_fastest_lap_data': {
      if (isPlaybackFlag || msg.requestId !== fastestRecoveryGeneration ||
          useTelemetryStore.getState().liveFastestLapData) break
      const lapTables = {
        telemetry: new ColumnTable<TelemetryRow>('telemetry'),
        motion: new ColumnTable<MotionRow>('motion'),
        motion_ex: new ColumnTable<MotionExRow>('motion_ex'),
        status: new ColumnTable<StatusRow>('status'),
        damage: new ColumnTable<DamageRow>('damage'),
        lap: new ColumnTable<LapRow>('lap'),
      }
      forEachDecodedBinaryRow(Uint8Array.from(msg.binary), row => {
        if (row.type === 'telemetry' || row.type === 'motion' || row.type === 'motion_ex')
          lapTables[row.type].append(row as any)
      })
      for (const row of msg.rows) {
        if (row.type === 'status' || row.type === 'damage' || row.type === 'lap')
          lapTables[row.type].append(row as any)
      }
      const data: AnalyzeLapData = {
        lapNum: msg.lapNum, startSessionTime: msg.startSessionTime,
        endSessionTime: msg.endSessionTime,
        telemetry: lapTables.telemetry.frozen(), motion: lapTables.motion.frozen(),
        motionEx: lapTables.motion_ex.frozen(), statusHistory: lapTables.status.frozen(),
        damageHistory: lapTables.damage.frozen(), lapProgress: lapTables.lap.frozen(),
        playerPositions: [],
      }
      if (!data.telemetry.length || !data.lapProgress.length) break
      fastestLapTime = msg.lapTimeMs
      cancelFastestRecovery()
      set({ liveFastestLapData: data, fastestLapNum: data.lapNum })
      break
    }
    case 'playback_lap_data': {
      const payload = msg as PlaybackLapDataMsg
      const lapData: AnalyzeLapData = {
        lapNum: payload.lapNum,
        startSessionTime: payload.startSessionTime,
        endSessionTime: payload.endSessionTime,
        telemetry: viewOfRows<TelemetryRow>('telemetry', payload.telemetry ?? [], isPlaybackFlag),
        motion: viewOfRows<MotionRow>('motion', payload.motionHistory ?? [], isPlaybackFlag),
        motionEx: viewOfRows<MotionExRow>('motion_ex', payload.motionExHistory ?? [], isPlaybackFlag),
        statusHistory: viewOfRows<StatusRow>('status', payload.statusHistory ?? [], isPlaybackFlag),
        damageHistory: viewOfRows<DamageRow>('damage', payload.damageHistory ?? [], isPlaybackFlag),
        lapProgress: viewOfRows<LapProgressPoint>('lap', payload.lapProgress ?? [], false),
        playerPositions: payload.playerPositions ?? [],
        rowTypeMask: payload.rowTypeMask ?? 0xFFFFFFFF,
      }
      playbackLapCacheOrder = [...playbackLapCacheOrder.filter(lapNum => lapNum !== lapData.lapNum), lapData.lapNum]
      set(state => {
        const prior = state.playbackLapDataCache[lapData.lapNum]
        const cache = {
          ...state.playbackLapDataCache,
          [lapData.lapNum]: prior ? mergeAnalyzeLapData(prior, lapData) : lapData,
        }
        while (playbackLapCacheOrder.length > MAX_ANALYZE_LAP_CACHE) {
          const evicted = playbackLapCacheOrder.shift()
          if (evicted !== undefined) delete cache[evicted]
        }
        return { playbackLapDataCache: cache }
      })
      break
    }
    case 'playback_seek_flush_bin': {
      void processPlaybackSeekFlush(msg as any).catch(error => {
        console.error('Failed to decode playback seek flush:', error)
        waitingForAllLapsHistory = false
        if ((msg as any).authoritativeSeek !== false) setSeekPending(false)
        recompute(DirtySlice.All)
        const requestId = Number((msg as any).requestId)
        if (Number.isFinite(requestId) && requestId > 0)
          window.playerBridge.seekInstalled(requestId)
      })
      break
    }
    case 'playback_seek_flush_failed':
      waitingForAllLapsHistory = false
      setSeekPending(false)
      // A failed seek queues no Strategy rebuild; keep the previous snapshot.
      set({ strategyRebuilding: false })
      requestedHistoryRowMask = 0
      break
    case 'playback_lap_blocks': {
      cancelFastestRecovery()
      isPlaybackFlag = true
      analyzeLapRevisionVal++
      const data = msg as any
      const nextDriver: number | null = Number.isFinite(data.playbackDriverIndex) ? data.playbackDriverIndex : null
      const driverChanged = nextDriver !== currentPlaybackDriver
      if (driverChanged && currentPlaybackDriver !== null && allLapsSnapshot?.driver === currentPlaybackDriver)
        rememberDriverHistory(currentPlaybackDriver, allLapsSnapshot.views)
      allLapsSnapshot = null
      currentPlaybackDriver = nextDriver
      tablesFromDriverCache = false
      fuelMaxReceived = -Infinity
      playbackLapCacheOrder = []
      historyCoverageStart.clear()
      historyV6Types.clear()
      pendingV6HistoryBackfillMask = 0
      requestedHistoryRowMask = 0
      waitingForAllLapsHistory = false
      authoritativeLapStatusStart = -Infinity
      authoritativeLapStatusPrefix = EMPTY.status
      for (const table of Object.values(TABLE_OF_FAMILY)) table.clear()
      allLapsLapBoundaries = []
      speedRpmBlocksVal = data.blocks
      playbackFastestLapNum = data.fastestLapNum
      playbackEvents = mergeRaceEventHistory(data.events ?? [], [])
      const map: Record<number, number> = {}
      for (const l of (data.laps ?? []) as { lapNum: number; lapTimeMs: number }[]) {
        if (l.lapTimeMs > 0) map[l.lapNum] = l.lapTimeMs
      }
      playbackLapTimes = map
      const initialFuelKg = Number(data.initialFuelKg)
      const trackLengthM = Number(data.trackLengthM)
      set({
        speedRpmBlocks: speedRpmBlocksVal,
        fastestLapNum: playbackFastestLapNum || null,
        playbackLapDataCache: {},
        allLapsLapBoundaries: [],
        liveFastestLapData: null,
        fuelUpperLimit: Number.isFinite(initialFuelKg) && initialFuelKg >= 0
          ? initialFuelKg + 1
          : null,
        analyzeDeltaAvailable: data.lapDistanceAvailable === true || data.deltaAvailable === true,
        analyzeTrackLengthM: Number.isFinite(trackLengthM) && trackLengthM > 0 ? trackLengthM : 0,
        playbackTnrdVersion: typeof data.tnrdVersion === 'string' ? data.tnrdVersion : null,
        playbackAnalysisDrivers: Array.isArray(data.analysisDrivers) ? data.analysisDrivers : [],
        playbackDriverIndex: Number.isFinite(data.playbackDriverIndex)
          ? data.playbackDriverIndex : null,
      })
      if (allLapsMode && seekRendererPending) {
        // The seek that follows a driver change is already in flight and, in
        // All Laps, carries the full prefix. A second full-session request here
        // used to extract and merge the entire race twice per switch.
        requestedHistoryRowMask |= fullSessionHistoryRowMask
        const cached = driverChanged && nextDriver !== null ? allLapsDriverCache.get(nextDriver) : undefined
        if (cached) {
          for (const family of Object.keys(TABLE_OF_FAMILY) as HistoryFamily[])
            TABLE_OF_FAMILY[family].replaceWithView(cached[family])
          tablesFromDriverCache = true
          waitingForAllLapsHistory = false
          currentStintStartTime = findCurrentStintStart(stsTable.frozen())
          set({ currentStintStartTime })
          recompute(DirtySlice.All)
        } else {
          waitingForAllLapsHistory = true
        }
      } else {
        const missingHistory = fullSessionHistoryRowMask & ~requestedHistoryRowMask
        if (allLapsMode && missingHistory !== 0) {
          waitingForAllLapsHistory = true
          requestedHistoryRowMask |= missingHistory
          window.playerBridge.getAllLapsData(missingHistory)
        }
      }
      requestVisibleWindowHistory()
      break
    }
  }
}

const enum DirtySlice {
  None = 0,
  Telemetry = 1 << 0,
  Motion = 1 << 1,
  MotionEx = 1 << 2,
  Status = 1 << 3,
  Damage = 1 << 4,
  Lap = 1 << 5,
  Derived = 1 << 6,
  All = Telemetry | Motion | MotionEx | Status | Damage | Lap | Derived,
}

function dirtySliceFor(msg: { type: string }): DirtySlice {
  switch (msg.type) {
    case 'telemetry': return DirtySlice.Telemetry | DirtySlice.Derived
    case 'motion': return DirtySlice.Motion
    case 'motion_ex': return DirtySlice.MotionEx
    case 'status': return DirtySlice.Status | DirtySlice.Derived
    case 'damage': return DirtySlice.Damage
    case 'lap': return DirtySlice.Lap | DirtySlice.Derived
    case 'race_event':
      return (msg as RaceEventMsg).code === 'FLBK' ? DirtySlice.All : DirtySlice.Derived
    case 'playback_lap_blocks': return DirtySlice.Derived
    case 'playback_close':
    case 'playback_seek_flush_failed': return DirtySlice.All
    case 'playback_seek_flush_bin': return DirtySlice.None
    default: return DirtySlice.None
  }
}

// Recompute only the window groups touched by a delivered batch. Unchanged
// groups retain their array identity, so their Zustand subscribers stay cold.
function recompute(dirty: DirtySlice): void {
  if (dirty === DirtySlice.None) return
  // A normal seek flush only contains the current lap. In AL keep the last
  // complete chart publication visible until the single prefix flush arrives,
  // otherwise the graph briefly collapses to one lap and then expands again.
  if (waitingForAllLapsHistory) return
  const latestSessionTime = telTable.lastTime() ?? 0
  const cutoff = allLapsMode ? -Infinity : latestSessionTime - secondsVal
  // `current_lap_ms` is legitimately zero at rollover. Falling back to session
  // origin in that state prepends the previous lap to every Analyze/CL slice,
  // so distance mapping stops at the stale prefix and charts retain one point.
  const lapStartSessionTime = lapTrackingActive ? lapStartTime : latestSessionTime
  const isPlayback = speedRpmBlocksVal !== null
  const lapTimesByNum = isPlayback ? playbackLapTimes : liveLapTimes

  // Live sessions always retain these slices because the completed current lap
  // becomes the in-memory previous/fastest-lap cache at rollover. Playback has
  // an indexed lap cache, so it can skip the slices when no distance view uses
  // them without losing data needed by a later view switch.
  const needsAnalyzeSlices = analyzeLapEnabled || !isPlayback
  let publishAnalyze = needsAnalyzeSlices && lapTrackingActive && !pendingAnalyzeLapReset
  if (needsAnalyzeSlices && pendingAnalyzeLapReset) {
    const countSince = (table: ColumnTable<any>) => table.length - table.lowerBound(lapStartSessionTime, true)
    const ready = (bit: number, count: number, minimum: number) =>
      !(historyRowMask & bit) || count >= minimum
    publishAnalyze = ready(HISTORY_ROW.telemetry, countSince(telTable), 2) &&
      ready(HISTORY_ROW.motion, countSince(motTable), 1) &&
      ready(HISTORY_ROW.motionEx, countSince(motExTable), 1) &&
      ready(HISTORY_ROW.status, countSince(stsTable), 1) &&
      ready(HISTORY_ROW.damage, countSince(dmgTable), 1) &&
      ready(HISTORY_ROW.lap, countSince(lapProgressTable), 2)
    if (publishAnalyze) {
      pendingAnalyzeLapReset = false
      dirty |= DirtySlice.All
    }
  }

  // Finite windows publish a frozen range; All Laps publishes the table's live
  // view, which grows in place and wakes its charts through
  // subscribeAllLapsData instead of a new React value per packet.
  const windowOf = <T extends { session_time: number }>(table: ColumnTable<T>): ColumnView<T> =>
    allLapsMode ? table.liveView() : table.frozen(table.lowerBound(cutoff, false))
  const lapSlice = <T extends { session_time: number }>(table: ColumnTable<T>, withPredecessor = false): ColumnView<T> => {
    const start = table.lowerBound(lapStartSessionTime, true)
    return table.frozen(withPredecessor ? Math.max(0, start - 1) : start)
  }

  const next: Partial<TelemetryStoreState> = {}
  if (dirty & DirtySlice.Telemetry) {
    next.latest = telTable.last()
    if (historyRowMask & HISTORY_ROW.telemetry) {
      next.telemetry = windowOf(telTable)
      if (publishAnalyze) next.analyzeLapTelemetry = lapSlice(telTable)
    }
  }
  if (dirty & DirtySlice.Motion) {
    next.motion = windowOf(motTable)
    if (publishAnalyze) next.analyzeLapMotion = lapSlice(motTable)
  }
  if (dirty & DirtySlice.MotionEx) {
    next.motionEx = windowOf(motExTable)
    if (publishAnalyze) next.analyzeLapMotionEx = lapSlice(motExTable)
  }
  if (dirty & DirtySlice.Status) {
    if (historyRowMask & HISTORY_ROW.status) {
      next.statusHistory = windowOf(stsTable)
      let current: ColumnView<StatusRow> = stsTable.frozen()
      if (publishAnalyze && isPlayback &&
          Math.abs(authoritativeLapStatusStart - lapStartSessionTime) < 0.05 &&
          authoritativeLapStatusPrefix.length > 0) {
        const prefix = authoritativeLapStatusPrefix
        const prefixFirst = prefix.time(0)
        const prefixLast = prefix.time(prefix.length - 1)
        const currentFirst = current.length ? current.time(0) : Infinity
        const currentLast = current.length ? current.time(current.length - 1) : -Infinity
        const stillContainsPrefix = current.length >= prefix.length &&
          currentFirst <= prefixFirst && currentLast >= prefixLast
        if (!stillContainsPrefix) {
          // A late state-only/secondary response must not replace the complete
          // status history installed by the authoritative seek. Keep that
          // immutable prefix and add only genuinely newer streamed samples.
          current = concatAfter(prefix, current)
        }
      }
      if (publishAnalyze) {
        next.analyzeLapStatusHistory = current.slice(Math.max(0, current.lowerBound(lapStartSessionTime, true) - 1))
      }
    }
  }
  if (dirty & DirtySlice.Damage) {
    if (historyRowMask & HISTORY_ROW.damage) {
      next.damageHistory = windowOf(dmgTable)
      if (publishAnalyze) next.analyzeLapDamageHistory = lapSlice(dmgTable, true)
    }
  }
  if (dirty & DirtySlice.Lap) {
    if (publishAnalyze) next.analyzeLapProgress = lapSlice(lapProgressTable)
  }
  if (dirty & DirtySlice.Derived) {
    // This revision also identifies authoritative playback timeline installs.
    // Publish it even when no distance/analyze consumer is active so time-axis
    // charts can invalidate joins made against the pre-seek history.
    next.analyzeLapRevision = analyzeLapRevisionVal
    next.lapTimesByNum = lapTimesByNum
    if (isPlayback) {
      // Playback events are sorted once at load. Resolve the visible prefix by
      // binary search and only publish a new array when an event boundary is
      // actually crossed; telemetry frames between events stay allocation-free.
      let lo = 0, hi = playbackEvents.length
      while (lo < hi) {
        const mid = (lo + hi) >> 1
        if ((playbackEvents[mid].session_time ?? 0) <= latestSessionTime) lo = mid + 1
        else hi = mid
      }
      if (useTelemetryStore.getState().raceEvents.length !== lo) {
        next.raceEvents = playbackEvents.slice(0, lo)
      }
    } else if (useTelemetryStore.getState().raceEvents !== raceEventsArr) {
      next.raceEvents = raceEventsArr
    }
    if (publishAnalyze) {
      next.analyzeLapStartTime = lapStartSessionTime
    }
  }
  // The All Laps live views keep their identity across appends; do not hand
  // React an unchanged value for them.
  const state = useTelemetryStore.getState()
  for (const key of ['telemetry', 'motion', 'motionEx', 'statusHistory', 'damageHistory'] as const) {
    if (next[key] !== undefined && next[key] === state[key]) delete next[key]
  }
  if (Object.keys(next).length > 0) set(next)
  if (allLapsMode && (dirty & (DirtySlice.Telemetry | DirtySlice.Motion | DirtySlice.MotionEx | DirtySlice.Status | DirtySlice.Damage))) {
    let changedHistoryMask = 0
    if (dirty & DirtySlice.Telemetry) changedHistoryMask |= HISTORY_ROW.telemetry
    if (dirty & DirtySlice.Motion) changedHistoryMask |= HISTORY_ROW.motion
    if (dirty & DirtySlice.MotionEx) changedHistoryMask |= HISTORY_ROW.motionEx
    if (dirty & DirtySlice.Status) changedHistoryMask |= HISTORY_ROW.status
    if (dirty & DirtySlice.Damage) changedHistoryMask |= HISTORY_ROW.damage
    scheduleAllLapsDataNotification(changedHistoryMask)
  }
}

function dirtySliceForHistoryMask(value: unknown): DirtySlice {
  const mask = Number(value)
  if (!Number.isFinite(mask)) return DirtySlice.All
  let dirty = DirtySlice.Derived
  if (mask & HISTORY_ROW.telemetry) dirty |= DirtySlice.Telemetry
  if (mask & HISTORY_ROW.motion) dirty |= DirtySlice.Motion
  if (mask & HISTORY_ROW.motionEx) dirty |= DirtySlice.MotionEx
  if (mask & HISTORY_ROW.status) dirty |= DirtySlice.Status
  if (mask & HISTORY_ROW.damage) dirty |= DirtySlice.Damage
  if (mask & HISTORY_ROW.lap) dirty |= DirtySlice.Lap
  return dirty
}

function tableSpan(table: ColumnTable<any>): { rows: number; first: number | null; last: number | null } {
  return { rows: table.length, first: table.firstTime() ?? null, last: table.lastTime() ?? null }
}

async function processPlaybackSeekFlush(payload: any): Promise<void> {
  const allHistory = payload.allHistory === true
  const authoritative = payload.authoritativeSeek !== false
  // Live range backfills are decoded cooperatively. Keep the pre-request list
  // only to identify genuinely newer streamed events when installing the
  // authoritative historical prefix.
  const raceEventsAtDecodeStart = raceEventsArr
  const generation = authoritative ? ++seekTimelineGeneration : seekTimelineGeneration
  const cancelled = () => generation !== seekTimelineGeneration
  const seekRetention = {
    binaryBytes: Number(payload.binary?.byteLength ?? payload.binary?.length ?? 0),
    coldJsonChars: typeof payload.coldJson === 'string' ? payload.coldJson.length : 0,
    decodedTelemetryRows: 0,
    decodedMotionRows: 0,
    decodedMotionExRows: 0,
    decodedStatusRows: 0,
    decodedDamageRows: 0,
    decodedLapRows: 0,
  }
  activeSeekDecodeRetention = seekRetention

  try {
    playbackDebug('history-flush-state-before-decode', {
      requestId: payload.requestId,
      authoritative,
      allHistory,
      rowTypeMask: `0x${(Number(payload.rowTypeMask) >>> 0).toString(16)}`,
      historyStart: payload.historyStart,
      seekRendererPending,
      historyV6Types: [...historyV6Types].sort((a, b) => a - b),
      pendingV6HistoryBackfillMask: `0x${pendingV6HistoryBackfillMask.toString(16)}`,
      telemetry: tableSpan(telTable),
      status: tableSpan(stsTable),
    })
    playbackDebug('seek-flush-received', {
    lapNum: payload.lapNum,
    currentLapStart: payload.currentLapStart,
    binaryBytes: payload.binary?.byteLength ?? payload.binary?.length ?? null,
    coldJsonChars: typeof payload.coldJson === 'string' ? payload.coldJson.length : null,
    fastestLapNum: playbackFastestLapNum || null,
  })

  if (authoritative) {
    historyCoverageStart.clear()
    // AL cleared these at seek-start. Other modes retain the last publication
    // for display, but their working tables must start collecting only rows
    // from the newly committed timeline while the backfill decodes.
    if (!allHistory || tablesFromDriverCache) {
      for (const table of Object.values(TABLE_OF_FAMILY)) table.clear()
      tablesFromDriverCache = false
    }
    analyzeLapRevisionVal++
    pendingAnalyzeLapReset = false
    lapStartTime = payload.currentLapStart
    lapNum = payload.lapNum
    lapTrackingActive = true
    set({ fastestLapNum: playbackFastestLapNum || null })
  }

  // Let Chromium service the input/paint that delivered this IPC message
  // before allocating and decoding the history payload.
  await yieldToMainThread()
  if (cancelled()) return

  const binaryInput = payload.binary as Uint8Array | ArrayBuffer | null | undefined
  const binary = binaryInput instanceof Uint8Array ? binaryInput
    : binaryInput ? new Uint8Array(binaryInput) : new Uint8Array(0)
  // Families whose rows are V6 patches install as a time-ordered overlay;
  // other additive history fills only the missing prefix.
  const incoming: Partial<Record<HistoryFamily, ColumnTable<any>>> = {}
  const overlay = new Set<HistoryFamily>()
  const raceEvents: RaceEventMsg[] = []
  const decodedV6Types: Record<string, number> = {}

  if (isV6HistoryPayload(binary)) {
    // TNRD V6: typed column blocks straight from the recording. No JSON, no
    // per-sample objects; the decode yields between slices like the old one.
    const decoded = await decodeV6History(binary, async () => {
      await yieldToMainThread()
      return !cancelled()
    })
    if (!decoded || cancelled()) return
    Object.assign(incoming, decoded.tables)
    Object.assign(decodedV6Types, decoded.typeCounts)
    if (isPlaybackFlag) for (const family of Object.keys(decoded.tables) as HistoryFamily[]) overlay.add(family)
  } else {
    // V1-V5 recordings (and V6 on hosts without columnar history): packed hot
    // rows plus JSON cold rows, appended into temporary tables.
    const tableFor = (family: HistoryFamily): ColumnTable<any> =>
      incoming[family] ??= new ColumnTable<any>(family)
    const add = (family: HistoryFamily, row: Record<string, any>): void => {
      if (isPlaybackFlag && Number.isInteger(Number(row._v6_type))) {
        overlay.add(family)
        tableFor(family).appendPatch(row as any)
      } else {
        tableFor(family).append(row as any)
      }
    }
    let binaryOffset = 0
    while (binaryOffset < binary.byteLength) {
      binaryOffset = decodeBinaryBatchRange(binary, row => {
        if (row.type === 'telemetry' || row.type === 'motion' || row.type === 'motion_ex') add(row.type, row)
      }, binaryOffset, 4096)
      seekRetention.decodedTelemetryRows = incoming.telemetry?.length ?? 0
      seekRetention.decodedMotionRows = incoming.motion?.length ?? 0
      seekRetention.decodedMotionExRows = incoming.motion_ex?.length ?? 0
      if (binaryOffset < binary.byteLength) {
        await yieldToMainThread()
        if (cancelled()) return
      }
    }

    const coldJson = (payload.coldJson as string) || ''
    let start = 0
    let rowsSinceYield = 0
    while (start < coldJson.length) {
      let end = coldJson.indexOf('\n', start)
      if (end === -1) end = coldJson.length
      if (end > start) {
        try {
          const row = JSON.parse(coldJson.slice(start, end)) as GatewayMsg
          const v6Type = Number((row as any)._v6_type)
          if (Number.isInteger(v6Type)) {
            const key = String(v6Type)
            decodedV6Types[key] = (decodedV6Types[key] ?? 0) + 1
          }
          if (row.type === 'telemetry' || row.type === 'motion' || row.type === 'motion_ex' ||
              row.type === 'status' || row.type === 'damage' || row.type === 'lap') add(row.type, row)
          else if (row.type === 'race_event') raceEvents.push(row)
        } catch (e) {}
      }
      start = end + 1
      if (++rowsSinceYield >= 512 && start < coldJson.length) {
        rowsSinceYield = 0
        seekRetention.decodedStatusRows = incoming.status?.length ?? 0
        seekRetention.decodedDamageRows = incoming.damage?.length ?? 0
        seekRetention.decodedLapRows = incoming.lap?.length ?? 0
        await yieldToMainThread()
        if (cancelled()) return
      }
    }
  }
  seekRetention.decodedTelemetryRows = incoming.telemetry?.length ?? 0
  seekRetention.decodedMotionRows = incoming.motion?.length ?? 0
  seekRetention.decodedMotionExRows = incoming.motion_ex?.length ?? 0
  seekRetention.decodedStatusRows = incoming.status?.length ?? 0
  seekRetention.decodedDamageRows = incoming.damage?.length ?? 0
  seekRetention.decodedLapRows = incoming.lap?.length ?? 0
  if (cancelled()) return

  // An authoritative seek owns the decoded prefix; it keeps only rows streamed
  // after its endpoint. Additive window/AL responses are different: they may
  // finish after a newer authoritative flush and are allowed only to fill the
  // missing prefix (or, for V6 patches, the fields they carry). Replacing their
  // overlapping suffix used to discard the correct V6 ERS history and leave one
  // boundary seed plus the live tail.
  for (const family of Object.keys(TABLE_OF_FAMILY) as HistoryFamily[]) {
    const table = incoming[family]
    if (!table || table.length === 0) continue
    installHistory(TABLE_OF_FAMILY[family], table.frozen(),
      authoritative ? 'authoritative' : overlay.has(family) ? 'overlay' : 'prefix', MAX_ROWS)
  }
  if (!authoritative && Object.keys(decodedV6Types).length > 0) {
    // Sparse V6 page backfills fill fields into timestamps the chart bridges
    // have already consumed. Advance the revision so they rebuild those rows
    // instead of syncing only samples appended after the page change.
    analyzeLapRevisionVal++
  }
  if (authoritative) {
    authoritativeLapStatusStart = Number(payload.currentLapStart)
    const prefixStart = Math.max(0, stsTable.lowerBound(authoritativeLapStatusStart, true) - 1)
    authoritativeLapStatusPrefix = stsTable.frozen(prefixStart)
  }
  if (!isPlaybackFlag && (Number(payload.rowTypeMask) & HISTORY_ROW.raceEvent)) {
    const priorEvents = new Set(raceEventsAtDecodeStart)
    const streamedDuringDecode = raceEventsArr.filter(event => !priorEvents.has(event))
    raceEventsArr = mergeRaceEventHistory(raceEvents, streamedDuringDecode)
  }
  if (!isPlaybackFlag && allLapsMode && (Number(payload.rowTypeMask) & HISTORY_ROW.lap)) {
    allLapsLapBoundaries = reconstructLapBoundaries(lapProgressTable.frozen())
  }
  if (!isPlaybackFlag && !allLapsMode && (Number(payload.rowTypeMask) & HISTORY_ROW.lap)) {
    // The user may leave AL while its cooperative decode is in flight. Release
    // the late full-session payload immediately instead of retaining it until
    // the next lap transition.
    trimLiveWorkingSet()
  }
  currentStintStartTime = findCurrentStintStart(stsTable.frozen())
  if (allHistory) waitingForAllLapsHistory = false
  markHistoryCoverage(payload.rowTypeMask, payload.historyStart)

  const incomingLap = incoming.lap
  playbackDebug('seek-flush-decoded', {
    requestId: payload.requestId,
    authoritative,
    rowTypeMask: `0x${(Number(payload.rowTypeMask) >>> 0).toString(16)}`,
    historyStart: payload.historyStart,
    columnar: isV6HistoryPayload(binary),
    decodedV6Types,
    lapNum,
    lapStartTime,
    revision: analyzeLapRevisionVal,
    telemetryRows: incoming.telemetry?.length ?? 0,
    telemetryFirstTime: incoming.telemetry?.firstTime() ?? null,
    telemetryLastTime: incoming.telemetry?.lastTime() ?? null,
    motionRows: incoming.motion?.length ?? 0,
    motionExRows: incoming.motion_ex?.length ?? 0,
    statusRows: incoming.status?.length ?? 0,
    damageRows: incoming.damage?.length ?? 0,
    lapProgressRows: incomingLap?.length ?? 0,
    raceEventRows: raceEvents.length,
    lastLapNumber: incomingLap ? incomingLap.lastNum('lap_num') : null,
    lastLapTimeMs: incomingLap ? incomingLap.lastNum('current_lap_ms') : null,
    installedTelemetry: tableSpan(telTable),
    installedStatus: tableSpan(stsTable),
    // What the wing card will read once recompute() publishes `latest`.
    wingCard: (() => {
      const last = telTable.last() as Record<string, any> | null
      return {
        haveLastTelemetryRow: last !== null,
        sessionTime: last?.session_time ?? null,
        slm: last === null ? 'no-row' : last.slm === undefined ? 'MISSING' : last.slm,
        drs: last === null ? 'no-row' : last.drs === undefined ? 'MISSING' : last.drs,
      }
    })(),
  })
  const latestStatus = stsTable.last()
  const latestDamage = dmgTable.last()
  set({
    ...(latestStatus ? { status: latestStatus } : {}),
    ...(latestDamage ? { damage: latestDamage } : {}),
    ...(!isPlaybackFlag && allLapsMode ? { allLapsLapBoundaries } : {}),
    currentStintStartTime,
  })
  const latestLap = lapProgressTable.last()
  if (latestLap) { lapState = latestLap; set({ lap: latestLap }) }
  recompute(dirtySliceForHistoryMask(payload.rowTypeMask))
  requestVisibleWindowHistory()
  if (authoritative) setSeekPending(false)
  if (authoritative && Number.isFinite(Number(payload.requestId)) && Number(payload.requestId) > 0)
    window.playerBridge.seekInstalled(Number(payload.requestId))
  } finally {
    if (activeSeekDecodeRetention === seekRetention) activeSeekDecodeRetention = null
  }
}

// ── Public API ────────────────────────────────────────────────────────────────

// Set the visible time window (seconds). Infinity selects the full-session
// publication used by All Laps and Stint Laps. Recomputes slices at once.
function requestVisibleWindowHistory(): void {
  if (speedRpmBlocksVal === null) return
  const fileStart = Math.min(...speedRpmBlocksVal.map(block => Number(block.startSessionTime)).filter(Number.isFinite))
  const currentTime = Math.max(
    ...Object.values(TABLE_OF_FAMILY).map(table => table.lastTime() ?? 0),
  )
  if (!Number.isFinite(fileStart) || currentTime <= fileStart) return
  const firstTimes = new Map<number, number | undefined>([
    [HISTORY_ROW.telemetry, telTable.firstTime()],
    [HISTORY_ROW.status, stsTable.firstTime()],
    [HISTORY_ROW.damage, dmgTable.firstTime()],
    [HISTORY_ROW.motion, motTable.firstTime()],
    [HISTORY_ROW.motionEx, motExTable.firstTime()],
    [HISTORY_ROW.lap, lapProgressTable.firstTime()],
  ])
  const requestRange = (requestedMask: number, requiredStart: number, windowSeconds: number): void => {
    let missingMask = 0
    const families: Array<Record<string, unknown>> = []
    for (const bit of HISTORY_ROW_BITS) {
      if (!(requestedMask & bit)) continue
      const pendingV6Type = Boolean(pendingV6HistoryBackfillMask & bit)
      const covered = historyCovers(bit, requiredStart)
      const firstTime = firstTimes.get(bit)
      const missingPrefix = !covered && (firstTime ?? Infinity) > requiredStart + 1
      families.push({ bit: `0x${bit.toString(16)}`, pendingV6Type, covered, firstTime: firstTime ?? null, missingPrefix })
      if (pendingV6Type || missingPrefix)
        missingMask |= bit
    }
    playbackDebug('history-backfill-evaluation', {
      requestedMask: `0x${(requestedMask >>> 0).toString(16)}`,
      missingMask: `0x${(missingMask >>> 0).toString(16)}`,
      requiredStart,
      windowSeconds,
      currentTime,
      seekRendererPending,
      pendingV6HistoryBackfillMask: `0x${pendingV6HistoryBackfillMask.toString(16)}`,
      historyV6Types: [...historyV6Types].sort((a, b) => a - b),
      families,
    })
    if (missingMask !== 0) {
      playbackDebug('history-backfill-requested', {
        missingMask: `0x${(missingMask >>> 0).toString(16)}`,
        requiredStart,
        windowSeconds,
      })
      window.playerBridge.getWindowData(windowSeconds, missingMask)
      pendingV6HistoryBackfillMask &= ~missingMask
    }
  }

  const scopedFiniteMask = secondaryFiniteHistoryRowMask & ~fullSessionHistoryRowMask
  const scopedLapMask = secondaryLapHistoryRowMask & ~fullSessionHistoryRowMask
  if (!allLapsMode && scopedFiniteMask === 0 && scopedLapMask === 0) {
    if (finiteWindowBackfillEnabled) {
      if (!Number.isFinite(secondsVal) || secondsVal <= 0) return
      requestRange(historyRowMask, Math.max(fileStart, currentTime - secondsVal), secondsVal)
    } else {
      requestRange(historyRowMask, Math.max(fileStart, lapStartTime), 0)
    }
    return
  }

  const finiteStart = secondaryHistoryWindowSeconds > 0
    ? Math.max(fileStart, currentTime - secondaryHistoryWindowSeconds)
    : currentTime
  const lapStart = Math.max(fileStart, lapStartTime)
  // When one family is used by both kinds of chart, request the wider range
  // exactly once rather than racing two overlapping native backfills.
  const overlap = scopedFiniteMask & scopedLapMask
  const finiteRequestMask = (scopedFiniteMask & ~overlap) |
    (finiteStart <= lapStart ? overlap : 0)
  const lapRequestMask = (scopedLapMask & ~overlap) |
    (lapStart < finiteStart ? overlap : 0)
  if (finiteRequestMask && secondaryHistoryWindowSeconds > 0)
    requestRange(finiteRequestMask, finiteStart, secondaryHistoryWindowSeconds)
  if (lapRequestMask) requestRange(lapRequestMask, lapStart, 0)
}

export function setTelemetrySeconds(s: number, backfillFiniteWindow = true): void {
  const nextAllLapsMode = !Number.isFinite(s)
  const sameConfiguration = s === secondsVal && nextAllLapsMode === allLapsMode &&
    finiteWindowBackfillEnabled === backfillFiniteWindow
  finiteWindowBackfillEnabled = backfillFiniteWindow
  if (sameConfiguration) {
    requestVisibleWindowHistory()
    return
  }
  const wasAllLapsMode = allLapsMode
  secondsVal = s
  allLapsMode = nextAllLapsMode
  if (!allLapsMode) {
    waitingForAllLapsHistory = false
    if (allLapsLapBoundaries.length > 0) {
      allLapsLapBoundaries = []
      set({ allLapsLapBoundaries: [] })
    }
    // Normal playback can evict the old prefix from bounded renderer buffers.
    // A later AL entry must therefore be allowed to request it again.
    requestedHistoryRowMask = 0
    if (wasAllLapsMode) {
      trimLiveWorkingSet()
      trimPlaybackWorkingSet()
    }
  }
  set({ seconds: s })
  let requestHistory = false
  let entryMissingMask = 0
  if (allLapsMode && !wasAllLapsMode && speedRpmBlocksVal !== null) {
    const firstBlockStart = Math.min(...speedRpmBlocksVal.map(block => Number(block.startSessionTime)).filter(Number.isFinite))
    if (Number.isFinite(firstBlockStart)) {
      const missing = (bit: number, firstTime: number | undefined): void => {
        if ((fullSessionHistoryRowMask & bit) && !historyCovers(bit, firstBlockStart) &&
            (firstTime ?? Infinity) > firstBlockStart + 1) entryMissingMask |= bit
      }
      missing(HISTORY_ROW.telemetry, telTable.firstTime())
      missing(HISTORY_ROW.status, stsTable.firstTime())
      missing(HISTORY_ROW.damage, dmgTable.firstTime())
      missing(HISTORY_ROW.motion, motTable.firstTime())
      missing(HISTORY_ROW.motionEx, motExTable.firstTime())
      missing(HISTORY_ROW.lap, lapProgressTable.firstTime())
    }
    requestedHistoryRowMask |= fullSessionHistoryRowMask & ~entryMissingMask
    if (entryMissingMask !== 0) {
      waitingForAllLapsHistory = true
      requestHistory = true
    }
  }
  recompute(DirtySlice.All)
  if (requestHistory) {
    requestedHistoryRowMask |= entryMissingMask
    window.playerBridge.getAllLapsData(entryMissingMask)
  }
  requestVisibleWindowHistory()
}

// Current-lap distance slices are intentionally separate from the append-only
// All Laps sources. Building them copies every row accumulated in the current
// lap, so doing that on every playback batch is quadratic when the visible page
// contains only time-axis charts. AppShell enables this only when at least one
// lap-distance view actually consumes the slices.
export function setAnalyzeLapEnabled(enabled: boolean): void {
  if (enabled === analyzeLapEnabled) return
  analyzeLapEnabled = enabled
  if (enabled) {
    recompute(DirtySlice.All)
    return
  }
  if (speedRpmBlocksVal !== null) {
    set({
      ...EMPTY_ANALYZE_SLICES,
    })
  }
}

// Sets the single coordinated minimum history requirement. While AL is active,
// newly-visible row families are requested additively; already-loaded families
// are not decompressed or delivered again.
export function setHistoryRowMask(
  mask: number,
  fullSessionMask = mask,
  finiteWindowMask = 0,
  finiteWindowSeconds = 0,
  lapWindowMask = 0,
  v6HistoryTypes: readonly number[] = [],
): void {
  const normalized = mask >>> 0
  const normalizedFullSession = (fullSessionMask & normalized) >>> 0
  const disabled = historyRowMask & ~normalized
  historyRowMask = normalized
  fullSessionHistoryRowMask = normalizedFullSession
  secondaryFiniteHistoryRowMask = (finiteWindowMask & normalized & ~normalizedFullSession) >>> 0
  secondaryLapHistoryRowMask = (lapWindowMask & normalized & ~normalizedFullSession) >>> 0
  secondaryHistoryWindowSeconds = Number.isFinite(finiteWindowSeconds)
    ? Math.max(0, finiteWindowSeconds)
    : 0
  const nextV6Types = new Set(v6HistoryTypes)
  playbackDebug('history-requirements-changing', {
    previousMask: `0x${historyRowMask.toString(16)}`,
    nextMask: `0x${normalized.toString(16)}`,
    disabledMask: `0x${disabled.toString(16)}`,
    fullSessionMask: `0x${normalizedFullSession.toString(16)}`,
    finiteWindowMask: `0x${(finiteWindowMask >>> 0).toString(16)}`,
    finiteWindowSeconds,
    lapWindowMask: `0x${(lapWindowMask >>> 0).toString(16)}`,
    previousV6Types: [...historyV6Types].sort((a, b) => a - b),
    nextV6Types: [...nextV6Types].sort((a, b) => a - b),
    seekRendererPending,
  })
  for (const type of nextV6Types)
    if (!historyV6Types.has(type)) pendingV6HistoryBackfillMask |= historyRowMaskForV6Type(type)
  historyV6Types.clear()
  for (const type of nextV6Types) historyV6Types.add(type)
  requestedHistoryRowMask &= normalizedFullSession
  // Dropping a hidden tab's source buffer also drops the history represented
  // by that buffer. Keeping its old coverage marker made a later tab activation
  // believe the missing prefix was still installed, so only rows streamed after
  // the switch appeared. Invalidate exactly the disabled families: the normal
  // finite/current-lap or AL request below can then issue one selective native
  // indexed backfill, without replaying unrelated row types through the DOM.
  invalidateHistoryCoverage(disabled)
  const cleared: Partial<TelemetryStoreState> = {}
  if (disabled & HISTORY_ROW.telemetry) {
    telTable.keepLastOnly()
    cleared.telemetry = EMPTY.telemetry
    cleared.analyzeLapTelemetry = EMPTY.telemetry
  }
  if (disabled & HISTORY_ROW.status) {
    stsTable.keepLastOnly()
    cleared.statusHistory = EMPTY.status
    cleared.analyzeLapStatusHistory = EMPTY.status
  }
  if (disabled & HISTORY_ROW.damage) {
    dmgTable.keepLastOnly()
    cleared.damageHistory = EMPTY.damage
    cleared.analyzeLapDamageHistory = EMPTY.damage
  }
  if (disabled & HISTORY_ROW.motion) {
    motTable.clear()
    cleared.motion = EMPTY.motion
    cleared.analyzeLapMotion = EMPTY.motion
  }
  if (disabled & HISTORY_ROW.motionEx) {
    motExTable.clear()
    cleared.motionEx = EMPTY.motionEx
    cleared.analyzeLapMotionEx = EMPTY.motionEx
  }
  if (disabled & HISTORY_ROW.lap) {
    allLapsLapBoundaries = []
    cleared.allLapsLapBoundaries = []
  }
  if (Object.keys(cleared).length) set(cleared)
  if (allLapsMode && speedRpmBlocksVal !== null) {
    const missing = (normalizedFullSession & ~requestedHistoryRowMask) |
      (pendingV6HistoryBackfillMask & normalizedFullSession)
    if (missing !== 0) {
      waitingForAllLapsHistory = true
      requestedHistoryRowMask = (requestedHistoryRowMask | missing) >>> 0
      window.playerBridge.getAllLapsData(missing)
      pendingV6HistoryBackfillMask &= ~missing
    }
  }
  requestVisibleWindowHistory()
}

// Subscribe to live race events (transient banners). Delivered synchronously as
// they stream in, so none are lost to React batching when several arrive at once.
export function subscribeRaceEvent(cb: (e: RaceEventMsg) => void): () => void {
  raceEventListeners.add(cb)
  return () => { raceEventListeners.delete(cb) }
}

// Full-lap charts consume the in-place full-session arrays directly. This
// imperative signal lets their WebGL bridges append new rows without cloning
// the growing arrays through Zustand/React on every telemetry batch.
export function subscribeAllLapsData(sourceMask: number, cb: () => void): () => void {
  allLapsDataListeners.set(cb, sourceMask >>> 0)
  return () => { allLapsDataListeners.delete(cb) }
}

// Start the IPC subscription exactly once, at module load. The preload bridge is
// present before the bundle runs, and the store lives for the app's lifetime, so
// there is nothing to tear down.
let started = false
export function startTelemetryBridge(): void {
  if (started) return
  started = true
  const initialDebugSettings = getDebugSettings()
  configureRendererDiagnostics(initialDebugSettings.additionalLogging)
  configureMemoryLog(initialDebugSettings.memoryLog)
  subscribeDebugSettings(settings => {
    configureRendererDiagnostics(settings.additionalLogging)
    configureMemoryLog(settings.memoryLog)
  })

  window.playerBridge.onSeekStart((allHistory) => {
    // Freeze the published timeline immediately, before the seek IPC can race
    // with already-queued playback batches. Electron main holds all rows from
    // the new cursor until processPlaybackSeekFlush acknowledges installation.
    setSeekPending(true)
    set({ strategyRebuilding: true })
    playbackDebug('seek-started-in-renderer', {
      allHistory,
      historyRowMask: `0x${historyRowMask.toString(16)}`,
      fullSessionHistoryRowMask: `0x${fullSessionHistoryRowMask.toString(16)}`,
      historyV6Types: [...historyV6Types].sort((a, b) => a - b),
      pendingV6HistoryBackfillMask: `0x${pendingV6HistoryBackfillMask.toString(16)}`,
    })
    if (!allHistory) return
    // A driver switch is followed by exactly this seek; keep the outgoing
    // driver's complete history so switching back to them is immediate.
    allLapsSnapshot = allLapsMode && !waitingForAllLapsHistory && !tablesFromDriverCache && telTable.length > 0
      ? { driver: currentPlaybackDriver, views: snapshotTables() }
      : null
    // Keep the currently published views intact while the worker extracts the
    // new prefix. Fresh post-seek rows accumulate separately and are merged by
    // the authoritative response.
    waitingForAllLapsHistory = true
    tablesFromDriverCache = false
    for (const table of Object.values(TABLE_OF_FAMILY)) table.clear()
  })

  window.telemetryBridge.onBatch((batchStr: string) => {
    if (additionalLoggingEnabled) {
      rendererDiagnostics.jsonBatches++
      rendererDiagnostics.jsonChars += batchStr.length
    }
    // A batch that carries one of the admitted rows may still be batched
    // together with old-cursor rows, so filter per row rather than admitting
    // the whole batch.
    let seekFiltered = false
    if (seekRendererPending) {
      if (!SEEK_PENDING_ROW_MARKERS.some(marker => batchStr.includes(marker))) return
      seekFiltered = true
    }
    let dirty = DirtySlice.None
    let start = 0
    while (start < batchStr.length) {
      let end = batchStr.indexOf('\n', start)
      if (end === -1) end = batchStr.length
      if (end > start) {
        try {
          const msg = JSON.parse(batchStr.slice(start, end)) as GatewayMsg
          if (additionalLoggingEnabled) {
            rendererDiagnostics.jsonRows++
            observeRendererRow(msg, 'telemetry-batch')
          }
          if (!seekFiltered || SEEK_PENDING_ROW_TYPES.has(msg.type)) {
            dirty |= dirtySliceFor(msg)
            handleMsg(msg)
          }
        }
        catch (e) {
          if (additionalLoggingEnabled) {
            rendererDiagnostics.jsonParseErrors++
            console.error('[telemetry-diagnostics][renderer] Failed to parse batch JSON:', {
              error: String(e),
              batchChars: batchStr.length,
              rowOffset: start,
              rowChars: end - start,
              rowPreview: batchStr.slice(start, Math.min(end, start + 300)),
            })
          } else {
            console.error('Failed to parse batch JSON:', e)
          }
        }
      }
      start = end + 1
    }
    recompute(dirty)
    if (additionalLoggingEnabled && dirty !== DirtySlice.None) rendererDiagnostics.recomputes++
  })

  window.telemetryBridge.on((raw) => {
    const msg = raw as GatewayMsg
    if (additionalLoggingEnabled) {
      rendererDiagnostics.singleRows++
      observeRendererRow(msg, 'telemetry')
    }
    handleMsg(msg)
    recompute(dirtySliceFor(msg))
    if (additionalLoggingEnabled) rendererDiagnostics.recomputes++
  })

  window.telemetryBridge.onBinary((batch) => {
    if (additionalLoggingEnabled) {
      rendererDiagnostics.binaryBatches++
      rendererDiagnostics.binaryBytes += batch.byteLength
    }
    if (seekRendererPending) return
    let dirty = DirtySlice.None
    try {
      forEachDecodedBinaryRow(batch, row => {
        if (additionalLoggingEnabled) rendererDiagnostics.binaryRows++
        const msg = row as GatewayMsg
        if (additionalLoggingEnabled) observeRendererRow(msg, 'telemetry-binary')
        dirty |= dirtySliceFor(msg)
        handleMsg(msg)
      })
    } catch (e) {
      if (additionalLoggingEnabled) {
        rendererDiagnostics.binaryDecodeErrors++
        console.error('[telemetry-diagnostics][renderer] Failed to decode binary batch:', {
          error: String(e),
          bytes: batch.byteLength,
          firstBytesHex: binaryPreview(batch),
        })
      } else {
        console.error('Failed to decode binary batch:', e)
      }
    }
    recompute(dirty)
    if (additionalLoggingEnabled && dirty !== DirtySlice.None) rendererDiagnostics.recomputes++
  })

  window.telemetryBridge.onResume(({ binary, coldJson }) => {
    if (additionalLoggingEnabled) {
      rendererDiagnostics.resumePayloads++
      rendererDiagnostics.resumeBinaryBytes += binary.byteLength
      rendererDiagnostics.resumeJsonChars += coldJson.length
      console.info(`[telemetry-diagnostics][renderer] applying resume payload: ${JSON.stringify({ binaryBytes: binary.byteLength, coldJsonChars: coldJson.length })}`)
    }
    if (seekRendererPending) return
    let dirty = DirtySlice.None
    try {
      forEachDecodedBinaryRow(binary, row => {
        if (additionalLoggingEnabled) rendererDiagnostics.binaryRows++
        const msg = row as GatewayMsg
        if (additionalLoggingEnabled) observeRendererRow(msg, 'telemetry-resume-binary')
        dirty |= dirtySliceFor(msg)
        handleMsg(msg)
      })
    } catch (e) {
      if (additionalLoggingEnabled) {
        rendererDiagnostics.binaryDecodeErrors++
        console.error('[telemetry-diagnostics][renderer] Failed to decode resume binary batch:', {
          error: String(e),
          bytes: binary.byteLength,
          firstBytesHex: binaryPreview(binary),
        })
      } else {
        console.error('Failed to decode resume binary batch:', e)
      }
    }

    let latestStatus: StatusRow | null = null
    let latestDamage: DamageRow | null = null
    let start = 0
    while (start < coldJson.length) {
      let end = coldJson.indexOf('\n', start)
      if (end === -1) end = coldJson.length
      if (end > start) {
        try {
          const msg = JSON.parse(coldJson.slice(start, end)) as GatewayMsg
          if (additionalLoggingEnabled) {
            rendererDiagnostics.jsonRows++
            observeRendererRow(msg, 'telemetry-resume-json')
          }
          // Sparse V6 playback delivers the hot families as JSON patches rather
          // than packed binary. They append exactly like their binary
          // counterparts above and publish through recompute(), not per-row
          // set(), so routing them through handleMsg costs no extra renders.
          if (msg.type === 'telemetry' || msg.type === 'motion' || msg.type === 'motion_ex') {
            dirty |= dirtySliceFor(msg)
            handleMsg(msg)
          } else if (msg.type === 'status') {
            appendPlaybackPatch(stsTable, msg, MAX_ROWS)
            latestStatus = stsTable.last()!
            if (!isPlaybackFlag && Number.isFinite(latestStatus.fuel_kg) && latestStatus.fuel_kg >= 0 && latestStatus.fuel_kg > fuelMaxReceived) {
              fuelMaxReceived = latestStatus.fuel_kg
            }
            dirty |= DirtySlice.Status | DirtySlice.Derived
          } else if (msg.type === 'damage') {
            appendPlaybackPatch(dmgTable, msg, MAX_ROWS)
            latestDamage = dmgTable.last()
            dirty |= DirtySlice.Damage
          }
        }
        catch (e) {
          if (additionalLoggingEnabled) {
            rendererDiagnostics.jsonParseErrors++
            console.error('[telemetry-diagnostics][renderer] Failed to parse resume JSON:', {
              error: String(e),
              payloadChars: coldJson.length,
              rowOffset: start,
              rowChars: end - start,
              rowPreview: coldJson.slice(start, Math.min(end, start + 300)),
            })
          } else {
            console.error('Failed to parse resume JSON:', e)
          }
        }
      }
      start = end + 1
    }

    // Publish cold current-state values once after the bulk history append;
    // per-row Zustand writes here would turn a long resume window into a render
    // storm. Each source buffer is independently chronological, so the hot and
    // cold channels do not need a combined O(n log n) sort.
    const current: Partial<TelemetryStoreState> = {}
    if (latestStatus) current.status = latestStatus
    if (latestDamage) current.damage = latestDamage
    if (!isPlaybackFlag && fuelMaxReceived > -Infinity) current.fuelUpperLimit = fuelMaxReceived + 1
    if (Object.keys(current).length > 0) {
      set(current)
    }
    recompute(dirty)
    if (additionalLoggingEnabled && dirty !== DirtySlice.None) rendererDiagnostics.recomputes++
  })

}

startTelemetryBridge()
