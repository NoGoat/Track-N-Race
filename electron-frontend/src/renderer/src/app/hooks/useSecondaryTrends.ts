import { useSecondaryState, type SecondaryTrends } from '../../lib/secondaryStream'

export type { SecondaryTrends }

/**
 * File B's Trends inputs, for the car followed in B: its completed laps and
 * their ERS and fuel use up to B's moment on the playhead. Streamed at each of
 * B's new laps (lap_trends, lib/secondaryStream.ts), as the engine pushes file
 * A's lap history. Null before B's stream has sent them.
 */
export function useSecondaryTrends(): SecondaryTrends | null {
  return useSecondaryState().trends
}
