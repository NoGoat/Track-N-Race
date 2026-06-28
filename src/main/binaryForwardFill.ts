import { lastHotRecords, recSessionTime, recWithSessionTime } from './binaryRows'

const FRAME_S = 1 / 60   // game telemetry cadence (~16.67 ms)

// Smooths the live hot-row stream for the renderer. The chart's x-axis is the
// telemetry stream, so a tick with no fresh data (UDP jitter, or the 16 ms tick
// beating against the game's 16.67 ms cadence) leaves a gap and the graph
// stutters. On an empty tick we re-emit the last telemetry/motion/motion_ex
// sample with session_time nudged forward — but never more than one game frame
// past the last REAL sample, so a real packet always lands monotonically after a
// fill (no spurious flashback reset in the renderer). Display-only: recording
// happens upstream in the addon and is unaffected.
export class HotRowSmoother {
  private lastTel: Buffer | null = null
  private lastMot: Buffer | null = null
  private lastMotEx: Buffer | null = null
  private lastRealST = 0
  private lastEmitST = 0

  constructor(private readonly tickMs: number) {}

  reset(): void {
    this.lastTel = this.lastMot = this.lastMotEx = null
    this.lastRealST = 0
    this.lastEmitST = 0
  }

  // Bytes to send this tick, or null if there's nothing to send.
  tick(pending: Uint8Array[]): Buffer | null {
    if (pending.length > 0) {
      const batch = pending.length === 1
        ? Buffer.from(pending[0])
        : Buffer.concat(pending.map(b => Buffer.from(b)))
      const last = lastHotRecords(batch)
      if (last.tel) { this.lastTel = last.tel; this.lastRealST = recSessionTime(last.tel) }
      if (last.mot) this.lastMot = last.mot
      if (last.motEx) this.lastMotEx = last.motEx
      if (this.lastRealST > this.lastEmitST) this.lastEmitST = this.lastRealST
      return batch
    }

    if (!this.lastTel) return null
    const target = Math.min(this.lastEmitST + this.tickMs / 1000, this.lastRealST + FRAME_S)
    if (target <= this.lastEmitST + 1e-6) return null   // would overshoot real data
    this.lastEmitST = target
    const parts = [recWithSessionTime(this.lastTel, target)]
    if (this.lastMot) parts.push(recWithSessionTime(this.lastMot, target))
    if (this.lastMotEx) parts.push(recWithSessionTime(this.lastMotEx, target))
    return Buffer.concat(parts)
  }
}
