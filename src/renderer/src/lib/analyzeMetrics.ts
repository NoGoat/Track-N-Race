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
  visible: boolean
}

export interface AnalyzeConfig {
  version: 1
  collapsed: boolean
  showYAxis: boolean
  series: AnalyzeSeriesConfig[]
}

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

export const ANALYZE_METRICS = [...base, ...tyreMetrics]
export const ANALYZE_METRIC_BY_ID = new Map(ANALYZE_METRICS.map(def => [def.id, def]))

export const DEFAULT_ANALYZE_CONFIG: AnalyzeConfig = {
  version: 1,
  collapsed: false,
  showYAxis: true,
  series: ['speed', 'rpm', 'ers'].map(metricId => ({
    metricId,
    color: ANALYZE_METRIC_BY_ID.get(metricId)!.defaultColor,
    visible: true,
  })),
}

export function sanitizeAnalyzeConfig(value: Partial<AnalyzeConfig> | null | undefined): AnalyzeConfig {
  const seen = new Set<string>()
  const hasSeries = Array.isArray(value?.series)
  const series = hasSeries ? value!.series.flatMap(item => {
    const def = item && ANALYZE_METRIC_BY_ID.get(item.metricId)
    if (!def || seen.has(def.id)) return []
    seen.add(def.id)
    const color = /^#[0-9a-f]{6}$/i.test(item.color ?? '') ? item.color : def.defaultColor
    return [{ metricId: def.id, color, visible: item.visible !== false }]
  }) : []
  return {
    version: 1,
    collapsed: value?.collapsed === true,
    showYAxis: value?.showYAxis !== false,
    series: hasSeries ? series : DEFAULT_ANALYZE_CONFIG.series.map(item => ({ ...item })),
  }
}
