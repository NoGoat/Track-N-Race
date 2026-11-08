import { memo, type ReactNode, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { AlertTriangle } from 'lucide-react'
import PlaybackBar from '../app/components/PlaybackBar'
import { setAnalyzeCursorSource } from '../lib/analyzeCursor'
import {
  dominancePaint, ersPaint, harvestPaint, inputsPaint, speedPaint,
  ERS_RAMP_DARK, ERS_RAMP_LIGHT, HARVEST_RAMP_DARK, HARVEST_RAMP_LIGHT, INPUT_COLORS_DARK,
  SUPERCLIP_COLOR_DARK, SUPERCLIP_COLOR_LIGHT, INPUT_COLORS_LIGHT, INPUT_STATE_LABELS,
  SPEED_RAMP_DARK, SPEED_RAMP_LIGHT,
  type AnalysisMapResult, type BrakingObservation, type BrakingPairResult, type BrakingPointResult,
  type ErsHarvestResult, type ErsUsageResult, type SpeedResult, type TrackPaint,
} from '../lib/analyzeMapPaint'
import { useSize } from '../hooks/useSize'
import type { AnalyzeMapColoring } from '../lib/analyzeMetrics'
import { getPlaybackCursorTime } from '../lib/playbackCursor'
import { useModalPresence } from '../lib/useModalPresence'
import { useTelemetryStore } from '../stores/telemetryStore'
import type { AnalyzeLapData, PlayerPositionPoint } from '../types'
import TrackMap, { type TrackMapMarker, type TrackMark } from './TrackMap'
import { useTrackMapStore } from '../stores/trackMapStore'
import AnalyzeInputComparison from './AnalyzeInputComparison'

interface Props {
  comparison: AnalyzeLapData | null
  comparisonColor: string
  comparisonLabel: string
  compatibleCircuit: boolean
  current: AnalyzeLapData | null
  currentColor: string
  currentLabel: string
  fixedMode: boolean
  isDark: boolean
  mapColoring: AnalyzeMapColoring
  /** libtnrp's colouring for these laps in `mapColoring`; null until it arrives. */
  mapResult: AnalysisMapResult | null
  /** A comparison lap is chosen, whether or not its data has arrived yet. */
  comparisonSelected: boolean
  /** libtnrp's braking points for these laps; null when they are switched off. */
  brakingResult: AnalysisMapResult | null
  showInputPanel: boolean
  mapDimmed: boolean
  reduceAnimations: boolean
  sectorColors: boolean
  trackId: number | null
  focus: AnalyzeMapFocus | null
}

export interface AnalyzeMapFocus {
  id: number
  elapsedSeconds: number
}

interface LocalClock {
  cursor: number
  playing: boolean
  speed: number
  startedAt: number
  total: number
}

interface BarState {
  currentTime: number
  isPlaying: boolean
  progressPct: number
  speed: number
  totalTime: number
}

function lapDuration(lap: AnalyzeLapData | null): number {
  return lap ? Math.max(0, lap.endSessionTime - lap.startSessionTime) : 0
}

function readClock(clock: LocalClock, now = performance.now()): number {
  if (!clock.playing) return clock.cursor
  return Math.min(clock.total, clock.cursor + (now - clock.startedAt) / 1000 * clock.speed)
}

function writeMarkerAt(
  marker: TrackMapMarker,
  points: readonly PlayerPositionPoint[],
  target: number,
): boolean {
  if (points.length === 0) return false
  if (target <= points[0].session_time) {
    marker.x = points[0].x
    marker.z = points[0].z
    return true
  }
  const last = points[points.length - 1]
  if (target >= last.session_time) {
    marker.x = last.x
    marker.z = last.z
    return true
  }
  let lo = 0
  let hi = points.length - 1
  while (lo + 1 < hi) {
    const mid = (lo + hi) >>> 1
    if (points[mid].session_time <= target) lo = mid
    else hi = mid
  }
  const before = points[lo]
  const after = points[hi]
  const span = after.session_time - before.session_time
  const ratio = span > 0 ? (target - before.session_time) / span : 0
  marker.x = before.x + (after.x - before.x) * ratio
  marker.z = before.z + (after.z - before.z) * ratio
  return true
}

const formatKw = (kw: number) => `${Number.isInteger(kw) ? kw : kw.toFixed(1)} kW`

/** ERS Usage scale, in the map's bottom-right corner: full colour is the
 *  car's peak for the grip the lap was driven in, both when it saw both. */
function ErsScale({ usage, isDark }: { usage: ErsUsageResult | null; isDark: boolean }) {
  if (!usage || (!usage.restricted && !usage.usesNormal && !usage.usesReduced)) return null
  return <div className="pointer-events-none absolute bottom-2 right-2 z-10 flex items-center gap-1.5 text-[9px] font-mono text-[var(--text-secondary)]">
    {usage.restricted
      ? <span>No ERS data</span>
      : <>
          <span>0</span>
          <span className="h-1.5 w-16 rounded-full" style={{
            background: `linear-gradient(in oklch to right, ${(isDark ? ERS_RAMP_DARK : ERS_RAMP_LIGHT).join(', ')})`,
          }} />
          {usage.usesNormal && <span className="text-[var(--text-primary)]">{formatKw(usage.normalPeakKw)}</span>}
          {usage.usesReduced && <span>
            {usage.usesNormal ? '· ' : ''}<span className="text-[var(--text-primary)]">{formatKw(usage.reducedPeakKw)}</span> reduced grip
          </span>}
        </>}
  </div>
}

/** ERS Harvest scale: full colour is the car's peak harvest power. */
function HarvestScale({ harvest, isDark }: { harvest: ErsHarvestResult | null; isDark: boolean }) {
  if (!harvest || (!harvest.restricted && harvest.level.length === 0)) return null
  return <div className="pointer-events-none absolute bottom-2 right-2 z-10 flex items-center gap-1.5 text-[9px] font-mono text-[var(--text-secondary)]">
    {harvest.restricted
      ? <span>No ERS data</span>
      : <>
          <span>0</span>
          <span className="h-1.5 w-16 rounded-full" style={{
            background: `linear-gradient(in oklch to right, ${(isDark ? HARVEST_RAMP_DARK : HARVEST_RAMP_LIGHT).join(', ')})`,
          }} />
          <span className="text-[var(--text-primary)]">{formatKw(Math.round(harvest.peakKw))}</span>
          {harvest.superclip.some(Boolean) && <span className="ml-1 flex items-center gap-1">
            <span className="h-1.5 w-3 rounded-full" style={{ background: isDark ? SUPERCLIP_COLOR_DARK : SUPERCLIP_COLOR_LIGHT }} />
            Superclip
          </span>}
        </>}
  </div>
}

/** Speed scale: the ramp runs from the slowest to the fastest speed on show,
 *  the same on both laps' maps. */
function SpeedScale({ speed, minKph, maxKph, isDark }: { speed: SpeedResult | null; minKph: number; maxKph: number; isDark: boolean }) {
  if (!speed || speed.speedKph.length === 0) return null
  return <div className="pointer-events-none absolute bottom-2 right-2 z-10 flex items-center gap-1.5 text-[9px] font-mono text-[var(--text-secondary)]">
    <span>{Math.round(minKph)}</span>
    <span className="h-1.5 w-16 rounded-full" style={{
      background: `linear-gradient(in oklch to right, ${(isDark ? SPEED_RAMP_DARK : SPEED_RAMP_LIGHT).join(', ')})`,
    }} />
    <span className="text-[var(--text-primary)]">{Math.round(maxKph)} km/h</span>
  </div>
}

// Each corner's apex is marked this far either side of it on the Speed map.
const APEX_MARK_M = 6

/** Marks each corner's apex on a lap's speed map, with its slowest speed on hover. */
function apexMarks(speed: SpeedResult | null, isDark: boolean): TrackMark[] {
  if (!speed || !(speed.trackLengthM > 0)) return []
  const length = speed.trackLengthM
  const color = isDark ? '#F4F5F8' : '#14151A'
  return speed.apexes.map((apex, index) => ({
    fromFraction: Math.max(0, apex.distanceM - APEX_MARK_M) / length,
    toFraction: Math.min(length, apex.distanceM + APEX_MARK_M) / length,
    color,
    tooltip: <div className="flex flex-col gap-0.5 whitespace-nowrap tabular-nums">
      <span className="text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">
        Apex {index + 1}{sideText(apex.side)} · {formatM(apex.distanceM)}
      </span>
      <span className="text-[12px] font-bold">{Math.round(apex.speedKph)} km/h</span>
    </div>,
  }))
}

const formatM = (m: number) => `${Math.round(m).toLocaleString()} m`
// Laps this close through a braking zone count as level.
const BRAKING_LEVEL_S = 0.01
// A zone's braking half is drawn at this opacity against its acceleration
// half; a single lap's zone splits at its slowest point when each half is at
// least PHASE_MIN_M long.
const BRAKING_HALF_OPACITY = 0.45
const PHASE_MIN_M = 30
/** Which half of a braking zone: up to the slowest point, or after it. */
type BrakingPhase = 'braking' | 'acceleration' | null

/** Words one of libtnrp's braking zone observations with the laps' names. */
function brakingObservationText(observation: BrakingObservation, labels: [string, string]): string {
  const lap = labels[observation.lap]
  const other = labels[1 - observation.lap]
  switch (observation.kind) {
    case 'quicker_despite_coast':
      return `${lap} is ${observation.value.toFixed(3)} s quicker despite lifting ${observation.other.toFixed(1)} s earlier`
    case 'apex_speed':
      return `${lap} carries ${Math.round(observation.value - observation.other)} km/h more through the apex (${Math.round(observation.value)} vs ${Math.round(observation.other)})`
    case 'later_but_slower':
      return `${lap} brakes ${Math.round(observation.value)} m later but is ${observation.other.toFixed(3)} s slower`
    case 'more_harvest':
      return `${lap} harvests ${Math.round(observation.value - observation.other)} kJ more than ${other}`
    case 'low_battery':
      return `${lap} starts the zone with a near-empty battery (${observation.value.toFixed(2)} MJ)`
    case 'wheelspin':
      return `${lap} spins the rears on exit (${Math.round(observation.value)}% for ${observation.other.toFixed(1)} s)`
    case 'earlier_throttle_spin':
      return `${lap} is back on throttle ${Math.round(observation.value)} m earlier but spins the rears (${Math.round(observation.other)}%)`
    case 'earlier_throttle':
      return `${lap} is back on throttle ${Math.round(observation.value)} m earlier than ${other}`
    case 'faster_to_full':
      return `${lap} is flat out ${Math.round(observation.value)} m sooner than ${other}`
    case 'lift_only':
      return `${lap} takes it on a lift where ${other} brakes`
  }
}

// Observations about the corner exit; the rest are about the braking.
const EXIT_OBSERVATIONS = new Set(['wheelspin', 'earlier_throttle_spin', 'earlier_throttle', 'faster_to_full'])

/** One half of a braking zone on hover, head to head: a row per measure with
 *  each lap's value either side and the better one in bold in its lap's
 *  colour, then libtnrp's observations. The braking half covers the brake,
 *  entry and corners up to the slowest point; the acceleration half the
 *  corners after it and the exit. A zone that is not split shows both. One lap
 *  on its own shows just its column. */
function BrakingTooltip({ zone, phase, fromM, splitM, toM, current, comparison, pair, currentLabel, comparisonLabel, currentColor, comparisonColor }: {
  zone: number
  /** The half under the pointer; null for a zone that is not split. */
  phase: BrakingPhase
  fromM: number
  splitM: number
  toM: number
  current: BrakingPointResult | undefined
  comparison: BrakingPointResult | undefined
  pair: BrakingPairResult | undefined
  currentLabel: string
  comparisonLabel: string
  currentColor: string
  comparisonColor: string
}) {
  const braking = phase !== 'acceleration'
  const acceleration = phase !== 'braking'
  const both = current !== undefined && comparison !== undefined
  // Each lap's time over this half, or over the whole zone when unsplit.
  const times = !pair ? null
    : phase === 'braking' ? [pair.currentBrakingS, pair.comparisonBrakingS]
    : phase === 'acceleration' ? [pair.currentAccelerationS, pair.comparisonAccelerationS]
    : [pair.currentTimeS, pair.comparisonTimeS]
  const timed = times !== null && times[0] >= 0 && times[1] >= 0
  const quicker = timed ? times[1] - times[0] : 0
  const verdict = !timed ? null
    : Math.abs(quicker) < BRAKING_LEVEL_S ? { text: `Level through the ${braking && acceleration ? 'zone' : 'half'}`, color: undefined }
    : quicker > 0
      ? { text: `${currentLabel} ${Math.abs(quicker).toFixed(3)} s quicker`, color: currentColor }
      : { text: `${comparisonLabel} ${Math.abs(quicker).toFixed(3)} s quicker`, color: comparisonColor }

  // [measure, current value, comparison value, which lap did better, and
  // optionally which laps' values are warnings: an exit that spun]; a value
  // is null where that lap has none, and `better` is null where neither wins.
  type Better = 0 | 1 | null
  type Row = [string, string | null, string | null, Better, [boolean, boolean]?]
  const pick = (a: number, b: number, higher: boolean, margin = 0): Better =>
    Math.abs(a - b) <= margin ? null : (a > b) === higher ? 0 : 1
  const known = (value: number | undefined) => value !== undefined && value >= 0
  // Braking points compare only when both laps first brake for the same
  // corner; one lap can take it on a lift.
  const sameBrake = both && (pair?.sameBrakingPoint ?? true)
  // The corners in this half: up to the split for braking, after it for
  // acceleration.
  const corners = alignCorners(current?.apexes ?? [], comparison?.apexes ?? []).filter(([a, b]) => {
    const at = (a ?? b)!.distanceM
    return splitM < 0 || (braking && at <= splitM) || (acceleration && at > splitM)
  })
  const rows: Row[] = [
    ...(braking ? [
      ['Brakes at', current ? brakeAtText(current) : null, comparison ? brakeAtText(comparison) : null,
        both && sameBrake ? pick(current.distanceM, comparison.distanceM, true, 0.5) : null],
      ['Braking', current ? metresText(current.brakingDistanceM) : null, comparison ? metresText(comparison.brakingDistanceM) : null,
        both && sameBrake && current.brakingDistanceM >= 0 && comparison.brakingDistanceM >= 0
          ? pick(current.brakingDistanceM, comparison.brakingDistanceM, false, 0.5) : null],
      ['Entry', current ? `${Math.round(current.speedKph)} km/h` : null, comparison ? `${Math.round(comparison.speedKph)} km/h` : null,
        both && sameBrake ? pick(current.speedKph, comparison.speedKph, true, 0.5) : null],
      ['Coast', current ? coastText(current) : null, comparison ? coastText(comparison) : null, null],
    ] satisfies Row[] : []),
    // One apex row per corner, flat-out ones included.
    ...corners.map(([a, b], index): Row => [
      `${corners.length > 1 ? `Apex ${index + 1}` : 'Apex'}${sideText((a ?? b)!.side)}`,
      current ? (a ? `${Math.round(a.speedKph)} km/h` : '—') : null,
      comparison ? (b ? `${Math.round(b.speedKph)} km/h` : '—') : null,
      a && b ? pick(a.speedKph, b.speedKph, true, 0.5) : null,
    ]),
    ...(braking ? [
      ['ERS', known(pair?.currentHarvestKj) ? `+${Math.round(pair!.currentHarvestKj).toLocaleString()} kJ` : null,
        known(pair?.comparisonHarvestKj) ? `+${Math.round(pair!.comparisonHarvestKj).toLocaleString()} kJ` : null,
        pair && known(pair.currentHarvestKj) && known(pair.comparisonHarvestKj)
          ? pick(pair.currentHarvestKj, pair.comparisonHarvestKj, true, 0.5) : null],
    ] satisfies Row[] : []),
    // Corner exit. Back on the throttle earlier wins unless it spins the
    // rears, which hands it to the other lap; flat out sooner only counts
    // without wheelspin.
    ...(acceleration ? [
      ['Throttle on', current ? metresText(current.throttleOnM) : null,
        comparison ? metresText(comparison.throttleOnM) : null,
        both ? exitBetter(current.throttleOnM, comparison.throttleOnM, current.wheelspin, comparison.wheelspin) : null,
        [!!current?.wheelspin && current.throttleOnM >= 0, !!comparison?.wheelspin && comparison.throttleOnM >= 0]],
      ['Full throttle', current ? metresText(current.fullThrottleM) : null,
        comparison ? metresText(comparison.fullThrottleM) : null,
        both ? exitBetter(current.fullThrottleM, comparison.fullThrottleM, current.wheelspin, comparison.wheelspin) : null],
      // Back on the throttle to flat out: shorter is better, unless it spun.
      ['0 → 100%', current ? metresText(current.throttleToFullM) : null,
        comparison ? metresText(comparison.throttleToFullM) : null,
        both ? exitBetter(current.throttleToFullM, comparison.throttleToFullM, current.wheelspin, comparison.wheelspin) : null],
      ['Wheelspin', current ? spinText(current) : null, comparison ? spinText(comparison) : null,
        both && (current.wheelspin || comparison.wheelspin)
          ? current.wheelspin && comparison.wheelspin ? pick(current.wheelspinPct, comparison.wheelspinPct, false, 0.5)
            : current.wheelspin ? 1 : 0
          : null],
    ] satisfies Row[] : []),
    ['Time', timed ? `${times[0].toFixed(3)} s` : null, timed ? `${times[1].toFixed(3)} s` : null,
      timed ? pick(times[0], times[1], false, BRAKING_LEVEL_S / 2) : null],
  ]
  const shown = rows.filter(([, a, b]) => a !== null || b !== null)
  const observations = (pair?.observations ?? [])
    .filter(observation => EXIT_OBSERVATIONS.has(observation.kind) ? acceleration : braking)
    .map(observation => brakingObservationText(observation, [currentLabel, comparisonLabel]))
  const [shownFrom, shownTo] = phase === 'braking' ? [fromM, splitM] : phase === 'acceleration' ? [splitM, toM] : [fromM, toM]
  const lapHead = (label: string, color: string, align: 'left' | 'right') => (
    <th className={`pb-1 text-[9px] font-bold ${align === 'left' ? 'text-left' : 'text-right'}`} style={{ color }}>
      {label}
    </th>
  )
  // A warning (an exit that spun) shows in the warning colour instead of the lap's.
  const cell = (value: string | null, wins: boolean, warn: boolean, color: string, align: 'left' | 'right') => (
    <td className={`border-t border-[var(--border)] py-[3px] ${align === 'left' ? 'text-left' : 'text-right'} ${wins || warn ? 'font-bold' : ''} ${warn ? 'text-amber-500' : ''}`}
      style={wins && !warn ? { color } : undefined}>{value ?? '—'}</td>
  )

  return <div className="flex w-[270px] flex-col gap-2 whitespace-nowrap tabular-nums">
    <div className="flex items-baseline justify-between gap-3 text-[9px] uppercase tracking-widest text-[var(--text-secondary)]">
      <span>{phase === 'acceleration' ? 'Acceleration' : 'Braking'} zone {zone}{corners.length > 1 ? ` · ${corners.length} corners` : ''}</span>
      <span>{formatM(shownFrom)} – {formatM(shownTo)}</span>
    </div>
    {verdict && <div className="text-[12px] font-bold" style={verdict.color ? { color: verdict.color } : undefined}>{verdict.text}</div>}
    <table className="w-full border-collapse">
      <thead><tr>
        {current && lapHead(currentLabel, currentColor, 'left')}
        <th />
        {comparison && lapHead(comparisonLabel, comparisonColor, 'right')}
      </tr></thead>
      <tbody>
        {shown.map(([measure, a, b, better, warn], rowIndex) => <tr key={measure} className="braking-row" style={{ animationDelay: `${rowIndex * 18}ms` }}>
          {current && cell(a, better === 0, warn?.[0] ?? false, currentColor, 'left')}
          <td className="border-t border-[var(--border)] px-1.5 py-[3px] text-center text-[9px] text-[var(--text-secondary)]">{measure}</td>
          {comparison && cell(b, better === 1, warn?.[1] ?? false, comparisonColor, 'right')}
        </tr>)}
      </tbody>
    </table>
    {/* Observations are sentences: they wrap inside the card, each line hanging
        under its marker, rather than running past its edge. */}
    {observations.length > 0 && <div className="flex flex-col gap-1 whitespace-normal border-t border-[var(--border)] pt-1.5 leading-snug text-[var(--text-secondary)]">
      {observations.map(text => <div key={text} className="flex gap-1.5">
        <span className="shrink-0 text-[var(--text-muted)]">▸</span><span className="min-w-0">{text}</span>
      </div>)}
    </div>}
  </div>
}

/** Where a lap braked, or that it only lifted (and where). */
const brakeAtText = (point: BrakingPointResult) =>
  point.liftOnly ? `lift ${Math.round(point.distanceM).toLocaleString()} m` : formatM(point.distanceM)
const metresText = (metres: number) => metres < 0 ? '—' : `${Math.round(metres).toLocaleString()} m`
// Apexes this close on two laps are the same corner.
const CORNER_MATCH_M = 80
/** Which way a corner turns, after its apex label. */
const sideText = (side: number) => side < 0 ? ' · L' : side > 0 ? ' · R' : ''

/** Two laps' corners lined up: each current-lap apex with the comparison
 *  lap's nearest within CORNER_MATCH_M, and any left over on their own, in
 *  order along the lap. */
function alignCorners(a: BrakingPointResult['apexes'], b: BrakingPointResult['apexes']) {
  type Apex = BrakingPointResult['apexes'][number]
  const taken = new Set<number>()
  const rows: Array<[Apex | undefined, Apex | undefined]> = a.map(apex => {
    let best = -1
    let bestGap = CORNER_MATCH_M
    b.forEach((other, index) => {
      const gap = Math.abs(other.distanceM - apex.distanceM)
      if (!taken.has(index) && gap <= bestGap) { best = index; bestGap = gap }
    })
    if (best < 0) return [apex, undefined]
    taken.add(best)
    return [apex, b[best]]
  })
  b.forEach((apex, index) => { if (!taken.has(index)) rows.push([undefined, apex]) })
  return rows.sort((x, y) => (x[0] ?? x[1])!.distanceM - (y[0] ?? y[1])!.distanceM)
}

const coastText = (point: BrakingPointResult) =>
  point.coastS > 0 ? `${point.coastS.toFixed(1)} s · ${Math.round(point.coastM)} m` : 'none'
const spinText = (point: BrakingPointResult) =>
  point.wheelspin ? `${Math.round(point.wheelspinPct)}% · ${point.wheelspinS.toFixed(1)} s` : 'none'
/** The better corner exit point (0 current, 1 comparison): the earlier one
 *  when neither lap spun, the clean lap when only one did, and none when both
 *  did, since an exit that spins cannot count as the better one. */
function exitBetter(a: number, b: number, aSpun: boolean, bSpun: boolean): 0 | 1 | null {
  if (a < 0 || b < 0 || (aSpun && bSpun)) return null
  if (aSpun !== bSpun) return aSpun ? 1 : 0
  return Math.abs(a - b) < 0.5 ? null : a < b ? 0 : 1
}

const LEGEND_CLASS = 'pointer-events-none absolute bottom-2 right-2 z-10 flex items-center text-[9px] font-mono text-[var(--text-secondary)]'

/** Driver inputs key, in the map's bottom-right corner. */
function InputsLegend({ isDark }: { isDark: boolean }) {
  const palette = isDark ? INPUT_COLORS_DARK : INPUT_COLORS_LIGHT
  return <div className={`${LEGEND_CLASS} gap-2.5`}>
    {INPUT_STATE_LABELS.map((label, state) => <span key={label} className="flex items-center gap-1">
      <span className="h-1.5 w-3 rounded-full" style={{ background: palette[state] }} />{label}
    </span>)}
  </div>
}

// The analysis screen re-renders with every telemetry frame. The map keeps its
// own clock and draws its markers from a source function, so it only needs to
// re-render when the laps, colours or view settings around it change.
export default memo(function AnalyzeMapComparison({
  comparison, comparisonColor, comparisonLabel, compatibleCircuit, current, currentColor, currentLabel,
  fixedMode, isDark, mapColoring, mapResult, comparisonSelected, brakingResult, showInputPanel,
  mapDimmed, reduceAnimations,
  sectorColors, trackId, focus,
}: Props) {
  const playbackBarPresence = useModalPresence(fixedMode, 180)
  const aeroMode = useTelemetryStore(state => state.protocolStatus?.aero_mode ?? 'drs')
  // The primary recording is the one loaded for playback; its map choice
  // applies to every lap drawn on its circuit.
  const mapResolution = useTrackMapStore(state => state.session)
  const result = mapResult?.mode === mapColoring ? mapResult : null
  const ersMode = mapColoring === 'ers'
  const inputsMode = mapColoring === 'inputs'
  const harvestMode = mapColoring === 'harvest'
  const speedMode = mapColoring === 'speed'
  const currentSpeed = speedMode && result?.hasCurrent ? result.currentSpeed : null
  const comparisonSpeed = speedMode && result?.hasComparison ? result.comparisonSpeed : null
  const currentHarvest = harvestMode && result?.hasCurrent ? result.currentHarvest : null
  const comparisonHarvest = harvestMode && result?.hasComparison ? result.comparisonHarvest : null
  const currentUsage = ersMode && result?.hasCurrent ? result.current : null
  const comparisonUsage = ersMode && result?.hasComparison ? result.comparison : null
  const [trackPaint, comparisonTrackPaint] = useMemo<[TrackPaint | null, TrackPaint | null]>(() => {
    if (!result) return [null, null]
    switch (result.mode) {
      case 'dominance': return [dominancePaint(result, currentColor, comparisonColor, isDark), null]
      case 'harvest': return [
        result.hasCurrent ? harvestPaint(result.currentHarvest, isDark) : null,
        result.hasComparison ? harvestPaint(result.comparisonHarvest, isDark) : null,
      ]
      case 'inputs': return [
        result.hasCurrent ? inputsPaint(result.currentInputs, isDark) : null,
        result.hasComparison ? inputsPaint(result.comparisonInputs, isDark) : null,
      ]
      case 'speed': return [
        result.hasCurrent ? speedPaint(result.currentSpeed, result.speedScaleMinKph, result.speedScaleMaxKph, isDark) : null,
        result.hasComparison ? speedPaint(result.comparisonSpeed, result.speedScaleMinKph, result.speedScaleMaxKph, isDark) : null,
      ]
      default: return [
        result.hasCurrent ? ersPaint(result.current, isDark) : null,
        result.hasComparison ? ersPaint(result.comparison, isDark) : null,
      ]
    }
  }, [comparisonColor, currentColor, isDark, result])
  // ERS Usage, ERS Harvest, Driver Inputs and Speed give each lap its own
  // map, side by side on a wide pane and stacked on a tall one. The layout
  // follows the selection, not the lap data, so switching laps does not drop
  // to one map while it loads.
  const separateMaps = (ersMode || harvestMode || inputsMode || speedMode) && comparisonSelected
  const legend = (usage: ErsUsageResult | null, harvest: ErsHarvestResult | null, speed: SpeedResult | null) => ersMode
    ? <ErsScale usage={usage} isDark={isDark} />
    : harvestMode
      ? <HarvestScale harvest={harvest} isDark={isDark} />
    : speedMode && result
      ? <SpeedScale speed={speed} minKph={result.speedScaleMinKph} maxKph={result.speedScaleMaxKph} isDark={isDark} />
    : inputsMode && result
      ? <InputsLegend isDark={isDark} />
      : null
  // Two cars whose peaks differ in a grip both laps saw are drawn against different caps.
  const peakMismatches = separateMaps && currentUsage && comparisonUsage
    ? [
        result?.normalPeakMismatch && `normal grip ${currentLabel} ${formatKw(currentUsage.normalPeakKw)}, ${comparisonLabel} ${formatKw(comparisonUsage.normalPeakKw)}`,
        result?.reducedPeakMismatch && `reduced grip ${currentLabel} ${formatKw(currentUsage.reducedPeakKw)}, ${comparisonLabel} ${formatKw(comparisonUsage.reducedPeakKw)}`,
      ].filter((text): text is string => typeof text === 'string')
    : separateMaps && currentHarvest && comparisonHarvest && result?.harvestPeakMismatch
      ? [`harvest ${currentLabel} ${formatKw(Math.round(currentHarvest.peakKw))}, ${comparisonLabel} ${formatKw(Math.round(comparisonHarvest.peakKw))}`]
      : []
  // Braking zones, highlighted on the racing line in two halves: braking, up
  // to the slowest point, dimmed, then acceleration. With two laps each half
  // takes the colour of the lap that was quicker through it, neutral when
  // level; one lap on its own shows its zones in its colour. Separate maps
  // show the same highlights. Hovering a zone shows both laps in detail, with
  // the hovered half brought forward.
  const brakingMarks = useMemo<TrackMark[] | null>(() => {
    const braking = brakingResult?.hasCurrent ? brakingResult.braking : null
    if (!braking || !(braking.trackLengthM > 0)) return null
    const length = braking.trackLengthM
    const level = isDark ? '#E9EAEE' : '#1B1D24'
    const tooltip = (zone: number, a: BrakingPointResult | undefined, b: BrakingPointResult | undefined,
      pair: BrakingPairResult | undefined) =>
      (phase: BrakingPhase, fromM: number, splitM: number, toM: number) =>
        <BrakingTooltip zone={zone} phase={phase} fromM={fromM} splitM={splitM} toM={toM}
          current={a} comparison={b} pair={pair}
          currentLabel={currentLabel} comparisonLabel={comparisonLabel}
          currentColor={currentColor} comparisonColor={comparisonColor} />
    // The quicker lap's colour over a stretch, neutral when level or untimed.
    const winner = (currentS: number, comparisonS: number) => {
      if (currentS < 0 || comparisonS < 0) return level
      const quicker = comparisonS - currentS
      return Math.abs(quicker) < BRAKING_LEVEL_S ? level : quicker > 0 ? currentColor : comparisonColor
    }
    // A zone as its braking half, dimmed, then its acceleration half, each
    // with its own tooltip; whole when it has no split.
    const halves = (fromM: number, splitM: number, toM: number, brakingColor: string, accelerationColor: string,
      card: (phase: BrakingPhase, fromM: number, splitM: number, toM: number) => ReactNode): TrackMark[] =>
      splitM < 0
        ? [{ fromFraction: fromM / length, toFraction: toM / length, color: accelerationColor,
             tooltip: card(null, fromM, splitM, toM) }]
        : [{ fromFraction: fromM / length, toFraction: splitM / length, color: brakingColor,
             opacity: BRAKING_HALF_OPACITY, tooltip: card('braking', fromM, splitM, toM) },
           { fromFraction: splitM / length, toFraction: toM / length, color: accelerationColor,
             tooltip: card('acceleration', fromM, splitM, toM) }]
    if (braking.comparison.length === 0)
      return braking.current.flatMap((point, index) => {
        const toM = Math.max(point.distanceM, point.endM)
        const split = point.minSpeedM - point.distanceM >= PHASE_MIN_M && toM - point.minSpeedM >= PHASE_MIN_M
          ? point.minSpeedM : -1
        return halves(point.distanceM, split, toM, currentColor, currentColor,
          tooltip(index + 1, point, undefined, undefined))
      })
    return braking.pairs.flatMap(pair => halves(pair.fromM, pair.splitM, pair.toM,
      winner(pair.currentBrakingS, pair.comparisonBrakingS),
      pair.splitM < 0 ? winner(pair.currentTimeS, pair.comparisonTimeS)
        : winner(pair.currentAccelerationS, pair.comparisonAccelerationS),
      tooltip(pair.current + 1, braking.current[pair.current], braking.comparison[pair.comparison], pair)))
  }, [brakingResult, comparisonColor, comparisonLabel, currentColor, currentLabel, isDark])
  // The Speed map marks each lap's corners on its own map, over any braking zones.
  const currentMarks = useMemo<TrackMark[] | null>(() => {
    const apexes = apexMarks(currentSpeed, isDark)
    return apexes.length > 0 ? [...(brakingMarks ?? []), ...apexes] : brakingMarks
  }, [brakingMarks, currentSpeed, isDark])
  const comparisonMarks = useMemo<TrackMark[] | null>(() => {
    if (!separateMaps) return null
    const apexes = apexMarks(comparisonSpeed, isDark)
    return apexes.length > 0 ? [...(brakingMarks ?? []), ...apexes] : brakingMarks
  }, [brakingMarks, comparisonSpeed, isDark, separateMaps])
  const { ref: mapsRef, width: mapsWidth, height: mapsHeight } = useSize()
  const stackMaps = mapsHeight > mapsWidth
  const total = Math.max(lapDuration(current), lapDuration(comparison))
  const initialCursor = Math.max(0, Math.min(total, focus?.elapsedSeconds ?? 0))
  const clockRef = useRef<LocalClock>({ cursor: initialCursor, playing: false, speed: 1, startedAt: performance.now(), total })
  const [barState, setBarState] = useState<BarState>({ currentTime: initialCursor, isPlaying: false, progressPct: total > 0 ? initialCursor / total : 0, speed: 1, totalTime: total })
  const markerBufferRef = useRef<TrackMapMarker[]>([])
  const currentMarkerBufferRef = useRef<TrackMapMarker[]>([])
  const comparisonMarkerBufferRef = useRef<TrackMapMarker[]>([])
  const currentMarkerRef = useRef<TrackMapMarker>({ x: 0, z: 0, label: '', color: currentColor })
  const comparisonMarkerRef = useRef<TrackMapMarker>({ x: 0, z: 0, label: '', color: comparisonColor })

  const publishClock = useCallback((cursor: number, playing: boolean) => {
    const clock = clockRef.current
    const next: BarState = {
      currentTime: cursor,
      isPlaying: playing,
      progressPct: clock.total > 0 ? cursor / clock.total : 0,
      speed: clock.speed,
      totalTime: clock.total,
    }
    setBarState(previous => previous.currentTime === next.currentTime &&
      previous.isPlaying === next.isPlaying && previous.progressPct === next.progressPct &&
      previous.speed === next.speed && previous.totalTime === next.totalTime ? previous : next)
  }, [])

  const setClockTime = useCallback((cursor: number) => {
    const clock = clockRef.current
    clock.cursor = Math.max(0, Math.min(clock.total, cursor))
    clock.startedAt = performance.now()
    publishClock(clock.cursor, clock.playing)
  }, [publishClock])

  useEffect(() => {
    const clock = clockRef.current
    const cursor = Math.max(0, Math.min(total, focus?.elapsedSeconds ?? 0))
    clock.cursor = cursor
    clock.playing = false
    clock.startedAt = performance.now()
    clock.total = total
    publishClock(cursor, false)
  }, [comparison, current, focus, publishClock, total])

  useEffect(() => {
    if (!fixedMode) {
      const clock = clockRef.current
      clock.cursor = readClock(clock)
      clock.playing = false
      clock.startedAt = performance.now()
      publishClock(clock.cursor, false)
      return
    }
    const timer = window.setInterval(() => {
      const clock = clockRef.current
      let cursor = readClock(clock)
      if (clock.playing && cursor >= clock.total) {
        cursor = clock.total
        clock.cursor = cursor
        clock.playing = false
        clock.startedAt = performance.now()
      }
      publishClock(cursor, clock.playing)
    }, 100)
    return () => window.clearInterval(timer)
  }, [fixedMode, publishClock])

  const elapsedSource = useCallback((): number => {
    const globalCursorTime = getPlaybackCursorTime()
    return fixedMode
      ? readClock(clockRef.current)
      : focus
        ? Math.max(0, Math.min(total, focus.elapsedSeconds))
      : current && globalCursorTime !== null
        ? globalCursorTime - current.startSessionTime
        : 0
  }, [current, fixedMode, focus, total])

  const writeMarkers = useCallback((output: TrackMapMarker[], withCurrent: boolean, withComparison: boolean): readonly TrackMapMarker[] => {
    const elapsed = elapsedSource()
    output.length = 0

    if (current && withCurrent) {
      const marker = currentMarkerRef.current
      marker.label = currentLabel
      marker.color = currentColor
      const target = current.startSessionTime + Math.max(0, Math.min(lapDuration(current), elapsed))
      if (writeMarkerAt(marker, current.playerPositions, target)) output.push(marker)
    }
    if (comparison && withComparison) {
      const marker = comparisonMarkerRef.current
      marker.label = comparisonLabel
      marker.color = comparisonColor
      const target = comparison.startSessionTime + Math.max(0, Math.min(lapDuration(comparison), elapsed))
      if (writeMarkerAt(marker, comparison.playerPositions, target)) output.push(marker)
    }
    return output
  }, [comparison, comparisonColor, comparisonLabel, current, currentColor, currentLabel, elapsedSource])

  const markerSource = useCallback(
    () => writeMarkers(markerBufferRef.current, true, true), [writeMarkers])
  const currentMarkerSource = useCallback(
    () => writeMarkers(currentMarkerBufferRef.current, true, false), [writeMarkers])
  const comparisonMarkerSource = useCallback(
    () => writeMarkers(comparisonMarkerBufferRef.current, false, true), [writeMarkers])

  // Split mode draws this clock as a scrubber on the charts. Only the fixed
  // comparison owns a clock of its own; otherwise the cursor already follows
  // playback or a chart hover, which the charts know about without the map.
  useEffect(() => {
    if (!fixedMode) return
    return setAnalyzeCursorSource(elapsedSource)
  }, [elapsedSource, fixedMode])

  const togglePlay = useCallback(() => {
    const clock = clockRef.current
    let cursor = readClock(clock)
    if (!clock.playing && cursor >= clock.total) cursor = 0
    clock.cursor = cursor
    clock.playing = !clock.playing && clock.total > 0
    clock.startedAt = performance.now()
    publishClock(clock.cursor, clock.playing)
  }, [publishClock])

  const setSpeed = useCallback((speed: number) => {
    const clock = clockRef.current
    clock.cursor = readClock(clock)
    clock.speed = speed
    clock.startedAt = performance.now()
    publishClock(clock.cursor, clock.playing)
  }, [publishClock])

  const map = useMemo(() => <TrackMap
    trackId={compatibleCircuit ? trackId : null}
    participants={null}
    isDark={isDark}
    sectorColors={sectorColors}
    reduceAnimations={reduceAnimations}
    mapDimmed={mapDimmed}
    aeroMode={aeroMode}
    markerSource={separateMaps ? currentMarkerSource : markerSource}
    trackPaint={trackPaint}
    trackMarks={currentMarks}
    mapResolution={mapResolution}
  />, [aeroMode, compatibleCircuit, currentMarkerSource, currentMarks, isDark, mapDimmed, mapResolution, markerSource, reduceAnimations, sectorColors, separateMaps, trackId, trackPaint])

  const comparisonMap = useMemo(() => separateMaps && <TrackMap
    trackId={compatibleCircuit ? trackId : null}
    participants={null}
    isDark={isDark}
    sectorColors={sectorColors}
    reduceAnimations={reduceAnimations}
    mapDimmed={mapDimmed}
    aeroMode={aeroMode}
    markerSource={comparisonMarkerSource}
    trackPaint={comparisonTrackPaint}
    trackMarks={comparisonMarks}
    mapResolution={mapResolution}
  />, [aeroMode, compatibleCircuit, comparisonMarkerSource, comparisonMarks, comparisonTrackPaint, isDark, mapDimmed, mapResolution, reduceAnimations, sectorColors, separateMaps, trackId])

  return <div className="h-full min-h-0 flex flex-col">
    <div ref={mapsRef} className="flex-1 min-h-0 relative">
      {separateMaps
        ? <div className={`absolute inset-0 flex ${stackMaps ? 'flex-col' : ''}`}>
            <div className="relative flex-1 min-w-0 min-h-0">
              {map}
              {legend(currentUsage, currentHarvest, currentSpeed)}
            </div>
            <div className={`shrink-0 bg-[var(--border)] ${stackMaps ? 'h-px' : 'w-px'}`} />
            <div className="relative flex-1 min-w-0 min-h-0">
              {comparisonMap}
              {legend(comparisonUsage, comparisonHarvest, comparisonSpeed)}
            </div>
          </div>
        : <>
            {map}
            {legend(currentUsage, currentHarvest, currentSpeed)}
          </>}
      {peakMismatches.length > 0 && <div
        role="status"
        className="pointer-events-none absolute top-2 right-2 z-10 flex items-center gap-1.5 rounded-md border border-amber-500/40 bg-[var(--bg-panel)] px-2 py-1 text-[10px] text-[var(--text-primary)]"
      >
        <AlertTriangle size={12} className="shrink-0 text-amber-500" />
        <span>Peak MGU-K differs: {peakMismatches.join('; ')}</span>
      </div>}
      {showInputPanel && <AnalyzeInputComparison
        current={current}
        comparison={comparison}
        currentColor={currentColor}
        comparisonColor={comparisonColor}
        currentLabel={currentLabel}
        comparisonLabel={comparisonLabel}
        elapsedSource={elapsedSource}
      />}
    </div>
    {playbackBarPresence.mounted && <div
      className={`analyze-map-playback-bar ${playbackBarPresence.visible ? 'analyze-map-playback-bar--visible' : ''}`}
      aria-hidden={!playbackBarPresence.visible}
      inert={!playbackBarPresence.visible}
    >
      <div className="analyze-map-playback-bar__inner">
        <PlaybackBar
          compact
          onSeekProgress={progress => setClockTime(progress * clockRef.current.total)}
          onSeekBackward={() => setClockTime(readClock(clockRef.current) - 5)}
          onSeekForward={() => setClockTime(readClock(clockRef.current) + 5)}
          onSpeedChange={setSpeed}
          onTogglePlay={togglePlay}
          showExport={false}
          showLapSelect={false}
          state={barState}
        />
      </div>
    </div>}
  </div>
})