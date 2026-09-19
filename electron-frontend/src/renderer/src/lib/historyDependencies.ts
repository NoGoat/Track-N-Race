import type { CoreLayout, InputLayout, MiscLayout, PageLayouts, PowerLayout, Tab, TyresLayout } from '../app/appConfig'
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

export const V6_DATA = {
  speed: 1, rpm: 2, gear: 3, throttle: 4, brake: 5, steering: 6, aero: 7,
  tyreSurfaceTemp: 8, tyreInnerTemp: 9, brakeTemp: 10, engineTemp: 11,
  tyreWear: 12, tyreState: 13, damage: 14, fuel: 15, ersStore: 16,
  ersHarvest: 17, ersDeployment: 18, enginePower: 19, brakeBias: 20,
  gForce: 21, rideHeight: 22, position: 23, lapTiming: 24,
} as const

export interface DataRequirements {
  // Rows that must continue flowing for current cards, tables, maps, or charts.
  streamMask: number
  // Families that require a time range rather than only their latest value.
  historyMask: number
  v6Types: number[]
  v6HistoryTypes: number[]
}

type Requirement = Readonly<{ stream: number; history?: number; types?: readonly number[] }>

// Every UI consumer is tagged here. Components do not independently talk to
// IPC; the coordinator below unions the tags for the active page/layout and
// sends one stable subscription generation through the bridge.
export const DATA_CONSUMERS = {
  globalClock: { stream: DATA_ROW.telemetry | DATA_ROW.lap | DATA_ROW.sessionHistoryFastest, types: [V6_DATA.speed, V6_DATA.lapTiming] },
  globalBanners: { stream: DATA_ROW.session | DATA_ROW.participants | DATA_ROW.raceEvent },
  liveLeaderWatcher: { stream: DATA_ROW.timing, types: [V6_DATA.lapTiming] },

  overviewStats: { stream: DATA_ROW.telemetry | DATA_ROW.status | DATA_ROW.lap | DATA_ROW.damage, types: Object.values(V6_DATA) },
  overviewTelemetry: { stream: DATA_ROW.telemetry | DATA_ROW.status, history: DATA_ROW.telemetry | DATA_ROW.status, types: [V6_DATA.speed, V6_DATA.rpm, V6_DATA.ersStore] },
  tyreTemperatureHistory: { stream: DATA_ROW.telemetry, history: DATA_ROW.telemetry, types: [V6_DATA.tyreSurfaceTemp, V6_DATA.tyreInnerTemp, V6_DATA.brakeTemp] },
  tyreWearHistory: { stream: DATA_ROW.damage, history: DATA_ROW.damage, types: [V6_DATA.tyreWear] },
  damageCards: { stream: DATA_ROW.damage, types: [V6_DATA.damage] },

  timingTower: {
    stream: DATA_ROW.timing | DATA_ROW.participants | DATA_ROW.allStatus | DATA_ROW.lap | DATA_ROW.status,
    types: [V6_DATA.lapTiming, V6_DATA.tyreState, V6_DATA.aero, V6_DATA.fuel,
      V6_DATA.ersStore, V6_DATA.ersHarvest, V6_DATA.ersDeployment, V6_DATA.brakeBias],
  },
  sessionPage: { stream: DATA_ROW.session | DATA_ROW.raceEvent | DATA_ROW.timing | DATA_ROW.participants | DATA_ROW.positions, types: [V6_DATA.lapTiming, V6_DATA.position] },
  inputHistory: { stream: DATA_ROW.telemetry, history: DATA_ROW.telemetry, types: [V6_DATA.gear, V6_DATA.throttle, V6_DATA.brake, V6_DATA.steering] },
  powerCards: { stream: DATA_ROW.status, types: [V6_DATA.fuel, V6_DATA.ersStore, V6_DATA.ersHarvest, V6_DATA.ersDeployment, V6_DATA.enginePower, V6_DATA.brakeBias] },
  powerHistory: { stream: DATA_ROW.status, history: DATA_ROW.status, types: [V6_DATA.fuel, V6_DATA.ersStore, V6_DATA.ersHarvest, V6_DATA.ersDeployment, V6_DATA.enginePower] },
  tyrePageState: { stream: DATA_ROW.tyreSets | DATA_ROW.telemetry | DATA_ROW.damage | DATA_ROW.session, types: [V6_DATA.tyreState, V6_DATA.tyreSurfaceTemp, V6_DATA.tyreInnerTemp, V6_DATA.brakeTemp, V6_DATA.tyreWear, V6_DATA.damage] },
  strategyPage: { stream: DATA_ROW.strategy, types: [V6_DATA.fuel, V6_DATA.tyreState, V6_DATA.tyreWear, V6_DATA.lapTiming] },
  gForceHistory: { stream: DATA_ROW.motion, history: DATA_ROW.motion, types: [V6_DATA.gForce] },
  rideHeightHistory: { stream: DATA_ROW.motionEx, history: DATA_ROW.motionEx, types: [V6_DATA.rideHeight] },
  analyzeLapCoordinates: { stream: DATA_ROW.lap, history: DATA_ROW.lap, types: [V6_DATA.lapTiming] },
  analyzeMap: {
    stream: DATA_ROW.positions | DATA_ROW.lap | DATA_ROW.telemetry,
    history: DATA_ROW.positions | DATA_ROW.lap | DATA_ROW.telemetry,
    types: [V6_DATA.position, V6_DATA.lapTiming, V6_DATA.speed],
  },
} as const satisfies Record<string, Requirement>

function add(target: DataRequirements, requirement: Requirement): void {
  target.streamMask |= requirement.stream
  target.historyMask |= requirement.history ?? 0
  if (requirement.types) {
    target.v6Types.push(...requirement.types)
    if (requirement.history) target.v6HistoryTypes.push(...requirement.types)
  }
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
  pageLayouts?: PageLayouts,
): DataRequirements {
  const result: DataRequirements = { streamMask: 0, historyMask: 0, v6Types: [], v6HistoryTypes: [] }
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
        // Cards use current values only. They stay subscribed to the live row
        // families, but never trigger indexed history/backfill reads.
        result.streamMask |= DATA_ROW.telemetry
        result.v6Types.push(V6_DATA.tyreSurfaceTemp, V6_DATA.tyreInnerTemp, V6_DATA.brakeTemp)
        add(result, DATA_CONSUMERS.damageCards)
      }
    }
    if (any(core.damageItems)) add(result, DATA_CONSUMERS.damageCards)
  } else if (tab === 'timing_tower') {
    add(result, DATA_CONSUMERS.timingTower)
  } else if (tab === 'session') {
    add(result, DATA_CONSUMERS.sessionPage)
  } else if (tab === 'input') {
    if (input.showGear || input.showAccelerator || input.showBrake || input.showSteering) add(result, DATA_CONSUMERS.inputHistory)
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
    const showGForce = pageLayouts?.miscGForce === 'split'
      ? misc.showGLateral || misc.showGLongitudinal
      : misc.showGForce
    const showRideHeight = pageLayouts?.miscRideHeight === 'split'
      ? misc.showRideFront || misc.showRideRear
      : misc.showRideHeight
    if (showGForce) add(result, DATA_CONSUMERS.gForceHistory)
    if (showRideHeight) add(result, DATA_CONSUMERS.rideHeightHistory)
  } else if (tab === 'analyze') {
    result.streamMask |= analyzeMask
    result.historyMask |= analyzeMask
    result.v6Types.push(...v6TypesForRowMask(analyzeMask))
    result.v6HistoryTypes.push(...v6TypesForRowMask(analyzeMask))
  }

  result.streamMask |= result.historyMask
  result.streamMask >>>= 0
  result.historyMask >>>= 0
  result.v6Types = [...new Set(result.v6Types)].sort((a, b) => a - b)
  result.v6HistoryTypes = [...new Set(result.v6HistoryTypes)].sort((a, b) => a - b)
  return result
}

export function v6TypesForRowMask(mask: number): number[] {
  const values: number[] = []
  if (mask & DATA_ROW.telemetry) values.push(...[1,2,3,4,5,6,7,8,9,10,11])
  if (mask & DATA_ROW.status) values.push(...[7,13,15,16,17,18,19,20])
  if (mask & DATA_ROW.damage) values.push(12,14)
  if (mask & DATA_ROW.tyreSets) values.push(13)
  if (mask & (DATA_ROW.lap | DATA_ROW.timing)) values.push(24)
  if (mask & DATA_ROW.motion) values.push(21)
  if (mask & DATA_ROW.motionEx) values.push(22)
  if (mask & DATA_ROW.positions) values.push(23)
  return [...new Set(values)].sort((a, b) => a - b)
}

export function visibleChartSectionsForUi(
  tab: Tab,
  core: CoreLayout,
  input: InputLayout,
  pageLayouts: PageLayouts,
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
    const pedalSections: GraphSection[] = pageLayouts.inputPedals === 'split'
      ? [
          ...(input.showAccelerator ? ['inputAccelerator' as const] : []),
          ...(input.showBrake ? ['inputBrake' as const] : []),
        ]
      : input.showAccelerator || input.showBrake
        ? [pageLayouts.inputPedals === 'combined2' ? 'inputThrottleBrakeOverlay' : 'inputThrottleBrake']
        : []
    return [
      ...(input.showGear ? ['inputGear' as const] : []),
      ...pedalSections,
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
    const gForceSections: GraphSection[] = pageLayouts.miscGForce === 'split'
      ? [
          ...(misc.showGLateral ? ['miscGLateral' as const] : []),
          ...(misc.showGLongitudinal ? ['miscGLongitudinal' as const] : []),
        ]
      : misc.showGForce ? ['miscGForce'] : []
    const rideHeightSections: GraphSection[] = pageLayouts.miscRideHeight === 'split'
      ? [
          ...(misc.showRideFront ? ['miscRideFront' as const] : []),
          ...(misc.showRideRear ? ['miscRideRear' as const] : []),
        ]
      : misc.showRideHeight ? ['miscRideHeight'] : []
    return [
      ...gForceSections,
      ...rideHeightSections,
    ]
  }
  return []
}

export function dataMaskForAnalyze(
  view: 'graph' | 'charts' | 'split' | 'map',
  series: readonly AnalyzeSeriesConfig[],
): number {
  if (view === 'map') return DATA_CONSUMERS.analyzeMap.history
  let mask = DATA_CONSUMERS.analyzeLapCoordinates.history |
    (view === 'split' ? DATA_CONSUMERS.analyzeMap.history : 0)
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
  pageLayouts?: PageLayouts,
): number {
  return dataRequirementsForUi(tab, core, input, misc, power, tyres, tyreView, false, 0, pageLayouts).historyMask
}
