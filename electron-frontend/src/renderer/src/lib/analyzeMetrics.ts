import type { DamageRow, MotionExRow, MotionRow, StatusRow, TelemetryRow } from '../types'

export type AnalyzeSource = 'telemetry' | 'motion' | 'motionEx' | 'status' | 'damage'
export type AnalyzeMetricId = string

export interface AnalyzeMetricDefinition {
  id: AnalyzeMetricId
  group: 'Driving' | 'Motion' | 'Power' | 'Tyres'
  label: string
  source: AnalyzeSource
  defaultColor: string
  scaleKey: string
  min: number
  max: number
  unit: string
  lineType?: 'line' | 'step'
  getValue: (row: any) => number
  format: (value: number) => string
  axisFormat: (value: number) => string
}

export interface AnalyzeSeriesConfig {
  metricId: AnalyzeMetricId
  color: string
  negativeColor?: string
  visible: boolean
  showYAxis: boolean
  /** Combined tyre cards only: the corner keys (fl/fr/rl/rr) drawn on the card. */
  corners?: string[]
  /** Combined tyre cards only: custom line colour per corner key. */
  cornerColors?: Partial<Record<string, string>>
}

export interface AnalyzeConfig {
  version: 9
  collapsed: boolean
  view: 'graph' | 'split' | 'map'
  individualGraphs: boolean
  syncedTooltip: boolean
  sectorBoundaries: boolean
  sectorDelta: boolean
  currentLabel: string
  compareLabel: string
  lapALabel: string
  lapBLabel: string
  mapCurrentColor: string
  mapComparisonColor: string
  series: AnalyzeSeriesConfig[]
}

export const DEFAULT_MAP_CURRENT_COLOR = '#5794F2'
export const DEFAULT_MAP_COMPARISON_COLOR = '#C4162A'

export const DEFAULT_DELTA_POSITIVE_COLOR = '#C4162A'
export const DEFAULT_DELTA_NEGATIVE_COLOR = '#37872D'

export const DEFAULT_CURRENT_LABEL = 'Current'
export const DEFAULT_COMPARE_LABEL = 'Compare'
export const DEFAULT_LAP_A_LABEL = 'Lap A'
export const DEFAULT_LAP_B_LABEL = 'Lap B'

const number = (digits = 0) => (value: number) => value.toFixed(digits)
const withUnit = (unit: string, digits = 0) => (value: number) => `${value.toFixed(digits)}${unit}`

function metric(def: AnalyzeMetricDefinition): AnalyzeMetricDefinition { return def }

const corners = [
  { key: 'fl', label: 'FL', color: '#e10600' },
  { key: 'fr', label: 'FR', color: '#4488ff' },
  { key: 'rl', label: 'RL', color: '#37872D' },
  { key: 'rr', label: 'RR', color: '#ffd700' },
] as const

const base: AnalyzeMetricDefinition[] = [
  metric({ id: 'speed', group: 'Driving', label: 'Speed', source: 'telemetry', defaultColor: '#37872D', scaleKey: 'speed', min: 0, max: 380, unit: 'km/h', getValue: (r: TelemetryRow) => r.speed_kph, format: withUnit(' km/h'), axisFormat: number() }),
  metric({ id: 'rpm', group: 'Driving', label: 'RPM', source: 'telemetry', defaultColor: '#C4162A', scaleKey: 'rpm', min: 0, max: 16000, unit: 'rpm', getValue: (r: TelemetryRow) => r.rpm, format: v => `${Math.round(v).toLocaleString()} rpm`, axisFormat: v => v === 0 ? '0' : `${Math.round(v / 1000)}k` }),
  metric({ id: 'gear', group: 'Driving', label: 'Gear', source: 'telemetry', defaultColor: '#5794F2', scaleKey: 'gear', min: 0.5, max: 8.5, unit: '', lineType: 'step', getValue: (r: TelemetryRow) => r.gear, format: v => `Gear ${Math.round(v)}`, axisFormat: v => String(Math.round(v)) }),
  metric({ id: 'throttle', group: 'Driving', label: 'Throttle', source: 'telemetry', defaultColor: '#37872D', scaleKey: 'input-positive', min: 0, max: 1, unit: '%', lineType: 'step', getValue: (r: TelemetryRow) => r.throttle, format: v => `${Math.round(v * 100)}%`, axisFormat: v => `${Math.round(v * 100)}%` }),
  metric({ id: 'brake', group: 'Driving', label: 'Brake', source: 'telemetry', defaultColor: '#C4162A', scaleKey: 'input-positive', min: 0, max: 1, unit: '%', lineType: 'step', getValue: (r: TelemetryRow) => r.brake, format: v => `${Math.round(v * 100)}%`, axisFormat: v => `${Math.round(v * 100)}%` }),
  metric({ id: 'steering', group: 'Driving', label: 'Steering', source: 'telemetry', defaultColor: '#BF5FFF', scaleKey: 'input-signed', min: -1, max: 1, unit: '%', getValue: (r: TelemetryRow) => r.steering, format: v => `${v < 0 ? 'L ' : v > 0 ? 'R ' : ''}${Math.round(Math.abs(v) * 100)}%`, axisFormat: v => `${Math.round(v * 100)}%` }),
  metric({ id: 'ers', group: 'Driving', label: 'ERS', source: 'status', defaultColor: '#FADE2A', scaleKey: 'percent', min: 0, max: 100, unit: '%', getValue: (r: StatusRow) => r.ers_pct, format: withUnit('%', 1), axisFormat: withUnit('%') }),
  metric({ id: 'g-lateral', group: 'Motion', label: 'Lateral G', source: 'motion', defaultColor: '#F0A500', scaleKey: 'g-force', min: -6, max: 6, unit: 'g', getValue: (r: MotionRow) => r.g_lat, format: withUnit(' g', 2), axisFormat: withUnit('g') }),
  metric({ id: 'g-longitudinal', group: 'Motion', label: 'Longitudinal G', source: 'motion', defaultColor: '#5794F2', scaleKey: 'g-force', min: -6, max: 6, unit: 'g', getValue: (r: MotionRow) => r.g_long, format: withUnit(' g', 2), axisFormat: withUnit('g') }),
  metric({ id: 'ride-front', group: 'Motion', label: 'Front Ride Height', source: 'motionEx', defaultColor: '#73BF69', scaleKey: 'ride-height', min: -2, max: 20, unit: 'mm', getValue: (r: MotionExRow) => r.front_aero_height_mm, format: withUnit(' mm', 1), axisFormat: withUnit('mm') }),
  metric({ id: 'ride-rear', group: 'Motion', label: 'Rear Ride Height', source: 'motionEx', defaultColor: '#B877DB', scaleKey: 'ride-height', min: -2, max: 20, unit: 'mm', getValue: (r: MotionExRow) => r.rear_aero_height_mm, format: withUnit(' mm', 1), axisFormat: withUnit('mm') }),
  metric({ id: 'power-ice', group: 'Power', label: 'ICE Power', source: 'status', defaultColor: '#5794F2', scaleKey: 'power', min: 0, max: 1000, unit: 'kW', getValue: (r: StatusRow) => r.engine_power_ice_kw ?? 0, format: withUnit(' kW', 1), axisFormat: withUnit('kW') }),
  metric({ id: 'power-mguk', group: 'Power', label: 'MGU-K Power', source: 'status', defaultColor: '#FADE2A', scaleKey: 'power', min: 0, max: 1000, unit: 'kW', getValue: (r: StatusRow) => r.engine_power_mguk_kw ?? 0, format: withUnit(' kW', 1), axisFormat: withUnit('kW') }),
  metric({ id: 'harvest-mguk', group: 'Power', label: 'MGU-K Harvest', source: 'status', defaultColor: '#37872D', scaleKey: 'harvest', min: 0, max: 2000, unit: 'kJ', getValue: (r: StatusRow) => (r.ers_harvested_mguk_j ?? 0) / 1000, format: withUnit(' kJ', 1), axisFormat: withUnit('kJ') }),
  metric({ id: 'harvest-mguh', group: 'Power', label: 'MGU-H Harvest', source: 'status', defaultColor: '#C4162A', scaleKey: 'harvest', min: 0, max: 2000, unit: 'kJ', getValue: (r: StatusRow) => (r.ers_harvested_mguh_j ?? 0) / 1000, format: withUnit(' kJ', 1), axisFormat: withUnit('kJ') }),
  metric({ id: 'fuel', group: 'Power', label: 'Fuel', source: 'status', defaultColor: '#F0A500', scaleKey: 'fuel', min: 0, max: 110, unit: 'kg', getValue: (r: StatusRow) => r.fuel_kg, format: withUnit(' kg', 2), axisFormat: withUnit('kg') }),
]

const tyreMetrics: AnalyzeMetricDefinition[] = corners.flatMap(corner => [
  metric({ id: `surface-${corner.key}`, group: 'Tyres', label: `Surface Temp ${corner.label}`, source: 'telemetry', defaultColor: corner.color, scaleKey: 'tyre-temp', min: 0, max: 125, unit: '°C', getValue: (r: TelemetryRow) => (r as any)[`tyre_temp_surface_${corner.key}`], format: withUnit(' °C', 1), axisFormat: withUnit('°') }),
  metric({ id: `inner-${corner.key}`, group: 'Tyres', label: `Inner Temp ${corner.label}`, source: 'telemetry', defaultColor: corner.color, scaleKey: 'tyre-temp', min: 0, max: 125, unit: '°C', getValue: (r: TelemetryRow) => (r as any)[`tyre_temp_inner_${corner.key}`], format: withUnit(' °C', 1), axisFormat: withUnit('°') }),
  metric({ id: `brake-temp-${corner.key}`, group: 'Tyres', label: `Brake Temp ${corner.label}`, source: 'telemetry', defaultColor: corner.color, scaleKey: 'brake-temp', min: 0, max: 1250, unit: '°C', getValue: (r: TelemetryRow) => (r as any)[`brake_temp_${corner.key}`], format: withUnit(' °C', 1), axisFormat: withUnit('°') }),
  metric({ id: `wear-${corner.key}`, group: 'Tyres', label: `Tyre Wear ${corner.label}`, source: 'damage', defaultColor: corner.color, scaleKey: 'percent', min: 0, max: 100, unit: '%', getValue: (r: DamageRow) => (r as any)[`tyre_wear_${corner.key}`], format: withUnit('%', 1), axisFormat: withUnit('%') }),
  metric({ id: `life-${corner.key}`, group: 'Tyres', label: `Tyre Life ${corner.label}`, source: 'damage', defaultColor: corner.color, scaleKey: 'percent', min: 0, max: 100, unit: '%', getValue: (r: DamageRow) => 100 - (r as any)[`tyre_wear_${corner.key}`], format: withUnit('%', 1), axisFormat: withUnit('%') }),
])

// The baseline across all four corners: the mean of whichever corners report.
function averageWear(row: DamageRow): number {
  let sum = 0
  let count = 0
  for (const corner of corners) {
    const value = (row as any)[`tyre_wear_${corner.key}`]
    if (Number.isFinite(value)) { sum += value; count++ }
  }
  return count ? sum / count : NaN
}

const averageMetrics: AnalyzeMetricDefinition[] = [
  metric({ id: 'wear-avg', group: 'Tyres', label: 'Average Tyre Wear', source: 'damage', defaultColor: '#FF780A', scaleKey: 'percent', min: 0, max: 100, unit: '%', getValue: averageWear, format: withUnit('%', 1), axisFormat: withUnit('%') }),
  metric({ id: 'life-avg', group: 'Tyres', label: 'Average Tyre Life', source: 'damage', defaultColor: '#73BF69', scaleKey: 'percent', min: 0, max: 100, unit: '%', getValue: (r: DamageRow) => 100 - averageWear(r), format: withUnit('%', 1), axisFormat: withUnit('%') }),
]

export const ANALYZE_METRICS = [...base, ...tyreMetrics, ...averageMetrics]
export const ANALYZE_METRIC_BY_ID = new Map(ANALYZE_METRICS.map(def => [def.id, def]))
export const ANALYZE_METRIC_CATEGORIES = ['Driving', 'Motion', 'Power', 'Tyres'] as const
export const ANALYZE_TYRE_CORNERS = corners.map(corner => ({ key: corner.key, label: corner.label }))
// Per-corner metric ids are `${idPrefix}-${corner.key}`. `shortLabel` names a
// combined card, whose four colour swatches leave little room for text.
export const ANALYZE_TYRE_ROWS = [
  { idPrefix: 'surface', label: 'Surface Temp', shortLabel: 'Surface', combinedColor: '#FF9830' },
  { idPrefix: 'inner', label: 'Inner Temp', shortLabel: 'Inner', combinedColor: '#B877DB' },
  { idPrefix: 'brake-temp', label: 'Brake Temp', shortLabel: 'Brake', combinedColor: '#F2495C' },
  { idPrefix: 'wear', label: 'Tyre Wear', shortLabel: 'T.Wear', combinedColor: '#8AB8FF' },
  { idPrefix: 'life', label: 'Tyre Life', shortLabel: 'T.Life', combinedColor: '#73BF69' },
] as const

// A combined series is one sidebar card (and one stacked panel) that plots the
// chosen corners of a tyre metric on a shared scale, in the corner colours. It
// drives the corners' own chart series, so it and those corners are mutually
// exclusive in a config: charting a row "combined" moves its corners onto it.
export interface AnalyzeCombinedMetric {
  id: AnalyzeMetricId
  label: string
  unit: string
  idPrefix: string
  /** The card's own colour: its title in the sidebar and its chart axis. */
  defaultColor: string
  memberIds: readonly AnalyzeMetricId[]
}

export const ANALYZE_COMBINED_METRICS: AnalyzeCombinedMetric[] = ANALYZE_TYRE_ROWS.map(row => ({
  id: `${row.idPrefix}-all`,
  label: `${row.label} · Combined`,
  unit: ANALYZE_METRIC_BY_ID.get(`${row.idPrefix}-${corners[0].key}`)!.unit,
  idPrefix: row.idPrefix,
  defaultColor: row.combinedColor,
  memberIds: corners.map(corner => `${row.idPrefix}-${corner.key}`),
}))
export const ANALYZE_COMBINED_BY_ID = new Map(ANALYZE_COMBINED_METRICS.map(def => [def.id, def]))

const CORNER_KEYS: readonly string[] = corners.map(corner => corner.key)

/** Valid corner keys in FL, FR, RL, RR order; a missing list means all four. */
export function sanitizeCorners(value: unknown): string[] {
  if (!Array.isArray(value)) return [...CORNER_KEYS]
  return CORNER_KEYS.filter(key => value.includes(key))
}

/** The metric definitions a series config draws: itself, its chosen corners, or none (delta). */
export function analyzeSeriesMemberIds(item: Pick<AnalyzeSeriesConfig, 'metricId' | 'corners'>): readonly AnalyzeMetricId[] {
  const combined = ANALYZE_COMBINED_BY_ID.get(item.metricId)
  if (combined) return (item.corners ?? CORNER_KEYS).map(key => `${combined.idPrefix}-${key}`)
  return ANALYZE_METRIC_BY_ID.has(item.metricId) ? [item.metricId] : []
}

/** Custom corner colours with anything that is not a known key and a hex colour dropped. */
export function sanitizeCornerColors(value: unknown): Partial<Record<string, string>> | undefined {
  if (!value || typeof value !== 'object') return undefined
  const entries = CORNER_KEYS.flatMap(key => {
    const color = (value as Record<string, unknown>)[key]
    return typeof color === 'string' && /^#[0-9a-f]{6}$/i.test(color) ? [[key, color] as const] : []
  })
  return entries.length ? Object.fromEntries(entries) : undefined
}

/** The corner key a combined card's member metric id stands for. */
export function analyzeCornerKeyOf(combinedId: AnalyzeMetricId, memberId: AnalyzeMetricId): string | undefined {
  const combined = ANALYZE_COMBINED_BY_ID.get(combinedId)
  return combined && memberId.startsWith(`${combined.idPrefix}-`) ? memberId.slice(combined.idPrefix.length + 1) : undefined
}

/** The colour a series draws one of its member lines in. */
export function analyzeSeriesLineColor(item: Pick<AnalyzeSeriesConfig, 'metricId' | 'color' | 'cornerColors'>, memberId: AnalyzeMetricId): string {
  const key = analyzeCornerKeyOf(item.metricId, memberId)
  if (key === undefined) return item.color
  return item.cornerColors?.[key] ?? ANALYZE_METRIC_BY_ID.get(memberId)?.defaultColor ?? item.color
}

/** Whether a series has anything to plot; a combined card can have no corners picked yet. */
export function analyzeSeriesHasLines(item: Pick<AnalyzeSeriesConfig, 'metricId' | 'corners'>): boolean {
  return item.metricId === 'delta' || analyzeSeriesMemberIds(item).length > 0
}

/** The definition whose scale and formatting a series' axis uses. */
export function analyzeSeriesScaleDef(metricId: AnalyzeMetricId): AnalyzeMetricDefinition | undefined {
  return ANALYZE_METRIC_BY_ID.get(ANALYZE_COMBINED_BY_ID.get(metricId)?.memberIds[0] ?? metricId)
}

/** Series ids that cannot be charted alongside `metricId`. */
export function analyzeSeriesConflicts(metricId: AnalyzeMetricId): readonly AnalyzeMetricId[] {
  const combined = ANALYZE_COMBINED_BY_ID.get(metricId)
  if (combined) return combined.memberIds
  const owner = ANALYZE_COMBINED_METRICS.find(def => def.memberIds.includes(metricId))
  return owner ? [owner.id] : []
}

export const DEFAULT_ANALYZE_CONFIG: AnalyzeConfig = {
  version: 9,
  collapsed: false,
  view: 'graph',
  individualGraphs: false,
  syncedTooltip: false,
  sectorBoundaries: false,
  sectorDelta: false,
  currentLabel: '',
  compareLabel: '',
  lapALabel: '',
  lapBLabel: '',
  mapCurrentColor: DEFAULT_MAP_CURRENT_COLOR,
  mapComparisonColor: DEFAULT_MAP_COMPARISON_COLOR,
  series: [
    ...['speed', 'rpm', 'ers'].map(metricId => ({
      metricId,
      color: ANALYZE_METRIC_BY_ID.get(metricId)!.defaultColor,
      visible: true,
      showYAxis: true,
    })),
    { metricId: 'delta', color: DEFAULT_DELTA_POSITIVE_COLOR, negativeColor: DEFAULT_DELTA_NEGATIVE_COLOR, visible: true, showYAxis: true },
  ],
}

type StoredAnalyzeConfig = Partial<Omit<AnalyzeConfig, 'version' | 'view'>> & {
  version?: number
  view?: 'graph' | 'charts' | 'split' | 'map'
}

export function sanitizeAnalyzeConfig(value: StoredAnalyzeConfig | null | undefined): AnalyzeConfig {
  const seen = new Set<string>()
  const defaultShowYAxis = (value as (Partial<AnalyzeConfig> & { showYAxis?: boolean }) | null | undefined)?.showYAxis !== false
  const inputSeries = Array.isArray(value?.series) ? value.series : null
  const hasSeries = inputSeries !== null
  const series: AnalyzeSeriesConfig[] = inputSeries ? inputSeries.flatMap<AnalyzeSeriesConfig>(item => {
    if (item?.metricId === 'delta' && !seen.has('delta')) {
      seen.add('delta')
      return [{
        metricId: 'delta',
        color: /^#[0-9a-f]{6}$/i.test(item.color ?? '') ? item.color : DEFAULT_DELTA_POSITIVE_COLOR,
        negativeColor: /^#[0-9a-f]{6}$/i.test(item.negativeColor ?? '') ? item.negativeColor : DEFAULT_DELTA_NEGATIVE_COLOR,
        visible: item.visible !== false,
        showYAxis: typeof item.showYAxis === 'boolean' ? item.showYAxis : defaultShowYAxis,
      }]
    }
    const combined = item && ANALYZE_COMBINED_BY_ID.get(item.metricId)
    const def = item && ANALYZE_METRIC_BY_ID.get(item.metricId)
    const id = combined?.id ?? def?.id
    if (!id || seen.has(id) || analyzeSeriesConflicts(id).some(other => seen.has(other))) return []
    seen.add(id)
    // Combined cards used to store a placeholder grey before they had a colour.
    const color = /^#[0-9a-f]{6}$/i.test(item.color ?? '') && !(combined && item.color.toLowerCase() === '#7c8098')
      ? item.color
      : (combined ?? def)!.defaultColor
    const showYAxis = typeof item.showYAxis === 'boolean' ? item.showYAxis : defaultShowYAxis
    return [combined
      ? { metricId: id, color, visible: item.visible !== false, showYAxis, corners: sanitizeCorners(item.corners), cornerColors: sanitizeCornerColors(item.cornerColors) }
      : { metricId: id, color, visible: item.visible !== false, showYAxis }]
  }) : []
  if (!seen.has('delta')) series.push({
    metricId: 'delta', color: DEFAULT_DELTA_POSITIVE_COLOR,
    negativeColor: DEFAULT_DELTA_NEGATIVE_COLOR, visible: true, showYAxis: true,
  })
  const sectorBoundaries = value?.sectorBoundaries === true
  const label = (candidate: unknown, previousDefault: string) =>
    typeof candidate === 'string' && candidate !== previousDefault ? candidate.slice(0, 40) : ''
  return {
    version: 9,
    collapsed: value?.collapsed === true,
    view: value?.view === 'map' || value?.view === 'split' ? value.view : 'graph',
    individualGraphs: typeof value?.individualGraphs === 'boolean'
      ? value.individualGraphs
      : value?.view === 'charts',
    syncedTooltip: value?.syncedTooltip === true,
    sectorBoundaries,
    sectorDelta: value?.sectorDelta === true,
    currentLabel: label(value?.currentLabel, DEFAULT_CURRENT_LABEL),
    compareLabel: label(value?.compareLabel, DEFAULT_COMPARE_LABEL),
    lapALabel: label(value?.lapALabel, DEFAULT_LAP_A_LABEL),
    lapBLabel: label(value?.lapBLabel, DEFAULT_LAP_B_LABEL),
    mapCurrentColor: /^#[0-9a-f]{6}$/i.test(value?.mapCurrentColor ?? '') ? value!.mapCurrentColor! : DEFAULT_MAP_CURRENT_COLOR,
    mapComparisonColor: /^#[0-9a-f]{6}$/i.test(value?.mapComparisonColor ?? '') ? value!.mapComparisonColor! : DEFAULT_MAP_COMPARISON_COLOR,
    series: hasSeries ? series : DEFAULT_ANALYZE_CONFIG.series.map(item => ({ ...item })),
  }
}
