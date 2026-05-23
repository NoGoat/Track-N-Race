import { FRAME_SAMPLED, SLOW_RATE_MS } from './packetParsers'

export class RateLimiter {
  private lastFrameId = new Map<number, number>()
  private lastSlowMs  = new Map<number, number>()
  private sampleEvery: number

  constructor(sampleEvery = 1) {
    this.sampleEvery = sampleEvery
  }

  allow(pid: number, overallFrameId: number): boolean {
    if (FRAME_SAMPLED.has(pid)) {
      if (this.sampleEvery > 1) {
        const prev = this.lastFrameId.get(pid) ?? -1
        if (overallFrameId === prev) return false
        if (overallFrameId % this.sampleEvery !== 0) return false
      }
      this.lastFrameId.set(pid, overallFrameId)
      return true
    }

    const rateMs = SLOW_RATE_MS[pid] ?? 500
    if (rateMs === 0) return true
    const now  = Date.now()
    const last = this.lastSlowMs.get(pid) ?? 0
    if (now - last < rateMs) return false
    this.lastSlowMs.set(pid, now)
    return true
  }
}
