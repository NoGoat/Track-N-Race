import TimeChart from 'timechart'

// We build on `TimeChart.core` (not the default `TimeChart`) so we control the
// exact plugin set: the default export auto-injects d3Axis, legend, the zoom
// plugin (which would enable pan/zoom — we keep it disabled to match uPlot) and
// a built-in tooltip. We keep only lineChart + crosshair + nearestPoint and add
// our own axis / reference-line / tooltip / profiler plugins.
export type TChart = InstanceType<typeof TimeChart.core>

export const corePlugins = {
  lineChart: TimeChart.plugins.lineChart,
  crosshair: TimeChart.plugins.crosshair,
  nearestPoint: TimeChart.plugins.nearestPoint,
}

export { TimeChart }
