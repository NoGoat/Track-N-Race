import type { DistanceChartMode } from '../app/appConfig'

export function formatChartComparisonTooltip(
  values: number[] | undefined,
  mode: DistanceChartMode | null,
  formatValues: (values: number[]) => string,
): string {
  if (!values || (mode !== 'PL' && mode !== 'FL')) return ''
  const label = mode === 'PL' ? 'Previous lap' : 'Fastest lap'
  return [
    `<div style="color:var(--text-secondary);border-top:1px solid var(--border);margin-top:5px;padding-top:4px">${label}</div>`,
    `<div style="opacity:0.35">${formatValues(values)}</div>`,
  ].join('')
}
