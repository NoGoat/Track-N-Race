import type { FastestLapMsg, SessionHistoryFastestMsg } from '../types'

/**
 * The car holding the session's fastest lap, from a recording's or the live
 * session's rows. A fastest_lap row names it (live packets, and V6 playback,
 * which derives it from the lap summaries). Until one arrives, the holder is
 * the car with the lowest best lap among the session_history_fastest rows
 * seen (older recordings keep no fastest_lap rows). Used by file A's store and
 * file B's stream alike.
 */
export class FastestLapHolder {
  private named = false
  private readonly bestByCar = new Map<number, number>()

  /** A fastest_lap row: its car, which from now on wins over the lap histories. */
  fastestLap(msg: FastestLapMsg): number {
    this.named = true
    return msg.car_idx
  }

  /**
   * A session_history_fastest row: the holder by best lap so far (null when no
   * car has one), or undefined once a fastest_lap row has named the holder.
   */
  sessionHistory(msg: SessionHistoryFastestMsg): number | null | undefined {
    if (this.named) return undefined
    this.bestByCar.set(msg.car_idx, msg.best_lap_time_ms)
    let minMs = Infinity
    let minIdx: number | null = null
    for (const [idx, ms] of this.bestByCar) {
      if (ms < minMs) { minMs = ms; minIdx = idx }
    }
    return minIdx
  }

  clear(): void {
    this.named = false
    this.bestByCar.clear()
  }
}
