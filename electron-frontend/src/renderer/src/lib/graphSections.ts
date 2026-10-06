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
  | 'inputGear' | 'inputThrottleBrake' | 'inputThrottleBrakeOverlay' | 'inputAccelerator' | 'inputBrake' | 'inputSteering'
  | 'powerSplit' | 'powerHarvest' | 'powerStore' | 'powerFuel'
  | 'miscGForce' | 'miscGLateral' | 'miscGLongitudinal'
  | 'miscRideHeight' | 'miscRideFront' | 'miscRideRear'

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
    { key: 'inputThrottleBrake', label: 'Accelerator / Brake' },
    { key: 'inputThrottleBrakeOverlay', label: 'Accelerator / Brake (Combined 2)' },
    { key: 'inputAccelerator',   label: 'Accelerator (Split)' },
    { key: 'inputBrake',         label: 'Brake (Split)' },
    { key: 'inputSteering',      label: 'Steering' },
  ] },
  { group: 'Power', sections: [
    { key: 'powerSplit',   label: 'Power' },
    { key: 'powerHarvest', label: 'ERS Harvest' },
    { key: 'powerStore',   label: 'ERS Store' },
    { key: 'powerFuel',    label: 'Fuel History' },
  ] },
  { group: 'Misc', sections: [
    { key: 'miscGForce',     label: 'G-Force' },
    { key: 'miscGLateral',   label: 'G-Force — Lateral (Split)' },
    { key: 'miscGLongitudinal', label: 'G-Force — Longitudinal (Split)' },
    { key: 'miscRideHeight', label: 'Ride Height' },
    { key: 'miscRideFront',  label: 'Ride Height — Front (Split)' },
    { key: 'miscRideRear',   label: 'Ride Height — Rear (Split)' },
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
/** Analysis value axes, one per analyze metric scale key. */
export type AnalysisYAxisKey =
  | 'speed' | 'rpm' | 'gear' | 'input-positive' | 'input-signed' | 'percent' | 'g-force'
  | 'ride-height' | 'power' | 'harvest' | 'fuel' | 'tyre-temp' | 'brake-temp'
export type AnalysisYAxisState = Record<AnalysisYAxisKey, YAxisBehavior>
export interface ChartYAxisState {
  overview: TyreYAxisGroupState
  tyres: TyreYAxisGroupState
  power: PowerYAxisState
  analysis: AnalysisYAxisState
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

/**
 * Fixed range of an Analysis axis: the y range the live page chart showing the
 * same values uses ('fixed' never moves, 'expand' grows once data comes within
 * a pad of an edge). Harvest and fuel resolve their upper bound at runtime.
 */
export interface AnalysisFixedYRange {
  min: number
  max: number
  expand?: { lowerPad: number; upperPad: number; expandLower: boolean }
}

export const ANALYSIS_Y_AXIS_SECTIONS: { key: AnalysisYAxisKey; label: string; fixedRange: string; range: AnalysisFixedYRange }[] = [
  { key: 'speed',          label: 'Speed',                  fixedRange: 'Always 0–380 km/h',            range: { min: 0, max: 380 } },
  { key: 'rpm',            label: 'RPM',                    fixedRange: 'Always 0–16,000 rpm',          range: { min: 0, max: 16000 } },
  { key: 'gear',           label: 'Gear',                   fixedRange: 'Always gears 1–8',             range: { min: 0.5, max: 8.5 } },
  { key: 'input-positive', label: 'Throttle / Brake',       fixedRange: 'Always 0–100%',                range: { min: 0, max: 1 } },
  { key: 'input-signed',   label: 'Steering',               fixedRange: 'Always −100–100%',             range: { min: -1, max: 1 } },
  { key: 'percent',        label: 'ERS / Tyre Wear / Life', fixedRange: 'Always 0–100%',                range: { min: 0, max: 100 } },
  { key: 'g-force',        label: 'G-Force',                fixedRange: 'Always −6–6 g',                range: { min: -6, max: 6 } },
  { key: 'ride-height',    label: 'Ride Height',            fixedRange: '0–50 mm; expands when needed', range: { min: 0, max: 50, expand: { lowerPad: 2, upperPad: 5, expandLower: true } } },
  { key: 'power',          label: 'ICE / MGU-K Power',      fixedRange: '0–500 kW; expands above 500 kW when needed', range: { min: 0, max: 500, expand: { lowerPad: 0, upperPad: 0, expandLower: false } } },
  { key: 'harvest',        label: 'MGU-K / MGU-H Harvest',  fixedRange: '0–4000/8000 kJ by Formula; expands above when needed', range: { min: 0, max: 4000, expand: { lowerPad: 0, upperPad: 0, expandLower: false } } },
  { key: 'fuel',           label: 'Fuel',                   fixedRange: "Always 0 kg to the session's fuel load + 1 kg", range: { min: 0, max: 110 } },
  { key: 'tyre-temp',      label: 'Surface / Inner Temp',   fixedRange: '0–125°C; expands above 125°C when needed', range: { min: 0, max: 125, expand: { lowerPad: 0, upperPad: 0, expandLower: false } } },
  { key: 'brake-temp',     label: 'Brake Temp',             fixedRange: '0–1250°C; expands above 1250°C when needed', range: { min: 0, max: 1250, expand: { lowerPad: 0, upperPad: 0, expandLower: false } } },
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
  analysis: Object.fromEntries(ANALYSIS_Y_AXIS_SECTIONS.map(section => [section.key, 'fixed'])) as AnalysisYAxisState,
}

// ── Compact / Spacious density ───────────────────────────────────────────────

export type DensityMode = 'compact' | 'normal' | 'spacious'

// Density controls across all sections. overviewTyres has 7 levels:
//   6 Spacious, 0 Normal, 1–4 native Compact 1–4, 5 = compact tyre-card layout.
// sessionWeather has 5 levels: 4 Spacious, 0 Normal, 1 Compact 1 (icons),
//   2 Compact 2 (icon-free single-line), 3 Compact 3 (single-line with icon).
// sessionHeader has 4 levels: 3 Spacious, 0 Normal, 1 Compact 1 (with Zones label),
//   2 Compact 2 (removes Zones label, marshal strip stretches full width).
export interface CompactState {
  overviewStats:     DensityMode
  overviewDamage:    DensityMode
  overviewTyres:     number
  standingsTable:    DensityMode
  standingsTiming:   DensityMode
  standingsErs:      DensityMode
  standingsStrategy: DensityMode
  sessionCards:      DensityMode
  sessionProximity:  DensityMode
  sessionEvents:     DensityMode
  sessionWeather:    number
  sessionHeader:     number
  powerCards:        DensityMode
  strategySummary:   DensityMode
  stintSummary:      DensityMode
  damageSummary:     DensityMode
  playbackBar:       DensityMode
}

export const DEFAULT_COMPACT: CompactState = {
  overviewStats:     'normal',
  overviewDamage:    'normal',
  overviewTyres:     0,
  standingsTable:    'normal',
  standingsTiming:   'normal',
  standingsErs:      'normal',
  standingsStrategy: 'normal',
  sessionCards:      'normal',
  sessionProximity:  'normal',
  sessionEvents:     'normal',
  sessionWeather:    0,
  sessionHeader:     0,
  powerCards:        'normal',
  strategySummary:   'normal',
  stintSummary:      'normal',
  damageSummary:     'normal',
  playbackBar:       'normal',
}

// Density key sections — the integer controls are handled
// separately because they expose more than two density levels.
export type CompactDensityKey = Exclude<keyof CompactState, 'overviewTyres' | 'sessionWeather' | 'sessionHeader'>
export type CompactBoolKey = CompactDensityKey

export const COMPACT_GROUPS: { group: string; sections: { key: CompactDensityKey; label: string }[] }[] = [
  { group: 'Overview', sections: [
    { key: 'overviewStats',  label: 'Stats Row' },
    { key: 'overviewDamage', label: 'Damage Cards' },
    // overviewTyres (Tyre Cards) rendered as the multi-level control in Settings.
  ] },
  { group: 'Standings', sections: [
    { key: 'standingsTable',    label: 'Timing Tower' },
    { key: 'standingsTiming',   label: 'Timing Card' },
    { key: 'standingsErs',      label: 'Energy Recovery Card' },
    { key: 'standingsStrategy', label: 'Strategy Card' },
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
  { group: 'Trends', sections: [
    { key: 'stintSummary', label: 'Summary Cards' },
  ] },
  { group: 'Damage', sections: [
    { key: 'damageSummary', label: 'Summary Cards' },
  ] },
  { group: 'Playback', sections: [
    { key: 'playbackBar', label: 'Playback Bar' },
  ] },
]

export const ALL_COMPACT_BOOL_KEYS: CompactDensityKey[] =
  COMPACT_GROUPS.flatMap(g => g.sections.map(s => s.key))

export const DENSITY_OPTIONS: { value: DensityMode; label: string }[] = [
  { value: 'compact',  label: 'Compact' },
  { value: 'normal',   label: 'Normal' },
  { value: 'spacious', label: 'Spacious' },
]

export const TYRE_LEVEL_OPTIONS: { value: number; label: string }[] = [
  { value: 6, label: 'Spacious' },
  { value: 0, label: 'Normal' },
  { value: 1, label: 'Compact 1' },
  { value: 2, label: 'Compact 2' },
  { value: 3, label: 'Compact 3' },
  { value: 4, label: 'Compact 4' },
  { value: 5, label: 'Compact 5' },
]

export const WEATHER_LEVEL_OPTIONS: { value: number; label: string }[] = [
  { value: 4, label: 'Spacious' },
  { value: 0, label: 'Normal' },
  { value: 1, label: 'Compact 1' },
  { value: 2, label: 'Compact 2' },
  { value: 3, label: 'Compact 3' },
]

export const HEADER_LEVEL_OPTIONS: { value: number; label: string }[] = [
  { value: 3, label: 'Spacious' },
  { value: 0, label: 'Normal' },
  { value: 1, label: 'Compact 1' },
  { value: 2, label: 'Compact 2' },
]
