import type { CoreLayout, InputLayout, MiscLayout, PowerLayout, Tab, TyresLayout } from '../app/appConfig'
import { ANALYZE_METRIC_BY_ID, type AnalyzeSeriesConfig } from './analyzeMetrics'
import type { GraphSection } from './graphSections'

// Logical recording row families. These bits are shared with TnrdReader's V4
// directory and are deliberately not UDP packet ids: one game packet can feed
// more than one consumer family (Motion, for example, also produces Positions).
export const DATA_ROW = {
  telemetry: 1 << 1,
  status: 1 << 2,
  damage: 1 << 3,
  lap: 1 << 4,
  session: 1 << 5,
  raceEvent: 1 << 6,
  timing: 1 << 7,
  participants: 1 << 8,
  allStatus: 1 << 9,
  tyreSets: 1 << 10,
  motion: 1 << 11,
  motionEx: 1 << 12,
  positions: 1 << 13,
  sessionHistoryFastest: 1 << 14,
  strategy: 1 << 15,
} as const

// Kept as an alias for the finite/AL backfill code while the broader registry
// also coordinates current-state and streaming consumers.
export const HISTORY_ROW = DATA_ROW

export interface DataRequirements {
  // Rows that must continue flowing for current cards, tables, maps, or charts.
  streamMask: number
  // Families that require a time range rather than only their latest value.
  historyMask: number
}

type Requirement = Readonly<{ stream: number; history?: number }>

// Every UI consumer is tagged here. Components do not independently talk to
// IPC; the coordinator below unions the tags for the active page/layout and
// sends one stable subscription generation through the bridge.
export const DATA_CONSUMERS = {
  globalClock: { stream: DATA_ROW.telemetry | DATA_ROW.lap | DATA_ROW.sessionHistoryFastest },
  globalBanners: { stream: DATA_ROW.session | DATA_ROW.participants | DATA_ROW.raceEvent },
  liveLeaderWatcher: { stream: DATA_ROW.timing },

  overviewStats: { stream: DATA_ROW.telemetry | DATA_ROW.status | DATA_ROW.lap | DATA_ROW.damage },
  overviewTelemetry: { stream: DATA_ROW.telemetry | DATA_ROW.status, history: DATA_ROW.telemetry | DATA_ROW.status },
  tyreTemperatureHistory: { stream: DATA_ROW.telemetry, history: DATA_ROW.telemetry },
  tyreWearHistory: { stream: DATA_ROW.damage, history: DATA_ROW.damage },
  damageCards: { stream: DATA_ROW.damage },

  timingTower: { stream: DATA_ROW.timing | DATA_ROW.participants | DATA_ROW.allStatus | DATA_ROW.lap | DATA_ROW.status },
  sessionPage: { stream: DATA_ROW.session | DATA_ROW.raceEvent | DATA_ROW.timing | DATA_ROW.participants | DATA_ROW.positions },
  inputHistory: { stream: DATA_ROW.telemetry, history: DATA_ROW.telemetry },
  powerCards: { stream: DATA_ROW.status },
  powerHistory: { stream: DATA_ROW.status, history: DATA_ROW.status },
  tyrePageState: { stream: DATA_ROW.tyreSets | DATA_ROW.telemetry | DATA_ROW.damage | DATA_ROW.session },
  strategyPage: { stream: DATA_ROW.strategy },
  gForceHistory: { stream: DATA_ROW.motion, history: DATA_ROW.motion },
  rideHeightHistory: { stream: DATA_ROW.motionEx, history: DATA_ROW.motionEx },
  analyzeLapCoordinates: { stream: DATA_ROW.lap, history: DATA_ROW.lap },
  analyzeMap: { stream: DATA_ROW.positions | DATA_ROW.lap, history: DATA_ROW.positions | DATA_ROW.lap },
} as const satisfies Record<string, Requirement>

function add(target: DataRequirements, requirement: Requirement): void {
  target.streamMask |= requirement.stream
  target.historyMask |= requirement.history ?? 0
}

function any(values: object): boolean {
  return Object.values(values).some(Boolean)
}

export function dataRequirementsForUi(
  tab: Tab,
  core: CoreLayout,
  input: InputLayout,
  misc: MiscLayout,
  power: PowerLayout,
  tyres: TyresLayout,
  tyreView: 'cards' | 'graphs',
  isPlayback: boolean,
  analyzeMask = 0,
): DataRequirements {
  const result: DataRequirements = { streamMask: 0, historyMask: 0 }
  add(result, DATA_CONSUMERS.globalClock)
  add(result, DATA_CONSUMERS.globalBanners)
  if (!isPlayback) add(result, DATA_CONSUMERS.liveLeaderWatcher)

  if (tab === 'core') {
    if (core.showStats && any(core.statsCards)) add(result, DATA_CONSUMERS.overviewStats)
    if (core.showSpeedChart) add(result, DATA_CONSUMERS.overviewTelemetry)
    if (core.showThermal) {
      if (tyreView === 'graphs') {
        const { surfaceTemp, innerTemp, brakeTemp, tyreLife } = core.thermalGraphs
        if (surfaceTemp || innerTemp || brakeTemp) add(result, DATA_CONSUMERS.tyreTemperatureHistory)
        if (tyreLife) add(result, DATA_CONSUMERS.tyreWearHistory)
      } else if (any(core.thermalCards)) {
        // Wheel-card sparklines consume the same bounded temperature history.
        add(result, DATA_CONSUMERS.tyreTemperatureHistory)
        add(result, DATA_CONSUMERS.damageCards)
      }
    }
    if (any(core.damageItems)) add(result, DATA_CONSUMERS.damageCards)
  } else if (tab === 'timing_tower') {
    add(result, DATA_CONSUMERS.timingTower)
  } else if (tab === 'session') {
    add(result, DATA_CONSUMERS.sessionPage)
  } else if (tab === 'input') {
    if (input.showGear || input.showInputs || input.showSteering) add(result, DATA_CONSUMERS.inputHistory)
  } else if (tab === 'power') {
    if (any(power.statsCards)) add(result, DATA_CONSUMERS.powerCards)
    if (any(power.charts)) add(result, DATA_CONSUMERS.powerHistory)
  } else if (tab === 'tyres') {
    add(result, DATA_CONSUMERS.tyrePageState)
    if (tyres.charts.surfaceTemp || tyres.charts.innerTemp ||
        tyres.charts.brakeTemp)
      add(result, DATA_CONSUMERS.tyreTemperatureHistory)
    if (tyres.charts.tyreLife) add(result, DATA_CONSUMERS.tyreWearHistory)
  } else if (tab === 'strategy') {
    add(result, DATA_CONSUMERS.strategyPage)
  } else if (tab === 'misc') {
    if (misc.showGForce) add(result, DATA_CONSUMERS.gForceHistory)
    if (misc.showRideHeight) add(result, DATA_CONSUMERS.rideHeightHistory)
  } else if (tab === 'analyze') {
    result.streamMask |= analyzeMask
    result.historyMask |= analyzeMask
  }

  result.historyMask |= DATA_ROW.lap
  result.streamMask |= result.historyMask
  result.streamMask >>>= 0
  result.historyMask >>>= 0
  return result
}

export function visibleChartSectionsForUi(
  tab: Tab,
  core: CoreLayout,
  input: InputLayout,
  misc: MiscLayout,
  power: PowerLayout,
  tyres: TyresLayout,
  tyreView: 'cards' | 'graphs',
): GraphSection[] {
  if (tab === 'core') {
    const sections: GraphSection[] = []
    if (core.showSpeedChart) sections.push('overviewTelemetry')
    if (core.showThermal && tyreView === 'graphs') {
      if (core.thermalGraphs.surfaceTemp) sections.push('overviewTyreSurface')
      if (core.thermalGraphs.innerTemp) sections.push('overviewTyreInner')
      if (core.thermalGraphs.brakeTemp) sections.push('overviewTyreBrake')
      if (core.thermalGraphs.tyreLife) sections.push('overviewTyreWear')
    }
    return sections
  }
  if (tab === 'input') {
    return [
      ...(input.showGear ? ['inputGear' as const] : []),
      ...(input.showInputs ? ['inputThrottleBrake' as const] : []),
      ...(input.showSteering ? ['inputSteering' as const] : []),
    ]
  }
  if (tab === 'power') {
    return [
      ...(power.charts.powerSplit ? ['powerSplit' as const] : []),
      ...(power.charts.ersHarvest ? ['powerHarvest' as const] : []),
      ...(power.charts.ersStore ? ['powerStore' as const] : []),
      ...(power.charts.fuelHistory ? ['powerFuel' as const] : []),
    ]
  }
  if (tab === 'tyres') {
    return [
      ...(tyres.charts.surfaceTemp ? ['tyreSurface' as const] : []),
      ...(tyres.charts.innerTemp ? ['tyreInner' as const] : []),
      ...(tyres.charts.brakeTemp ? ['tyreBrake' as const] : []),
      ...(tyres.charts.tyreLife ? ['tyreWear' as const] : []),
    ]
  }
  if (tab === 'misc') {
    return [
      ...(misc.showGForce ? ['miscGForce' as const] : []),
      ...(misc.showRideHeight ? ['miscRideHeight' as const] : []),
    ]
  }
  return []
}

export function dataMaskForAnalyze(
  view: 'graph' | 'map',
  series: readonly AnalyzeSeriesConfig[],
): number {
  if (view === 'map') return DATA_CONSUMERS.analyzeMap.history
  let mask = DATA_CONSUMERS.analyzeLapCoordinates.history
  for (const item of series) {
    if (!item.visible || item.metricId === 'delta') continue
    const source = ANALYZE_METRIC_BY_ID.get(item.metricId)?.source
    if (source) mask |= DATA_ROW[source]
  }
  return mask >>> 0
}

// Compatibility helper used by existing callers/tests while the coordinator
// consumes the richer stream/history pair.
export function historyMaskForLayouts(
  tab: Tab,
  core: CoreLayout,
  input: InputLayout,
  misc: MiscLayout,
  power: PowerLayout,
  tyres: TyresLayout,
  tyreView: 'cards' | 'graphs',
): number {
  return dataRequirementsForUi(tab, core, input, misc, power, tyres, tyreView, false).historyMask
}
