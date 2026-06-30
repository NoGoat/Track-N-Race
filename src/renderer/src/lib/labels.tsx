import { createContext, useContext, useMemo, type ReactNode } from 'react'

// Renderer side of the library-owned i18n catalog. The full catalog arrives in
// the `protocol_status` control row (see tnrp/Labels.h) and is provided here via
// context; components resolve every enum/label through `t(key)`. The catalog is
// re-emitted on format change, so labels re-theme live when 2025↔2026 switches.
//
// FALLBACK_LABELS is a minimal snapshot used only before the first
// protocol_status arrives (pre-connect), so the UI never renders raw keys. The
// library remains the single source of truth at runtime.
const FALLBACK_LABELS: Record<string, string> = {
  'tyre.actual.7': 'INT', 'tyre.actual.8': 'WET',
  'tyre.actual.16': 'C5', 'tyre.actual.17': 'C4', 'tyre.actual.18': 'C3',
  'tyre.actual.19': 'C2', 'tyre.actual.20': 'C1', 'tyre.actual.21': 'C0',
  'tyre.actual.22': 'C6',
  'tyre.visual.7': 'INT', 'tyre.visual.8': 'WET',
  'tyre.visual.16': 'Soft', 'tyre.visual.17': 'Medium', 'tyre.visual.18': 'Hard',
  'ers.mode.0': 'None', 'ers.mode.1': 'Auto', 'ers.mode.2': 'Hotlap', 'ers.mode.3': 'Overtake',
  'drs.label': 'DRS',
}

const LabelsContext = createContext<Record<string, string>>(FALLBACK_LABELS)

export function LabelsProvider({
  labels,
  children,
}: {
  labels?: Record<string, string> | null
  children: ReactNode
}): React.JSX.Element {
  // Library catalog wins; fallback fills any gap before the first packet.
  const merged = useMemo(
    () => ({ ...FALLBACK_LABELS, ...(labels ?? {}) }),
    [labels],
  )
  return <LabelsContext.Provider value={merged}>{children}</LabelsContext.Provider>
}

export interface Labels {
  /** Resolve a label key; returns the key itself if unknown (so gaps are visible). */
  t: (key: string) => string
  /** Convenience for numeric-suffixed enum groups, e.g. tn('tyre.actual', c). */
  tn: (group: string, n: number) => string
  /** The raw resolved catalog (for rare bulk needs). */
  raw: Record<string, string>
}

export function useLabels(): Labels {
  const raw = useContext(LabelsContext)
  return useMemo<Labels>(
    () => ({
      t: (key) => raw[key] ?? key,
      tn: (group, n) => raw[`${group}.${n}`] ?? String(n),
      raw,
    }),
    [raw],
  )
}
