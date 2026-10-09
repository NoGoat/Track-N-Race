import { timeChartFrameScheduler } from './engine/core/frameScheduler'
import type { TimeChartFrameRate } from './engine/core/frameScheduler'

export type ChartFrameRate = TimeChartFrameRate

export function configureChartFrameRates(
  focused: ChartFrameRate,
  unfocused: ChartFrameRate,
): void {
  timeChartFrameScheduler.configureFrameRates(focused, unfocused)
}
