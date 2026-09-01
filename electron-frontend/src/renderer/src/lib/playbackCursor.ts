// Imperative playback cursor shared with hot render paths. The player state
// arrives up to 60 times per second; consumers such as the comparison map read
// this ref inside rAF, while charts use the imperative listener to schedule
// direct buffer updates without subscribing React to every tick.
let playbackCursorTime: number | null = null
const playbackCursorListeners = new Set<() => void>()

export function setPlaybackCursorTime(value: number | null): void {
  playbackCursorTime = value
  for (const listener of playbackCursorListeners) listener()
}

export function getPlaybackCursorTime(): number | null {
  return playbackCursorTime
}

export function subscribePlaybackCursor(listener: () => void): () => void {
  playbackCursorListeners.add(listener)
  return () => { playbackCursorListeners.delete(listener) }
}
