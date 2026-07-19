import TimeChartCore from './engine/core'
import { LineType } from './engine/options'
import { lineChart } from './engine/plugins/lineChart'
import { crosshair } from './engine/plugins/crosshair'
import { nearestPoint } from './engine/plugins/nearestPoint'

// Local facade over the vendored TimeChart fork. Importing the engine pieces
// directly keeps the unused upstream axis, legend, zoom and tooltip plugins out
// of the renderer bundle while preserving the small API used by our charts.
export type TChart = InstanceType<typeof TimeChartCore>

export const corePlugins = {
  lineChart,
  crosshair,
  nearestPoint,
}

export const TimeChart = {
  core: TimeChartCore,
  LineType,
}

export type { TimeChartPlugin } from './engine/plugins'
