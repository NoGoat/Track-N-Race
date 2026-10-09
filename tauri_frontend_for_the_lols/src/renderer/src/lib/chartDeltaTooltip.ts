import { DEFAULT_DELTA_NEGATIVE_COLOR, DEFAULT_DELTA_POSITIVE_COLOR } from './analyzeMetrics'

export function formatChartDeltaTooltip(
  delta: number,
  positiveColor = DEFAULT_DELTA_POSITIVE_COLOR,
  negativeColor = DEFAULT_DELTA_NEGATIVE_COLOR,
): string {
  if (!Number.isFinite(delta)) return ''
  const color = delta >= 0 ? positiveColor : negativeColor
  return `<div style="margin-top:5px"><span style="color:${color}">Delta</span>: ${delta >= 0 ? '+' : ''}${delta.toFixed(3)} s</div>`
}
