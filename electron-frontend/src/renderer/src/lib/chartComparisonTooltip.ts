import type { DistanceChartMode } from '../app/appConfig'

export function formatChartComparisonTooltip(
  values: number[] | undefined,
  mode: DistanceChartMode | null,
  formatValues: (values: number[]) => string,
): string {
  if (!values || (mode !== 'PL' && mode !== 'FL' && mode !== 'RL')) return ''
  const label = mode === 'PL' ? 'Previous lap' : mode === 'FL' ? 'Fastest lap' : 'Reference lap'
  return [
    `<div style="color:var(--text-secondary);border-top:1px solid var(--border);margin-top:5px;padding-top:4px">${label}</div>`,
    `<div style="opacity:0.35">${formatValues(values)}</div>`,
  ].join('')
}
