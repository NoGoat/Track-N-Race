import type { DistanceChartMode } from '../app/appConfig'

/** `label` names a comparison no window mode accounts for (file B's background trace). */
export function getChartComparisonLabel(mode: DistanceChartMode | null, label?: string | null): string | null {
  if (label) return label
  if (mode === 'PL') return 'Previous lap'
  if (mode === 'FL') return 'Fastest lap'
  if (mode === 'RL') return 'Reference lap'
  return null
}

export function formatChartComparisonTooltip(
  values: number[] | undefined,
  mode: DistanceChartMode | null,
  formatValues: (values: number[]) => string,
  comparisonLabel?: string | null,
): string {
  const label = getChartComparisonLabel(mode, comparisonLabel)
  if (!values || !label) return ''
  return [
    `<div style="color:var(--text-secondary);border-top:1px solid var(--border);margin-top:5px;padding-top:4px">${label}</div>`,
    `<div style="opacity:0.35">${formatValues(values)}</div>`,
  ].join('')
}
