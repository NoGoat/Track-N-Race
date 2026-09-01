import AnalyzeTimeChart, { type AnalyzeTimeChartProps } from './AnalyzeTimeChart'

type Props = AnalyzeTimeChartProps

export default function AnalyzeStackedTimeCharts(props: Props) {
  const hasVisibleMetric = props.selected.some(item =>
    item.visible && (item.metricId !== 'delta' || (props.distanceMode && props.comparisonSelected)),
  )

  return <div className="absolute inset-0 overflow-hidden">
    <div
      aria-hidden={hasVisibleMetric}
      className={`analysis-chart-empty-state pointer-events-none absolute inset-0 z-10 flex items-center justify-center text-[10px] uppercase tracking-widest text-[var(--text-secondary)] ${hasVisibleMetric ? '' : 'analysis-chart-empty-state--visible'}`}
    >
      No visible metrics
    </div>
    <AnalyzeTimeChart {...props} stackedMode />
  </div>
}
