import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from 'react'
import type { AnalyzeLapData } from '../types'
import type { SecondaryFileData } from '../components/AnalyzeScreen'
import { DATA_ROW } from './historyDependencies'
import { fetchSecondaryLap, lapHasFields, recordingDateLabel, secondaryFileKey } from './secondaryFile'

/*
 * File B's laps as reference laps. With side-by-side charts off, B's laps are
 * listed in the chart lap selectors under B's date; picking one draws that lap
 * as the comparison trace over file A's charts.
 *
 * The selectors carry plain lap numbers, so a B lap is encoded as
 * SECONDARY_LAP_BASE + its lap number.
 */
export const SECONDARY_LAP_BASE = 1_000_000

export function isSecondaryReferenceLap(value: number | null | undefined): value is number {
  return typeof value === 'number' && value >= SECONDARY_LAP_BASE
}

export function secondaryReferenceLap(lapNum: number): number {
  return SECONDARY_LAP_BASE + lapNum
}

export function secondaryLapNumber(value: number): number {
  return value - SECONDARY_LAP_BASE
}

export interface ReferenceLapOption {
  value: number
  label: string
  /** The file's date label ("10 Oct"); set only when two files are open. */
  group?: string
}

interface ReferenceLapGroup {
  label: string
  file: 'A' | 'B'
  options: ReferenceLapOption[]
}

/** The lap menu: flat with one file open, one group per file with two. */
export function groupReferenceLapOptions(options: ReferenceLapOption[]): ReferenceLapOption[] | ReferenceLapGroup[] {
  if (!options.some(option => isSecondaryReferenceLap(option.value))) return options
  const a = options.filter(option => !isSecondaryReferenceLap(option.value))
  const b = options.filter(option => isSecondaryReferenceLap(option.value))
  return [
    { label: a[0]?.group ?? 'File A', file: 'A', options: a },
    { label: b[0]?.group ?? 'File B', file: 'B', options: b },
  ]
}

/** B's laps read in B's colour, in the menu and once picked. */
export function formatReferenceLapOption(option: { value: number; label: string }): ReactNode {
  return isSecondaryReferenceLap(option.value)
    ? <span style={{ color: 'var(--file-b)' }}>{option.label}</span>
    : option.label
}

export function formatReferenceLapGroup(group: { label?: string; file?: 'A' | 'B' }): ReactNode {
  return <span style={{ color: group.file === 'B' ? 'var(--file-b)' : 'var(--file-a)' }}>{group.label}</span>
}

// An unanswered lap is asked again this many times (engine still indexing).
const MAX_ATTEMPTS = 20
// B laps kept; picked reference laps and the background's two laps fit easily.
const MAX_CACHED_LAPS = 8

type Entry = AnalyzeLapData | 'missing'

interface ComparisonValue {
  laps: ReadonlyMap<number, Entry>
  request: (lapNum: number, rowTypeMask: number) => void
  /** Side-by-side off: B's matching lap is drawn behind A's in Current Lap and time windows. */
  background: boolean
  /** B's date ("13 Sep"), naming its trace in tooltips. */
  label: string
  file: SecondaryFileData | null
  /** File A's name, for its date beside B's. */
  primaryFilename: string | null
  /** The V6 fields B's laps are read with (bit n = V6 type n; 0 = every field). */
  v6TypeMask: number
}

const ComparisonContext = createContext<ComparisonValue | null>(null)

/**
 * One cache of B's reference laps for every chart, keyed by B's lap number.
 * Like file A's history, a lap holds only the row types charts asked for and
 * the V6 fields in `v6TypeMask` (the page's; 0 = every field); a chart needing
 * more has the lap read again with both.
 */
export function SecondaryComparisonProvider({ file, primaryFilename, background, v6TypeMask = 0, children }: {
  file: SecondaryFileData | null; primaryFilename: string | null; background: boolean; v6TypeMask?: number; children: ReactNode
}) {
  // Another file, or another car followed in it, starts a fresh cache.
  const filePath = file ? secondaryFileKey(file) : null
  const driverIndex = file?.driverIndex ?? -1
  const [cache, setCache] = useState<{ filePath: string | null; laps: Map<number, Entry> }>({ filePath, laps: new Map() })
  if (cache.filePath !== filePath) setCache({ filePath, laps: new Map() })
  const cacheRef = useRef(cache)
  useEffect(() => { cacheRef.current = cache })
  // What is already being read per lap, so overlapping asks are not repeated.
  const inflight = useRef(new Map<string, { mask: number; types: number }>())

  const request = useCallback((lapNum: number, rowTypeMask: number) => {
    if (!filePath) return
    const key = `${filePath}\n${lapNum}`
    const cached = cacheRef.current.filePath === filePath ? cacheRef.current.laps.get(lapNum) : undefined
    if (cached === 'missing') return
    if (cached && lapHasFields(cached, rowTypeMask, v6TypeMask)) return
    // Read again whole, with what the lap held plus what is asked for now: a
    // lap read with only some fields is replaced, not merged.
    const fetchMask = ((cached ? cached.rowTypeMask ?? 0 : 0) | rowTypeMask) >>> 0
    const fetchTypes = cached && cached.v6TypeMask !== undefined && v6TypeMask !== 0
      ? (cached.v6TypeMask | v6TypeMask) >>> 0 : v6TypeMask
    const pending = inflight.current.get(key)
    if (pending && ((pending.mask & fetchMask) >>> 0) === fetchMask &&
        (pending.types === 0 || (fetchTypes !== 0 && ((pending.types & fetchTypes) >>> 0) === fetchTypes))) return
    const ask = { mask: fetchMask, types: fetchTypes }
    inflight.current.set(key, ask)
    let attempts = 0
    const settle = () => {
      if (inflight.current.get(key) === ask) inflight.current.delete(key)
    }
    const finish = (lap: AnalyzeLapData | 'missing') => {
      settle()
      setCache(current => {
        if (current.filePath !== filePath) return current
        const entry = lap !== 'missing' && (lap.telemetry.length > 0 || lap.lapProgress.length > 0) ? lap : 'missing'
        const laps = new Map(current.laps)
        laps.delete(lapNum)
        laps.set(lapNum, entry)
        while (laps.size > MAX_CACHED_LAPS) laps.delete(laps.keys().next().value!)
        return { filePath, laps }
      })
    }
    const retry = (delayMs: number) => {
      if (++attempts >= MAX_ATTEMPTS) finish('missing')
      else setTimeout(attempt, delayMs)
    }
    const attempt = () => {
      fetchSecondaryLap(lapNum, fetchMask | DATA_ROW.lap, driverIndex, fetchTypes).then(lap => {
        // No answer: the engine is still indexing file B.
        if (!lap) return retry(500)
        finish(lap)
      }).catch(() => retry(1000))
    }
    attempt()
  }, [filePath, driverIndex, v6TypeMask])

  const label = (file ? recordingDateLabel(file.filename) : null) ?? 'Second file'
  const value = useMemo<ComparisonValue>(() => ({ laps: cache.laps, request, background: background && filePath !== null, label, file, primaryFilename, v6TypeMask }),
    [background, cache.laps, file, filePath, label, primaryFilename, request, v6TypeMask])
  return <ComparisonContext.Provider value={value}>{children}</ComparisonContext.Provider>
}

/**
 * File B's lap `lapNum` (B's own lap number), holding at least the rows in
 * `rowTypeMask`; fetched on first use. Null until it arrives, or when B lacks it.
 */
export function useSecondaryLapData(lapNum: number | null, rowTypeMask: number): AnalyzeLapData | null {
  const context = useContext(ComparisonContext)
  const entry = lapNum !== null ? context?.laps.get(lapNum) : undefined
  const request = context?.request
  const requiredMask = (rowTypeMask | DATA_ROW.lap) >>> 0
  const complete = entry === 'missing' || (entry !== undefined && lapHasFields(entry, requiredMask, context?.v6TypeMask ?? 0))
  useEffect(() => {
    if (lapNum !== null && lapNum >= 1 && !complete) request?.(lapNum, requiredMask)
  }, [complete, lapNum, request, requiredMask])
  return entry && entry !== 'missing' ? entry : null
}

/** File B's lap for an encoded reference lap (see SECONDARY_LAP_BASE); null for A's laps. */
export function useSecondaryComparisonLap(referenceLap: number | null, rowTypeMask: number): AnalyzeLapData | null {
  return useSecondaryLapData(isSecondaryReferenceLap(referenceLap) ? secondaryLapNumber(referenceLap) : null, rowTypeMask)
}

/** File B while it is drawn behind A's charts (side-by-side off); otherwise null. */
export function useSecondaryBackgroundFile(): { file: SecondaryFileData; primaryFilename: string | null } | null {
  const context = useContext(ComparisonContext)
  return context?.background && context.file ? { file: context.file, primaryFilename: context.primaryFilename } : null
}

/** Whether B's lap is drawn behind A's, and the name its trace goes by. */
export function useSecondaryBackground(): { enabled: boolean; label: string } {
  const context = useContext(ComparisonContext)
  return { enabled: context?.background ?? false, label: context?.label ?? 'Second file' }
}

/** File B whenever it is open (side-by-side on or off), and file A's name beside it. */
export function useSecondaryOpenFile(): { file: SecondaryFileData; primaryFilename: string | null } | null {
  const context = useContext(ComparisonContext)
  return context?.file ? { file: context.file, primaryFilename: context.primaryFilename } : null
}
