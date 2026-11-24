// Two recordings on one lap-synced timeline. The table itself is built by the
// engine (tnrp/MergedLapTimeline.h) so every frontend applies the same lap
// rules; this file only does the arithmetic on it.

export interface MergedSpan {
  start: number
  end: number
}

export interface MergedSegment {
  kind: 'pre' | 'lap' | 'post'
  lap_num: number
  /** Merged seconds from the timeline's origin. */
  start: number
  duration: number
  /** Each file's part, in its own session time; absent = that file holds. */
  a?: MergedSpan
  b?: MergedSpan
}

export interface MergedTimeline {
  total: number
  a_last_lap: number
  b_last_lap: number
  segments: MergedSegment[]
}

export interface FilePosition {
  /** The file's session time shown at this merged time; null before its first frame. */
  time: number | null
  /** The file has finished its part of this segment (or has none) and holds its last frame. */
  holding: boolean
}

export interface MergedPosition {
  time: number
  segmentIndex: number
  segment: MergedSegment
  /** Seconds into the segment. */
  offset: number
  a: FilePosition
  b: FilePosition
}

export function parseMergedTimeline(json: string | null): MergedTimeline | null {
  if (!json) return null
  try {
    const value = JSON.parse(json) as Partial<MergedTimeline> & { type?: string }
    if (value.type !== 'merged_lap_timeline' || !Array.isArray(value.segments) || value.segments.length === 0) return null
    return { total: value.total ?? 0, a_last_lap: value.a_last_lap ?? 0, b_last_lap: value.b_last_lap ?? 0, segments: value.segments }
  } catch {
    return null
  }
}

/** The segment holding merged time `time` (the last one when past the end). */
export function segmentIndexAt(timeline: MergedTimeline, time: number): number {
  const segments = timeline.segments
  let lo = 0, hi = segments.length - 1
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1
    if (segments[mid].start <= time) lo = mid
    else hi = mid - 1
  }
  return lo
}

/**
 * One file's position at a merged time: inside its span it plays; after its
 * span (or in a segment it has no part in) it holds the last frame it showed.
 */
function filePosition(timeline: MergedTimeline, index: number, offset: number, pick: 'a' | 'b'): FilePosition {
  const span = timeline.segments[index][pick]
  if (span) {
    const length = span.end - span.start
    return offset <= length ? { time: span.start + offset, holding: false } : { time: span.end, holding: true }
  }
  for (let i = index - 1; i >= 0; i--) {
    const earlier = timeline.segments[i][pick]
    if (earlier) return { time: earlier.end, holding: true }
  }
  return { time: null, holding: true }
}

export function positionAt(timeline: MergedTimeline, time: number): MergedPosition {
  const clamped = Math.max(0, Math.min(timeline.total, time))
  const segmentIndex = segmentIndexAt(timeline, clamped)
  const segment = timeline.segments[segmentIndex]
  const offset = Math.max(0, clamped - segment.start)
  return {
    time: clamped,
    segmentIndex,
    segment,
    offset,
    a: filePosition(timeline, segmentIndex, offset, 'a'),
    b: filePosition(timeline, segmentIndex, offset, 'b'),
  }
}

/** The segment whose file-A span holds A's session time, or -1. */
function segmentIndexForA(timeline: MergedTimeline, aTime: number): number {
  const segments = timeline.segments
  for (let i = 0; i < segments.length; i++) {
    const span = segments[i].a
    if (span && aTime >= span.start && aTime < span.end) return i
  }
  // Exactly on (or past) the final line: the last segment A plays in.
  for (let i = segments.length - 1; i >= 0; i--) {
    const span = segments[i].a
    if (span && aTime >= span.end) return i
  }
  return -1
}

/** Merged time while file A is playing (not holding) at `aTime`. */
export function mergedTimeForA(timeline: MergedTimeline, aTime: number): number | null {
  const index = segmentIndexForA(timeline, aTime)
  if (index < 0) return null
  const segment = timeline.segments[index]
  const span = segment.a!
  return segment.start + Math.min(Math.max(0, aTime - span.start), span.end - span.start)
}

/** "1:15.8": minutes and tenths, for the lap readout. */
export function formatMergedClock(seconds: number): string {
  const total = Math.max(0, seconds)
  const minutes = Math.floor(total / 60)
  const rest = total - minutes * 60
  return `${minutes}:${rest.toFixed(1).padStart(4, '0')}`
}
