import type { SecondaryFileData } from '../../components/AnalyzeScreen'
import type { AnalyzeLapData } from '../../types'

/**
 * File B's copy of the lap file A is on, as the side-by-side charts and the
 * Tyres rack read it. Laps stay in step by number. The rows come from B's
 * stream (lib/secondaryStream.ts): the lap so far, up to B's playhead.
 */
export interface SecondaryLapState {
  /** File A's name, for the column headers. */
  primaryFilename: string | null
  file: SecondaryFileData
  /** The lap both files are on; B shows its own lap with the same number. */
  lapNum: number | null
  lapData: AnalyzeLapData | null
  /** B has no lap with this number (red flag, shorter race, retirement). */
  missing: boolean
}
