import type { DistanceChartMode } from '../app/appConfig'

export function getChartComparisonLabel(mode: DistanceChartMode | null): string | null {
  if (mode === 'PL') return 'Previous lap'
  if (mode === 'FL') return 'Fastest lap'
  if (mode === 'RL') return 'Reference lap'
  return null
}

export function formatChartComparisonTooltip(
  values: number[] | undefined,
  mode: DistanceChartMode | null,
  formatValues: (values: number[]) => string,
): string {
  const label = getChartComparisonLabel(mode)
  if (!values || !label) return ''
  return [
    `<div style="color:var(--text-secondary);border-top:1px solid var(--border);margin-top:5px;padding-top:4px">${label}</div>`,
    `<div style="opacity:0.35">${formatValues(values)}</div>`,
  ].join('')
}
