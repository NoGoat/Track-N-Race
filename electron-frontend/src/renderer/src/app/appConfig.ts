export const WINDOWS: { label: string; value: number }[] = [
  { label: '15s', value: 15 },
  { label: '30s', value: 30 },
  { label: '1m', value: 60 },
  { label: '2m', value: 120 },
  { label: '5m', value: 300 },
  { label: '10m', value: 600 },
]

export type Tab = 'core' | 'analyze' | 'timing_tower' | 'input' | 'misc' | 'power' | 'tyres' | 'session' | 'strategy'
export type Theme = 'dark' | 'midnight' | 'light'
export type TitlebarUpdateInterval = 0 | 250 | 500 | 1000
export type DistanceChartMode = 'CL' | 'PL' | 'FL' | 'RL'
export type ChartMode = DistanceChartMode | 'AL' | 'SL'
export type ChartWindow = number | ChartMode

export interface CoreLayout {
  showStats: boolean
  showSpeedChart: boolean
  showThermal: boolean
  statsCards: {
    speed: boolean; rpm: boolean; gear: boolean; throttle: boolean; brake: boolean
    drs: boolean; engine: boolean; ers: boolean; fuel: boolean; pos: boolean; tyre: boolean
  }
  thermalGraphs: { surfaceTemp: boolean; innerTemp: boolean; brakeTemp: boolean; tyreLife: boolean }
  thermalCards: { fl: boolean; fr: boolean; rl: boolean; rr: boolean }
  damageItems: {
    wingFl: boolean; wingFr: boolean; wingRear: boolean; floor: boolean
    diffuser: boolean; sidepod: boolean; gearbox: boolean; engine: boolean
    tyreDmgFl: boolean; tyreDmgFr: boolean; tyreDmgRl: boolean; tyreDmgRr: boolean
    brakeDmgFl: boolean; brakeDmgFr: boolean; brakeDmgRl: boolean; brakeDmgRr: boolean
  }
}

export const DEFAULT_CORE_LAYOUT: CoreLayout = {
  showStats: true, showSpeedChart: true, showThermal: true,
  statsCards: { speed: true, rpm: true, gear: true, throttle: true, brake: true, drs: true, engine: true, ers: true, fuel: true, pos: true, tyre: true },
  thermalGraphs: { surfaceTemp: true, innerTemp: true, brakeTemp: true, tyreLife: true },
  thermalCards: { fl: true, fr: true, rl: true, rr: true },
  damageItems: { wingFl: true, wingFr: true, wingRear: true, floor: true, diffuser: true, sidepod: false, gearbox: true, engine: true, tyreDmgFl: false, tyreDmgFr: false, tyreDmgRl: false, tyreDmgRr: false, brakeDmgFl: false, brakeDmgFr: false, brakeDmgRl: false, brakeDmgRr: false },
}

export interface InputLayout { showGear: boolean; showInputs: boolean; showSteering: boolean }
export const DEFAULT_INPUT_LAYOUT: InputLayout = { showGear: true, showInputs: true, showSteering: true }

export type InputPageLayout = 'grid' | 'vertical'
export interface PageLayouts { input: InputPageLayout }
export const DEFAULT_PAGE_LAYOUTS: PageLayouts = { input: 'grid' }

export interface MiscLayout { showGForce: boolean; showRideHeight: boolean }
export const DEFAULT_MISC_LAYOUT: MiscLayout = { showGForce: true, showRideHeight: true }

export interface PowerLayout {
  statsCards: { totalPower: boolean; ice: boolean; mguk: boolean; split: boolean; ersStore: boolean; ersPct: boolean; fuel: boolean }
  charts: { powerSplit: boolean; ersHarvest: boolean; ersStore: boolean; fuelHistory: boolean }
}
export const DEFAULT_POWER_LAYOUT: PowerLayout = {
  statsCards: { totalPower: true, ice: true, mguk: true, split: true, ersStore: true, ersPct: true, fuel: true },
  charts: { powerSplit: true, ersHarvest: true, ersStore: true, fuelHistory: true },
}

export interface TyresLayout { charts: { surfaceTemp: boolean; innerTemp: boolean; brakeTemp: boolean; tyreLife: boolean } }
export const DEFAULT_TYRES_LAYOUT: TyresLayout = { charts: { surfaceTemp: true, innerTemp: true, brakeTemp: true, tyreLife: true } }

export interface SessionLayout {
  header: {
    gpName: boolean
    marshalZones: boolean
    timeLeft: boolean
  }
  showMap: boolean
  showProximity: boolean
  showEvents: boolean
  showWeather: boolean
  sidebarPct: number
  statsCards: {
    totalLaps: boolean
    lapsRemaining: boolean
    pitSpeedLimit: boolean
    pitWindow: boolean
    pitRejoin: boolean
    trackTemp: boolean
    airTemp: boolean
    trackLength: boolean
    timeOfDay: boolean
  }
}
export const DEFAULT_SESSION_LAYOUT: SessionLayout = {
  header: {
    gpName: true,
    marshalZones: true,
    timeLeft: true,
  },
  showMap: true,
  showProximity: true,
  showEvents: true,
  showWeather: true,
  sidebarPct: 28,
  statsCards: {
    totalLaps: true,
    lapsRemaining: true,
    pitSpeedLimit: true,
    pitWindow: true,
    pitRejoin: true,
    trackTemp: true,
    airTemp: true,
    trackLength: true,
    timeOfDay: true,
  }
}

export interface StandingsLayout {
  showTimingTower: boolean
  sidebarPct: number
  cards: {
    timing: boolean
    energyRecovery: boolean
    strategy: boolean
  }
}
export const DEFAULT_STANDINGS_LAYOUT: StandingsLayout = {
  showTimingTower: true,
  sidebarPct: 28,
  cards: {
    timing: true,
    energyRecovery: true,
    strategy: true,
  },
}

export const TAB_LABELS: Record<Tab, string> = {
  core: 'Overview', analyze: 'Analysis', timing_tower: 'Standings', input: 'Input', power: 'Power', tyres: 'Tyres', session: 'Session', misc: 'Misc', strategy: 'Strategy'
}

export const TAB_OPTIONS = (['core', 'analyze', 'session', 'strategy', 'timing_tower', 'input', 'power', 'tyres', 'misc'] as Tab[])
  .map(value => ({ value, label: TAB_LABELS[value] }))
export interface ChartWindowOption { value: ChartWindow; label: string }
export interface ChartWindowOptionGroup { label: string; options: ChartWindowOption[] }

export const WINDOW_OPTIONS: ChartWindowOption[] = WINDOWS.map(({ value, label }) => ({ value, label }))

export function getChartWindowOptionGroups(clAvailable: boolean, recordingOpen: boolean): ChartWindowOptionGroup[] {
  const lapOptions: ChartWindowOption[] = clAvailable
    ? [
        { value: 'CL', label: 'Current' },
        { value: 'PL', label: 'Previous' },
        { value: 'FL', label: 'Fastest' },
        ...(recordingOpen ? [{ value: 'RL' as const, label: 'Selected' }] : []),
      ]
    : []
  return [
    { label: 'Laps', options: [...lapOptions, { value: 'SL', label: 'Stint Laps' }, { value: 'AL', label: 'All Laps' }] },
    { label: 'Time', options: WINDOW_OPTIONS },
  ]
}
export const PLAYBACK_SPEED_OPTIONS = [
  { value: 0.25, label: '0.25x' },
  { value: 0.5, label: '0.5x' },
  { value: 1, label: '1x' },
  { value: 2, label: '2x' },
  { value: 4, label: '4x' },
]
