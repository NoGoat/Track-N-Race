import type { CoreLayout, InputLayout, MiscLayout, PowerLayout, Tab, TyresLayout } from '../app/appConfig'

export const HISTORY_ROW = {
  telemetry: 1 << 1,
  status: 1 << 2,
  damage: 1 << 3,
  lap: 1 << 4,
  motion: 1 << 11,
  motionEx: 1 << 12,
} as const

// One coordinator computes the union for every configured visible chart/card.
// Components never issue native history requests independently.
export function historyMaskForLayouts(
  tab: Tab,
  core: CoreLayout,
  input: InputLayout,
  misc: MiscLayout,
  power: PowerLayout,
  tyres: TyresLayout,
  tyreView: 'cards' | 'graphs',
): number {
  let mask = HISTORY_ROW.lap
  const any = (values: object) => Object.values(values).some(Boolean)

  if (tab === 'input') return (mask | (input.showGear || input.showInputs || input.showSteering ? HISTORY_ROW.telemetry : 0)) >>> 0
  if (tab === 'misc') {
    if (misc.showGForce) mask |= HISTORY_ROW.motion
    if (misc.showRideHeight) mask |= HISTORY_ROW.motionEx
    return mask >>> 0
  }
  if (tab === 'power') return (mask | (any(power.charts) ? HISTORY_ROW.status : 0)) >>> 0
  if (tab === 'tyres') {
    // The compact wheel cards can display telemetry history; expanded graphs
    // use telemetry for temperatures and damage only for the wear series.
    mask |= HISTORY_ROW.telemetry
    if (tyres.charts.tyreLife) mask |= HISTORY_ROW.damage
    return mask >>> 0
  }
  if (tab !== 'core') return mask >>> 0

  const coreThermal = core.showThermal &&
    (any(core.thermalGraphs) || any(core.thermalCards))
  if (core.showSpeedChart) mask |= HISTORY_ROW.telemetry | HISTORY_ROW.status
  if (coreThermal && tyreView === 'graphs') {
    const { surfaceTemp, innerTemp, brakeTemp, tyreLife } = core.thermalGraphs
    if (surfaceTemp || innerTemp || brakeTemp) mask |= HISTORY_ROW.telemetry
    if (tyreLife) mask |= HISTORY_ROW.damage
  } else if (coreThermal) {
    // Card table views use the telemetry history behind the four wheel cards.
    mask |= HISTORY_ROW.telemetry
  }
  return mask >>> 0
}
