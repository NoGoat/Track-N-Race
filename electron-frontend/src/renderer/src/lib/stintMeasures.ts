import type { ColumnView } from './columnStore'
import type { StatusRow } from '../types'
import { lapStartSessionTime, type LapBoundary } from './chartCoordinates'

// The ERS counters are per-lap totals that the game resets at the timing line,
// so the last value before a reset is that lap's total. Status and timing
// packets can arrive either side of the line; a reset far from any line (a
// flashback or a garage visit) is not a lap total.
const RESET_LINE_TOLERANCE_S = 5
// A fuel sample further than this from a lap line cannot stand for the line.
const FUEL_LINE_TOLERANCE_S = 2
// Fuel only falls on track; any rise is a refuel or a reset.
const FUEL_RISE_KG = 0.01

interface Reset { time: number; total: number }

class LapCounter {
  readonly resets: Reset[] = []
  // NaN while the value is unavailable, so a gap is never read as a reset.
  latest = NaN
  push(time: number, value: number): void {
    if (value < this.latest) this.resets.push({ time, total: this.latest })
    this.latest = value
  }
}

/**
 * Incremental scan of the streamed driver's status history. The newest row
 * can still be patched in place by a later V6 sample at the same time, so it
 * is never consumed; callers read it directly for the lap in progress.
 */
export class StintStatusScan {
  readonly deployed = new LapCounter()
  // The two harvest counters are separate per-lap totals; each resets on its own.
  readonly harvestedK = new LapCounter()
  readonly harvestedH = new LapCounter()
  readonly fuelRises: number[] = []
  private lastFuel = NaN
  private scanned = 0
  private firstTime = NaN
  private lastScannedTime = NaN

  /** False when the history was replaced or trimmed; start a new scan. */
  update(rows: ColumnView<StatusRow>): boolean {
    const end = rows.length - 1
    if (end < this.scanned || (this.scanned > 0 &&
        (rows.time(0) !== this.firstTime || rows.time(this.scanned - 1) !== this.lastScannedTime))) return false
    for (let i = this.scanned; i < end; i++) {
      const time = rows.time(i)
      this.deployed.push(time, rows.num('ers_deployed_j', i))
      this.harvestedK.push(time, rows.num('ers_harvested_mguk_j', i))
      this.harvestedH.push(time, rows.num('ers_harvested_mguh_j', i))
      const fuel = rows.num('fuel_kg', i)
      if (fuel > this.lastFuel + FUEL_RISE_KG) this.fuelRises.push(time)
      this.lastFuel = fuel
    }
    if (end > this.scanned) {
      this.scanned = end
      this.firstTime = rows.time(0)
      this.lastScannedTime = rows.time(end - 1)
    }
    return true
  }
}

export interface LapMeasure {
  deployedMj: number | null
  harvestedMj: number | null
  fuelKg: number | null
}

/** The lap that ended at the line nearest `time`, or null when no line is close. */
function lapEndingNear(boundaries: readonly LapBoundary[], time: number): number | null {
  let lo = 0, hi = boundaries.length
  while (lo < hi) { const mid = (lo + hi) >> 1; if (boundaries[mid].sessionTime < time) lo = mid + 1; else hi = mid }
  let nearest = -1
  for (const index of [lo - 1, lo]) {
    if (index < 0 || index >= boundaries.length) continue
    if (nearest === -1 || Math.abs(boundaries[index].sessionTime - time) < Math.abs(boundaries[nearest].sessionTime - time)) nearest = index
  }
  if (nearest === -1 || Math.abs(boundaries[nearest].sessionTime - time) > RESET_LINE_TOLERANCE_S) return null
  return nearest > 0 ? boundaries[nearest - 1].lapNum : boundaries[nearest].lapNum - 1
}

function totalsByLap(resets: readonly Reset[], boundaries: readonly LapBoundary[]): Map<number, number> {
  const totals = new Map<number, number>()
  for (const reset of resets) {
    const lap = lapEndingNear(boundaries, reset.time)
    if (lap !== null) totals.set(lap, reset.total / 1_000_000)
  }
  return totals
}

function fuelAt(rows: ColumnView<StatusRow>, time: number): number {
  const after = rows.lowerBound(time, true)
  let best = NaN, bestGap = FUEL_LINE_TOLERANCE_S
  for (const i of [after - 1, after]) {
    if (i < 0 || i >= rows.length) continue
    const gap = Math.abs(rows.time(i) - time)
    const fuel = rows.num('fuel_kg', i)
    if (gap <= bestGap && Number.isFinite(fuel)) { best = fuel; bestGap = gap }
  }
  return best
}

/**
 * ERS used, ERS recharged and fuel used on each completed lap. Recharge is
 * MGU-K plus MGU-H, or MGU-K alone under regulations without an MGU-H (the
 * 2026 packet still carries a legacy MGU-H field).
 */
export function measureLaps(scan: StintStatusScan, rows: ColumnView<StatusRow>,
  boundaries: readonly LapBoundary[], laps: readonly number[], hasMguh: boolean): Map<number, LapMeasure> {
  const deployed = totalsByLap(scan.deployed.resets, boundaries)
  const harvestedK = totalsByLap(scan.harvestedK.resets, boundaries)
  const harvestedH = hasMguh ? totalsByLap(scan.harvestedH.resets, boundaries) : null
  const out = new Map<number, LapMeasure>()
  for (const lap of laps) {
    const start = lapStartSessionTime(boundaries, lap)
    const end = lapStartSessionTime(boundaries, lap + 1)
    let fuelKg: number | null = null
    if (start !== undefined && end !== undefined &&
        !scan.fuelRises.some(time => time > start && time <= end)) {
      const used = fuelAt(rows, start) - fuelAt(rows, end)
      if (used >= 0) fuelKg = used
    }
    const k = harvestedK.get(lap), h = harvestedH ? harvestedH.get(lap) : 0
    out.set(lap, { deployedMj: deployed.get(lap) ?? null, harvestedMj: k !== undefined && h !== undefined ? k + h : null, fuelKg })
  }
  return out
}

/**
 * The counters so far on the lap in progress. Null until the counter has
 * reset for this lap: just after the line it still holds the previous total.
 */
export function inProgressLap(scan: StintStatusScan, rows: ColumnView<StatusRow>,
  boundaries: readonly LapBoundary[], lap: number, hasMguh: boolean): Pick<LapMeasure, 'deployedMj' | 'harvestedMj'> {
  const tail = rows.length - 1
  const start = lapStartSessionTime(boundaries, lap)
  const read = (counter: LapCounter, field: string) => {
    const value = tail < 0 ? NaN : rows.num(field, tail)
    if (start === undefined || !Number.isFinite(value)) return null
    const resetAtTail = value < counter.latest
    const lastReset = counter.resets.at(-1)
    const resetThisLap = lastReset !== undefined && lastReset.time >= start - RESET_LINE_TOLERANCE_S
    const historyStartsThisLap = rows.time(0) >= start - RESET_LINE_TOLERANCE_S
    return resetAtTail || resetThisLap || historyStartsThisLap ? value / 1_000_000 : null
  }
  const k = read(scan.harvestedK, 'ers_harvested_mguk_j')
  const h = hasMguh ? read(scan.harvestedH, 'ers_harvested_mguh_j') : 0
  return {
    deployedMj: read(scan.deployed, 'ers_deployed_j'),
    harvestedMj: k !== null && h !== null ? k + h : null,
  }
}
