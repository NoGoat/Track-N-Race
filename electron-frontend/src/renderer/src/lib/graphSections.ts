// Shared Settings metadata. The first two features mirror qt_frontend; the
// chart Y-axis behavior below is intentionally Electron-only:
//   • per-graph Chart/Table view mode  (mirrors native GraphViewSettings.h)
//   • per-section compact density       (mirrors native CompactSettings.h)
//
// The unions, defaults and Settings grouping live here so App.tsx (which
// owns the persisted state) and Settings.tsx (which renders the controls) agree on
// the same keys — the same single-source-of-truth pattern the native headers use.

// ── Graphs: Chart vs Table ──────────────────────────────────────────────────

export type GraphView = 'chart' | 'table'

export type GraphSection =
  | 'overviewTelemetry'
  | 'overviewTyreSurface' | 'overviewTyreInner' | 'overviewTyreBrake' | 'overviewTyreWear'
  | 'overviewTyreCardFL' | 'overviewTyreCardFR' | 'overviewTyreCardRL' | 'overviewTyreCardRR'
  | 'tyreSurface' | 'tyreInner' | 'tyreBrake' | 'tyreWear'
  | 'tyreCardFL' | 'tyreCardFR' | 'tyreCardRL' | 'tyreCardRR'
  | 'inputGear' | 'inputThrottleBrake' | 'inputSteering'
  | 'powerSplit' | 'powerHarvest' | 'powerStore' | 'powerFuel'
  | 'miscGForce' | 'miscRideHeight'

export type GraphViewState = Record<GraphSection, GraphView>

// Grouped by the tab the graph lives on — drives the Settings "Graphs" page and
// also enumerates every section for defaults / set-all.
export const GRAPH_GROUPS: { group: string; sections: { key: GraphSection; label: string; chartLabel?: string }[] }[] = [
  { group: 'Overview', sections: [
    { key: 'overviewTelemetry',   label: 'Speed / RPM / ERS' },
    { key: 'overviewTyreSurface', label: 'Tyre Surface Temp' },
    { key: 'overviewTyreInner',   label: 'Tyre Inner Temp' },
    { key: 'overviewTyreBrake',   label: 'Brake Temp' },
    { key: 'overviewTyreWear',    label: 'Tyre Wear / Life' },
    { key: 'overviewTyreCardFL',  label: 'Tyre Card FL', chartLabel: 'Card' },
    { key: 'overviewTyreCardFR',  label: 'Tyre Card FR', chartLabel: 'Card' },
    { key: 'overviewTyreCardRL',  label: 'Tyre Card RL', chartLabel: 'Card' },
    { key: 'overviewTyreCardRR',  label: 'Tyre Card RR', chartLabel: 'Card' },
  ] },
  { group: 'Tyres', sections: [
    { key: 'tyreSurface', label: 'Tyre Surface Temp' },
    { key: 'tyreInner',   label: 'Tyre Inner Temp' },
    { key: 'tyreBrake',   label: 'Brake Temp' },
    { key: 'tyreWear',    label: 'Tyre Wear / Life' },
    { key: 'tyreCardFL',  label: 'Tyre Card FL', chartLabel: 'Card' },
    { key: 'tyreCardFR',  label: 'Tyre Card FR', chartLabel: 'Card' },
    { key: 'tyreCardRL',  label: 'Tyre Card RL', chartLabel: 'Card' },
    { key: 'tyreCardRR',  label: 'Tyre Card RR', chartLabel: 'Card' },
  ] },
  { group: 'Input', sections: [
    { key: 'inputGear',          label: 'Gear' },
    { key: 'inputThrottleBrake', label: 'Throttle / Brake' },
    { key: 'inputSteering',      label: 'Steering' },
  ] },
  { group: 'Power', sections: [
    { key: 'powerSplit',   label: 'Power Split' },
    { key: 'powerHarvest', label: 'ERS Harvest' },
    { key: 'powerStore',   label: 'ERS Store' },
    { key: 'powerFuel',    label: 'Fuel History' },
  ] },
  { group: 'Misc', sections: [
    { key: 'miscGForce',     label: 'G-Force' },
    { key: 'miscRideHeight', label: 'Ride Height' },
  ] },
]

export const ALL_GRAPH_SECTIONS: GraphSection[] =
  GRAPH_GROUPS.flatMap(g => g.sections.map(s => s.key))

export const DEFAULT_GRAPH_VIEW: GraphViewState =
  Object.fromEntries(ALL_GRAPH_SECTIONS.map(k => [k, 'chart'])) as GraphViewState

// ── Chart Y axes ───────────────────────────────────────────────────────────────

export type YAxisBehavior = 'fixed' | 'dynamic'
export type TyreYAxisKey = 'surfaceTemp' | 'innerTemp' | 'brakeTemp' | 'tyreLife'
export type TyreYAxisGroupState = Record<TyreYAxisKey, YAxisBehavior>
export type PowerYAxisKey = 'ersHarvest'
export type PowerYAxisState = Record<PowerYAxisKey, YAxisBehavior>
export interface ChartYAxisState {
  overview: TyreYAxisGroupState
  tyres: TyreYAxisGroupState
  power: PowerYAxisState
}

export const TYRE_Y_AXIS_SECTIONS: { key: TyreYAxisKey; label: string; fixedRange: string }[] = [
  { key: 'surfaceTemp', label: 'Surface Temp',     fixedRange: '0–125°C; expands above 125°C when needed' },
  { key: 'innerTemp',   label: 'Inner Temp',       fixedRange: '0–125°C; expands above 125°C when needed' },
  { key: 'brakeTemp',   label: 'Brake Temp',       fixedRange: '0–1250°C; expands above 1250°C when needed' },
  { key: 'tyreLife',    label: 'Tyre Wear / Life', fixedRange: 'Always 0–100%' },
]

export const POWER_Y_AXIS_SECTIONS: { key: PowerYAxisKey; label: string; fixedRange: string }[] = [
  { key: 'ersHarvest', label: 'ERS Harvest', fixedRange: '0–4000/8000 kJ by Formula; expands above when needed' },
]

const DEFAULT_TYRE_Y_AXIS_GROUP: TyreYAxisGroupState = {
  surfaceTemp: 'fixed',
  innerTemp:   'fixed',
  brakeTemp:   'fixed',
  tyreLife:    'fixed',
}

export const DEFAULT_CHART_Y_AXIS: ChartYAxisState = {
  overview: { ...DEFAULT_TYRE_Y_AXIS_GROUP },
  tyres:    { ...DEFAULT_TYRE_Y_AXIS_GROUP },
  power:    { ersHarvest: 'fixed' },
}

// ── Compact density ─────────────────────────────────────────────────────────

// 5 boolean sections + three integer density controls. overviewTyres has six levels:
//   0 Normal, 1–4 native Compact 1–4, 5 = the app's existing height-derived
//   compact tyre-card layout, kept as a selectable level.
// sessionWeather has four levels: 0 Normal, 1 Compact 1 (icons),
// 2 Compact 2 (the original icon-free single-line compact layout), and
// 3 Compact 3 (the single-line compact layout: [Icon] Condition ... +Time Rain%).
// sessionHeader has three levels: 0 Normal, 1 Compact 1 (with Zones label),
// 2 Compact 2 (removes Zones label, marshal strip stretches full width).
export interface CompactState {
  overviewStats:   boolean
  overviewDamage:  boolean
  overviewTyres:   number
  sessionCards:    boolean
  sessionProximity: boolean
  sessionEvents:   boolean
  sessionWeather:  number
  sessionHeader:   number
  powerCards:      boolean
  strategySummary: boolean
  playbackBar:     boolean
}

export const DEFAULT_COMPACT: CompactState = {
  overviewStats:   false,
  overviewDamage:  false,
  overviewTyres:   0,
  sessionCards:    false,
  sessionProximity: false,
  sessionEvents:   false,
  sessionWeather:  0,
  sessionHeader:   0,
  powerCards:      false,
  strategySummary: false,
  playbackBar:     false,
}

// Boolean (Normal/Compact) compact sections — the integer controls are handled
// separately because they expose more than two density levels.
export type CompactBoolKey = Exclude<keyof CompactState, 'overviewTyres' | 'sessionWeather' | 'sessionHeader'>

export const COMPACT_GROUPS: { group: string; sections: { key: CompactBoolKey; label: string }[] }[] = [
  { group: 'Overview', sections: [
    { key: 'overviewStats',  label: 'Stats Row' },
    { key: 'overviewDamage', label: 'Damage Cards' },
    // overviewTyres (Tyre Cards) rendered as the 6-level control in Settings.
  ] },
  { group: 'Session', sections: [
    { key: 'sessionCards',     label: 'Info Cards' },
    { key: 'sessionProximity', label: 'Proximity' },
    { key: 'sessionEvents',    label: 'Events' },
    // sessionHeader and sessionWeather rendered as integer-level controls in Settings.
  ] },
  { group: 'Power', sections: [
    { key: 'powerCards', label: 'Power Cards' },
  ] },
  { group: 'Strategy', sections: [
    { key: 'strategySummary', label: 'Summary Header' },
  ] },
  { group: 'Playback', sections: [
    { key: 'playbackBar', label: 'Playback Bar' },
  ] },
]

export const ALL_COMPACT_BOOL_KEYS: CompactBoolKey[] =
  COMPACT_GROUPS.flatMap(g => g.sections.map(s => s.key))

export const TYRE_LEVEL_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: 'Normal' },
  { value: 1, label: 'Compact 1' },
  { value: 2, label: 'Compact 2' },
  { value: 3, label: 'Compact 3' },
  { value: 4, label: 'Compact 4' },
  { value: 5, label: 'Compact 5' },
]

export const WEATHER_LEVEL_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: 'Normal' },
  { value: 1, label: 'Compact 1' },
  { value: 2, label: 'Compact 2' },
  { value: 3, label: 'Compact 3' },
]

export const HEADER_LEVEL_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: 'Normal' },
  { value: 1, label: 'Compact 1' },
  { value: 2, label: 'Compact 2' },
]
