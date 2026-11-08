// Paints the Analysis map from libtnrp's map calculation (tnrp/AnalysisMap.h,
// via window.analysisBridge.map): the library decides which lap is faster
// where and how hard each car deployed; this file only chooses colours.

/** Colours the track by lap position. `colorAt` takes the fraction of the lap
 *  from the start/finish line (0–1) and returns a CSS colour, or null to leave
 *  that stretch of track undrawn. */
export interface TrackPaint {
  colorAt(fraction: number): string | null
  /** The colours carry alpha, so overlapping stroke caps would show. */
  translucent: boolean
}

// Colour steps; equal neighbours share one stroke.
const ERS_STEPS = 20
// ERS Usage runs from no deployment to the cap: navy to aqua-mint on the dark
// theme (through steel blue and teal), ocean blue to coral on the light one
// (through lavender and rose). Blending in OKLCH the short way round keeps the
// middle values coloured rather than grey.
export const ERS_RAMP_DARK = ['#3B528B', '#7CF5D0'] as const
export const ERS_RAMP_LIGHT = ['#3D6FB6', '#FF7A59'] as const

type Oklab = [number, number, number]

function hexToOklab(hex: string): Oklab {
  const [r, g, b] = [1, 3, 5].map(i => {
    const c = parseInt(hex.slice(i, i + 2), 16) / 255
    return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4
  })
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
  return [
    0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
    1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
    0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
  ]
}

function oklabToHex([L, a, b]: Oklab): string {
  const l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3
  const m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3
  const s = (L - 0.0894841775 * a - 1.2914855480 * b) ** 3
  return '#' + [
    4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
    -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
  ].map(c => {
    const v = c <= 0.0031308 ? 12.92 * c : 1.055 * c ** (1 / 2.4) - 0.055
    return Math.round(Math.min(1, Math.max(0, v)) * 255).toString(16).padStart(2, '0')
  }).join('')
}

/** steps + 1 colours from the ramp's start to its end, blended in OKLCH
 *  along the shorter hue arc, as CSS `in oklch` gradients are. */
function rampColors([from, to]: readonly [string, string], steps = ERS_STEPS): string[] {
  const [l0, a0, b0] = hexToOklab(from)
  const [l1, a1, b1] = hexToOklab(to)
  const c0 = Math.hypot(a0, b0)
  const c1 = Math.hypot(a1, b1)
  const h0 = Math.atan2(b0, a0)
  const dh = ((Math.atan2(b1, a1) - h0 + 3 * Math.PI) % (2 * Math.PI)) - Math.PI
  return Array.from({ length: steps + 1 }, (_, step) => {
    const t = step / steps
    const c = c0 + (c1 - c0) * t
    const h = h0 + dh * t
    return oklabToHex([l0 + (l1 - l0) * t, c * Math.cos(h), c * Math.sin(h)])
  })
}

const ERS_COLORS_DARK = rampColors(ERS_RAMP_DARK)
const ERS_COLORS_LIGHT = rampColors(ERS_RAMP_LIGHT)

// ERS Harvest has ramps of its own so it never looks like ERS Usage: wine to
// pink on the dark theme, brown to amber on the light one.
export const HARVEST_RAMP_DARK = ['#5E2A4F', '#FF8FC7'] as const
export const HARVEST_RAMP_LIGHT = ['#4A3B2A', '#FFB224'] as const
const HARVEST_COLORS_DARK = rampColors(HARVEST_RAMP_DARK)
const HARVEST_COLORS_LIGHT = rampColors(HARVEST_RAMP_LIGHT)
// Superclipping is drawn in a colour of its own at full strength, whatever its
// power: it harvests a small fraction of what braking does. Periwinkle sits
// apart from both harvest ramps (wine to pink, brown to amber).
export const SUPERCLIP_COLOR_DARK = '#8C9CF0'
export const SUPERCLIP_COLOR_LIGHT = '#8C9CF0'

// Speed runs from the slowest to the fastest speed on show: raspberry to
// periwinkle on the dark theme and olive to periwinkle on the light one. Finer
// steps than ERS keep a few km/h between neighbouring colours.
const SPEED_STEPS = 50
export const SPEED_RAMP_DARK = ['#C8577F', '#767AC9'] as const
export const SPEED_RAMP_LIGHT = ['#7E7F3C', '#767AC9'] as const
const SPEED_COLORS_DARK = rampColors(SPEED_RAMP_DARK, SPEED_STEPS)
const SPEED_COLORS_LIGHT = rampColors(SPEED_RAMP_LIGHT, SPEED_STEPS)

/** One lap's speed from libtnrp. */
export interface SpeedResult {
  lapNum: number
  binM: number
  trackLengthM: number
  /** Per bin: the time-weighted mean speed (km/h). */
  speedKph: number[]
  /** The lap's slowest and fastest samples (km/h). */
  minKph: number
  maxKph: number
  /** Each corner in the lap's braking zones, with its slowest speed. */
  apexes: { distanceM: number; speedKph: number; side: number }[]
}

/** One lap's ERS Usage from libtnrp. */
export interface ErsUsageResult {
  lapNum: number
  binM: number
  trackLengthM: number
  /** Per bin: MGU-K output as a fraction (0-1) of the car's peak for the grip there. */
  level: number[]
  /** Every ERS field read zero: the driver's telemetry is Restricted. */
  restricted: boolean
  usesNormal: boolean
  usesReduced: boolean
  /** The car's highest MGU-K output (kW) in its recording at normal / reduced grip. */
  normalPeakKw: number
  reducedPeakKw: number
}

/** One lap's ERS Harvest from libtnrp. */
export interface ErsHarvestResult {
  lapNum: number
  binM: number
  trackLengthM: number
  /** Per bin: harvest power as a fraction (0-1) of the car's peak harvest power. */
  level: number[]
  /** Per bin: 1 where the car was superclipping (flat out, not deploying, harvesting). */
  superclip: number[]
  /** Every ERS field read zero: the driver's telemetry is Restricted. */
  restricted: boolean
  /** The car's highest harvest power (kW) in its recording. */
  peakKw: number
}

/** Something worth pointing out about a braking zone, as facts to word.
 *  `lap` 0 is the current lap, 1 the comparison lap. */
export interface BrakingObservation {
  kind: 'quicker_despite_coast' | 'apex_speed' | 'later_but_slower' | 'more_harvest' | 'low_battery'
    | 'wheelspin' | 'earlier_throttle_spin' | 'earlier_throttle' | 'faster_to_full' | 'lift_only'
  lap: 0 | 1
  value: number
  other: number
}

/** Two laps' matched braking zone from libtnrp. -1 marks an unknown figure. */
export interface BrakingPairResult {
  current: number
  comparison: number
  /** Metres: positive where the current lap brakes later. Only meaningful
   *  when sameBrakingPoint: in a complex one lap may first brake a corner later. */
  gapM: number
  sameBrakingPoint: boolean
  /** The zone both laps are timed over. */
  fromM: number
  toM: number
  currentTimeS: number
  comparisonTimeS: number
  /** Where the zone splits into braking (fromM to splitM) and acceleration
   *  (splitM to toM), halfway between the laps' slowest points; -1 unsplit.
   *  Each lap's seconds over each half, -1 when unsplit. */
  splitM: number
  currentBrakingS: number
  comparisonBrakingS: number
  currentAccelerationS: number
  comparisonAccelerationS: number
  /** ERS harvested across the zone (kJ) and the store entering it (MJ). */
  currentHarvestKj: number
  comparisonHarvestKj: number
  currentStoreMj: number
  comparisonStoreMj: number
  observations: BrakingObservation[]
}

/** One braking zone of one lap from libtnrp. */
export interface BrakingPointResult {
  /** Taken on a lift where the other lap brakes: distanceM is where the
   *  throttle came off and the coast is the whole lift. */
  liftOnly: boolean
  /** Lap distance (m) where the brake goes on, and the speed there. */
  distanceM: number
  speedKph: number
  /** Off both pedals right before braking; zero when straight from throttle to brake. */
  coastM: number
  coastS: number
  /** Brake on to brake off for the zone's first stop; -1 when unknown. */
  brakingDistanceM: number
  /** The zone's slowest corner, and where. */
  minSpeedKph: number
  minSpeedM: number
  /** Each corner in the zone, found from the steering so flat-out corners
   *  count: where half its steering is done, the slowest speed through it,
   *  and which way it turns (-1 left, 1 right, 0 when found from speed alone). */
  apexes: { distanceM: number; speedKph: number; side: number }[]
  /** Where the zone ends: full throttle, else the next braking point. */
  endM: number
  /** Corner exit; -1 where it did not happen before the next braking zone. */
  throttleOnM: number
  /** Seconds from the slowest point back to the throttle. */
  throttleOnS: number
  fullThrottleM: number
  /** Seconds from back on the throttle to flat out. */
  fullThrottleS: number
  /** Distance from back on the throttle to flat out; -1 when unknown. */
  throttleToFullM: number
  /** Rear wheelspin on the exit, from engine speed running ahead of road speed. */
  wheelspin: boolean
  wheelspinPct: number
  wheelspinS: number
}

/** One lap's driver inputs from libtnrp. */
export interface DriverInputsResult {
  lapNum: number
  binM: number
  trackLengthM: number
  /** Per bin: 0 coasting, 1 partial throttle, 2 full throttle, 3 braking. */
  state: number[]
}

export interface AnalysisMapResult {
  mode: 'dominance' | 'ers' | 'harvest' | 'inputs' | 'speed' | 'braking'
  dominance: {
    miniSectorM: number
    trackLengthM: number
    /** Per mini-sector: 0 = current lap faster, 1 = comparison lap, -1 = level. */
    faster: number[]
  }
  current: ErsUsageResult
  comparison: ErsUsageResult
  currentInputs: DriverInputsResult
  comparisonInputs: DriverInputsResult
  currentSpeed: SpeedResult
  comparisonSpeed: SpeedResult
  /** One km/h scale for both laps' speed maps. */
  speedScaleMinKph: number
  speedScaleMaxKph: number
  currentHarvest: ErsHarvestResult
  comparisonHarvest: ErsHarvestResult
  hasCurrent: boolean
  hasComparison: boolean
  normalPeakMismatch: boolean
  reducedPeakMismatch: boolean
  harvestPeakMismatch: boolean
  braking: {
    trackLengthM: number
    /** Each lap's braking zones, in lap order. */
    current: BrakingPointResult[]
    comparison: BrakingPointResult[]
    /** Matched zones; gapM > 0 where the current lap brakes later. Both laps
     *  are timed from fromM (first on the brakes) to toM (both flat out again). */
    pairs: BrakingPairResult[]
  }
}

export function parseAnalysisMapResult(raw: unknown): AnalysisMapResult | null {
  const result = raw as Partial<AnalysisMapResult> | null
  if (!result || !['dominance', 'ers', 'harvest', 'inputs', 'speed', 'braking'].includes(result.mode ?? '') || !result.braking ||
      !result.dominance || !result.current || !result.comparison ||
      !result.currentInputs || !result.comparisonInputs ||
      !result.currentSpeed || !result.comparisonSpeed ||
      !result.currentHarvest || !result.comparisonHarvest) return null
  return result as AnalysisMapResult
}

export function dominancePaint(
  result: AnalysisMapResult,
  currentColor: string,
  comparisonColor: string,
  isDark: boolean,
): TrackPaint | null {
  const { faster } = result.dominance
  if (faster.length === 0) return null
  const level = isDark ? '#ffffff' : '#000000'
  const colors = faster.map(value => value === 0 ? currentColor : value === 1 ? comparisonColor : level)
  return {
    translucent: false,
    colorAt: fraction => colors[Math.min(colors.length - 1, Math.max(0, Math.floor(fraction * colors.length)))],
  }
}

/** ERS Harvest: the harvest ramp from where the car harvested nothing to its
 *  peak harvest power. */
export function harvestPaint(harvest: ErsHarvestResult, isDark: boolean): TrackPaint | null {
  const paint = levelPaint(harvest, isDark ? HARVEST_COLORS_DARK : HARVEST_COLORS_LIGHT)
  if (!paint || !harvest.superclip.some(Boolean)) return paint
  const superclip = isDark ? SUPERCLIP_COLOR_DARK : SUPERCLIP_COLOR_LIGHT
  const bins = harvest.superclip.length
  return {
    translucent: false,
    colorAt: fraction => {
      const bin = Math.min(bins - 1, Math.max(0, Math.floor(fraction * harvest.trackLengthM / harvest.binM)))
      return harvest.superclip[bin] ? superclip : paint.colorAt(fraction)
    },
  }
}

export function ersPaint(usage: ErsUsageResult, isDark: boolean): TrackPaint | null {
  return levelPaint(usage, isDark ? ERS_COLORS_DARK : ERS_COLORS_LIGHT)
}

/** Per-bin levels (0-1) along a ramp; null for a Restricted car. */
function levelPaint(
  usage: { restricted: boolean; level: number[]; binM: number; trackLengthM: number },
  ramp: readonly string[],
): TrackPaint | null {
  if (usage.restricted || usage.level.length === 0 || !(usage.binM > 0)) return null
  const colors = usage.level.map(level => ramp[Math.round(Math.min(1, Math.max(0, level)) * ERS_STEPS)])
  const length = usage.trackLengthM
  return {
    translucent: false,
    colorAt: fraction => colors[Math.min(colors.length - 1, Math.max(0, Math.floor(fraction * length / usage.binM)))],
  }
}

/** Driver inputs: coasting, partial throttle, full throttle, braking. Red,
 *  amber and green read as brake, part throttle and flat out at a glance. */
export const INPUT_STATE_LABELS = ['Coasting', 'Partial', 'Full throttle', 'Braking'] as const
export const INPUT_COLORS_DARK = ['#8B8D98', '#FFB224', '#30A46C', '#E5484D'] as const
export const INPUT_COLORS_LIGHT = ['#80838D', '#C77700', '#218358', '#CE2C31'] as const

export function inputsPaint(inputs: DriverInputsResult, isDark: boolean): TrackPaint | null {
  if (inputs.state.length === 0 || !(inputs.binM > 0) || !(inputs.trackLengthM > 0)) return null
  const palette = isDark ? INPUT_COLORS_DARK : INPUT_COLORS_LIGHT
  const colors = inputs.state.map(state => palette[state] ?? palette[0])
  const length = inputs.trackLengthM
  return {
    translucent: false,
    colorAt: fraction => colors[Math.min(colors.length - 1, Math.max(0, Math.floor(fraction * length / inputs.binM)))],
  }
}

/** Speed along the speed ramp, from `minKph` (the slowest on show) to `maxKph`. */
export function speedPaint(speed: SpeedResult, minKph: number, maxKph: number, isDark: boolean): TrackPaint | null {
  if (speed.speedKph.length === 0 || !(speed.binM > 0) || !(speed.trackLengthM > 0)) return null
  const ramp = isDark ? SPEED_COLORS_DARK : SPEED_COLORS_LIGHT
  const span = maxKph - minKph
  const colors = speed.speedKph.map(kph =>
    ramp[span > 0 ? Math.round(Math.min(1, Math.max(0, (kph - minKph) / span)) * SPEED_STEPS) : SPEED_STEPS])
  const length = speed.trackLengthM
  return {
    translucent: false,
    colorAt: fraction => colors[Math.min(colors.length - 1, Math.max(0, Math.floor(fraction * length / speed.binM)))],
  }
}
