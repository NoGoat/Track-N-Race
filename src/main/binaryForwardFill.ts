import { lastHotRecords, recSessionTime, recWithSessionTime, telemetrySessionTimes } from './binaryRows'

const DEFAULT_PERIOD_S = 1 / 60   // bootstrap until the real cadence is measured
const WINDOW = 24                 // deltas kept for the median estimate
const MIN_SAMPLES = 8             // before trusting the measured period
const MIN_DELTA_S = 0.005         // 200 Hz ceiling — reject sub-frame noise
const MAX_DELTA_S = 0.2           // 5 Hz floor — reject pauses / gaps as "frames"
const CHANGE_EPS_S = 5e-5         // 0.05 ms — ignore rounding jitter, track real changes

function median(xs: number[]): number {
  const s = [...xs].sort((a, b) => a - b)
  const m = s.length >> 1
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2
}

// Median of session_time deltas (seconds), rounded to 0.01 ms granularity — the
// canonical frame period (e.g. 0.01667 s ⇒ 16.67 ms). Shared by the live detector
// and playback (which measures the recorded cadence at load).
export function medianRoundedPeriodS(deltasS: number[]): number {
  return Math.round(median(deltasS) * 1e5) / 1e5
}

// Smooths the live hot-row stream for the renderer. The chart's x-axis is the
// telemetry stream, so a tick with no fresh data (UDP jitter, or the flush tick
// beating against the game's frame cadence) leaves a gap and the graph stutters.
// On an empty tick we re-emit the last telemetry/motion/motion_ex sample with
// session_time nudged forward by exactly one measured frame — never more than one
// frame past the last REAL sample, so a real packet always lands monotonically
// after a fill (no spurious flashback reset). Display-only: recording happens
// upstream in the addon and is unaffected.
//
// The frame period is measured from the real telemetry stream (session_time deltas)
// rather than hardcoded, so fills line up with the actual send rate (20/40/60 Hz or
// an fps-capped value) to ~0.01 ms, not a fixed 16 ms grid.
export class HotRowSmoother {
  private lastTel: Buffer | null = null
  private lastMot: Buffer | null = null
  private lastMotEx: Buffer | null = null
  private lastRealST = 0
  private lastEmitST = 0

  // Rate detection.
  private periodS = DEFAULT_PERIOD_S
  private prevTelST: number | null = null
  private deltas: number[] = []

  reset(): void {
    this.lastTel = this.lastMot = this.lastMotEx = null
    this.lastRealST = 0
    this.lastEmitST = 0
    this.periodS = DEFAULT_PERIOD_S
    this.prevTelST = null
    this.deltas = []
  }

  // Measured frame period in ms, rounded to two decimals (e.g. 16.67).
  getPeriodMs(): number {
    return Math.round(this.periodS * 1e5) / 100
  }

  // Feed every real telemetry session_time, in order, to refine the period.
  private observe(sts: number[]): void {
    for (const st of sts) {
      if (this.prevTelST !== null) {
        const d = st - this.prevTelST
        if (d >= MIN_DELTA_S && d <= MAX_DELTA_S) {
          this.deltas.push(d)
          if (this.deltas.length > WINDOW) this.deltas.shift()
        }
      }
      this.prevTelST = st
    }
    if (this.deltas.length >= MIN_SAMPLES) {
      // Round the median to 0.01 ms granularity; only adopt it when it moves by
      // more than rounding jitter, so the value stays a stable 2-decimal period.
      const candidate = medianRoundedPeriodS(this.deltas)
      if (Math.abs(candidate - this.periodS) > CHANGE_EPS_S) this.periodS = candidate
    }
  }

  // Bytes to send this tick, or null if there's nothing to send.
  tick(pending: Uint8Array[]): Buffer | null {
    if (pending.length > 0) {
      const batch = pending.length === 1
        ? Buffer.from(pending[0])
        : Buffer.concat(pending.map(b => Buffer.from(b)))
      this.observe(telemetrySessionTimes(batch))
      const last = lastHotRecords(batch)
      if (last.tel) { this.lastTel = last.tel; this.lastRealST = recSessionTime(last.tel) }
      if (last.mot) this.lastMot = last.mot
      if (last.motEx) this.lastMotEx = last.motEx
      if (this.lastRealST > this.lastEmitST) this.lastEmitST = this.lastRealST
      return batch
    }

    // Empty tick: hold the last sample, advance session_time by one measured frame,
    // capped at one frame past the last real sample (so real data stays monotonic).
    if (!this.lastTel) return null
    const target = Math.min(this.lastEmitST + this.periodS, this.lastRealST + this.periodS)
    if (target <= this.lastEmitST + 1e-6) return null
    this.lastEmitST = target
    const parts = [recWithSessionTime(this.lastTel, target)]
    if (this.lastMot) parts.push(recWithSessionTime(this.lastMot, target))
    if (this.lastMotEx) parts.push(recWithSessionTime(this.lastMotEx, target))
    return Buffer.concat(parts)
  }
}
