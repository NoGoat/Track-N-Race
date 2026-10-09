import type { AnalyzeLapData } from '../types'
import { DATA_ROW } from './historyDependencies'

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
