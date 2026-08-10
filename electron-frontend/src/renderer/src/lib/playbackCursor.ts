// Imperative playback cursor shared with hot render paths. The player state
// arrives up to 60 times per second; consumers such as the comparison map read
// this ref inside rAF instead of subscribing React to every tick.
let playbackCursorTime: number | null = null

export function setPlaybackCursorTime(value: number | null): void {
  playbackCursorTime = value
}

export function getPlaybackCursorTime(): number | null {
  return playbackCursorTime
}
