import type { AnalyzeLapData, DamageRow, LapRow, MotionExRow, MotionRow, PlayerPositionPoint, StatusRow, TelemetryRow } from '../types'
import { DATA_ROW } from './historyDependencies'
import { viewOfRows } from './columnStore'

// Engine JSON from analysis:get-lap-data / playback_lap_data; checked before use.
interface LapDataPayload {
  type?: string
  lapNum: number
  startSessionTime: number
  endSessionTime: number
  telemetry?: TelemetryRow[]
  motionHistory?: MotionRow[]
  motionExHistory?: MotionExRow[]
  statusHistory?: StatusRow[]
  damageHistory?: DamageRow[]
  lapProgress?: LapRow[]
  playerPositions?: PlayerPositionPoint[]
  rowTypeMask: number
}

export function parseAnalyzeLapData(raw: unknown): AnalyzeLapData | null {
  const payload = raw as LapDataPayload | null | undefined
  if (!payload || payload.type !== 'playback_lap_data' || !Number.isFinite(payload.lapNum)) return null
  return {
    lapNum: payload.lapNum,
    startSessionTime: payload.startSessionTime,
    endSessionTime: payload.endSessionTime,
    telemetry: viewOfRows('telemetry', payload.telemetry ?? []),
    motion: viewOfRows('motion', payload.motionHistory ?? []),
    motionEx: viewOfRows('motion_ex', payload.motionExHistory ?? []),
    statusHistory: viewOfRows('status', payload.statusHistory ?? []),
    damageHistory: viewOfRows('damage', payload.damageHistory ?? []),
    lapProgress: viewOfRows('lap', payload.lapProgress ?? [], false),
    playerPositions: payload.playerPositions ?? [],
    rowTypeMask: Number.isFinite(payload.rowTypeMask) ? payload.rowTypeMask >>> 0 : 0xFFFFFFFF,
  }
}


export function mergeAnalyzeLapData(
  prior: AnalyzeLapData,
  incoming: AnalyzeLapData,
): AnalyzeLapData {
  const mask = incoming.rowTypeMask ?? 0xFFFFFFFF
  return {
    lapNum: incoming.lapNum,
    startSessionTime: incoming.startSessionTime,
    endSessionTime: incoming.endSessionTime,
    telemetry: mask & DATA_ROW.telemetry ? incoming.telemetry : prior.telemetry,
    motion: mask & DATA_ROW.motion ? incoming.motion : prior.motion,
    motionEx: mask & DATA_ROW.motionEx ? incoming.motionEx : prior.motionEx,
    statusHistory: mask & DATA_ROW.status ? incoming.statusHistory : prior.statusHistory,
    damageHistory: mask & DATA_ROW.damage ? incoming.damageHistory : prior.damageHistory,
    lapProgress: mask & DATA_ROW.lap ? incoming.lapProgress : prior.lapProgress,
    playerPositions: mask & DATA_ROW.positions ? incoming.playerPositions : prior.playerPositions,
    rowTypeMask: ((prior.rowTypeMask ?? 0) | mask) >>> 0,
  }
}
