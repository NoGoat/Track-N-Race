import type { TyreSetsMsg } from '../../types'
import { useSecondaryState } from '../../lib/secondaryStream'

/**
 * File B's tyre allocation at its moment on the lap-synced playhead, for the
 * car followed in B: streamed with B's other rows (lib/secondaryStream.ts), as
 * file A's arrives in its playback. Null before B's stream has one.
 */
export function useSecondaryTyreSets(): TyreSetsMsg | null {
  return useSecondaryState().tyreSets
}
