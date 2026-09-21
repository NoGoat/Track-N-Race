// Imperative cursor for the Analysis comparison clock. The comparison map owns
// that clock (it drives the playback bar and the track markers); the split-view
// charts read it from here inside their own rAF loop so a 60 Hz scrubber never
// re-renders React, exactly as the playback cursor keeps its hot path out of
// the store.
let elapsedSource: (() => number) | null = null

/** Publish the clock; the returned function withdraws this exact source. */
export function setAnalyzeCursorSource(source: () => number): () => void {
  elapsedSource = source
  return () => { if (elapsedSource === source) elapsedSource = null }
}

export function getAnalyzeCursorElapsed(): number | null {
  return elapsedSource ? elapsedSource() : null
}
