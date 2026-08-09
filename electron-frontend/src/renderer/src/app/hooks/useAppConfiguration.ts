import { useEffect, useMemo, useState } from 'react'
import { useAppConfig } from '../../hooks/useAppConfig'
import { configureChartFrameRates, type ChartFrameRate } from '../../lib/timechart/frameRate'
import { DEFAULT_CHART_Y_AXIS, DEFAULT_COMPACT, DEFAULT_GRAPH_VIEW, type ChartYAxisState, type CompactState, type GraphViewState, type TyreYAxisGroupState } from '../../lib/graphSections'
import { DEFAULT_CORE_LAYOUT, DEFAULT_INPUT_LAYOUT, DEFAULT_MISC_LAYOUT, DEFAULT_POWER_LAYOUT, DEFAULT_TYRES_LAYOUT, type ChartWindow, type CoreLayout, type InputLayout, type MiscLayout, type PowerLayout, type TitlebarUpdateInterval, type TyresLayout } from '../appConfig'

export function useAppConfiguration() {
  const [actualNativeTitlebar] = useState(() => window.electronStore.get('nativeTitlebar', false) as boolean)
  const [theme, setTheme] = useAppConfig<'dark' | 'light'>('theme', 'dark')
  const [chartWindow, setChartWindow] = useAppConfig<ChartWindow>('chartWindow', (() => {
    const legacyMode = window.electronStore.get('chartWindowMode', 'time') as 'time' | 'CL'
    if (legacyMode === 'CL') return 'CL'
    return window.electronStore.get('timeWindow', 30) as number
  })())
  const seconds = typeof chartWindow === 'number' ? chartWindow : 30
  const [mapTimeout, setMapTimeout] = useAppConfig<number>('mapTimeout', 10)
  const [tyreView, setTyreView] = useAppConfig<'cards' | 'graphs'>('tyreView', 'cards')
  const [tyreWearMode, setTyreWearMode] = useAppConfig<'wear' | 'life'>('tyreWearMode', 'life')
  const [rawCoreLayout, setCoreLayout] = useAppConfig<CoreLayout>('coreLayout', DEFAULT_CORE_LAYOUT)
  const [inputLayout, setInputLayout] = useAppConfig<InputLayout>('inputLayout', DEFAULT_INPUT_LAYOUT)
  const [miscLayout, setMiscLayout] = useAppConfig<MiscLayout>('miscLayout', DEFAULT_MISC_LAYOUT)
  const [rawPowerLayout, setPowerLayout] = useAppConfig<PowerLayout>('powerLayout', DEFAULT_POWER_LAYOUT)
  const [rawTyresLayout, setTyresLayout] = useAppConfig<TyresLayout>('tyresLayout', DEFAULT_TYRES_LAYOUT)
  const [rawGraphView, setGraphView] = useAppConfig<GraphViewState>('graphView', DEFAULT_GRAPH_VIEW)
  const [rawCompact, setCompact] = useAppConfig<CompactState>('compact', DEFAULT_COMPACT)
  const [rawChartYAxis, setChartYAxis] = useAppConfig<ChartYAxisState>('tyreYAxis', DEFAULT_CHART_Y_AXIS)
  const [bannerDuration, setBannerDuration] = useAppConfig<number>('bannerDuration', 3)
  const [sectorColors, setSectorColors] = useAppConfig<boolean>('sectorColors', false)
  const [mapDimmed, setMapDimmed] = useAppConfig<boolean>('mapDimmed', false)
  const [nativeTitlebar, setNativeTitlebar] = useAppConfig<boolean>('nativeTitlebar', false)
  const [titlebarUpdateInterval, setTitlebarUpdateInterval] = useAppConfig<TitlebarUpdateInterval>('titlebarUpdateInterval', 0)
  const [reduceAnimations, setReduceAnimations] = useAppConfig<boolean>('reduceAnimations', false)
  const [fpsInFocus, setFpsInFocus] = useAppConfig<ChartFrameRate>('chartFpsInFocus', 'display')
  const [fpsOutOfFocus, setFpsOutOfFocus] = useAppConfig<ChartFrameRate>('chartFpsOutOfFocus', 30)
  const [driversMode, setDriversMode] = useAppConfig<'dots' | 'both' | 'labels'>('driversMode', (() => {
    const legacy = window.electronStore.get('showLabels', null) as boolean | null
    if (legacy === true) return 'both'
    if (legacy === false) return 'dots'
    return 'both'
  })())

  useEffect(() => { document.documentElement.setAttribute('data-theme', theme) }, [theme])
  useEffect(() => { configureChartFrameRates(fpsInFocus, fpsOutOfFocus) }, [fpsInFocus, fpsOutOfFocus])

  const coreLayout = useMemo<CoreLayout>(() => ({
    ...DEFAULT_CORE_LAYOUT, ...rawCoreLayout,
    statsCards: { ...DEFAULT_CORE_LAYOUT.statsCards, ...(rawCoreLayout?.statsCards ?? {}) },
    thermalGraphs: { ...DEFAULT_CORE_LAYOUT.thermalGraphs, ...(rawCoreLayout?.thermalGraphs ?? {}) },
    thermalCards: { ...DEFAULT_CORE_LAYOUT.thermalCards, ...(rawCoreLayout?.thermalCards ?? {}) },
    damageItems: { ...DEFAULT_CORE_LAYOUT.damageItems, ...(rawCoreLayout?.damageItems ?? {}) },
  }), [rawCoreLayout])
  const powerLayout = useMemo<PowerLayout>(() => ({
    statsCards: { ...DEFAULT_POWER_LAYOUT.statsCards, ...(rawPowerLayout?.statsCards ?? {}) },
    charts: { ...DEFAULT_POWER_LAYOUT.charts, ...(rawPowerLayout?.charts ?? {}) },
  }), [rawPowerLayout])
  const tyresLayout = useMemo<TyresLayout>(() => ({ charts: { ...DEFAULT_TYRES_LAYOUT.charts, ...(rawTyresLayout?.charts ?? {}) } }), [rawTyresLayout])
  const graphView = useMemo<GraphViewState>(() => ({ ...DEFAULT_GRAPH_VIEW, ...(rawGraphView ?? {}) }), [rawGraphView])
  const compact = useMemo<CompactState>(() => {
    const merged = { ...DEFAULT_COMPACT, ...(rawCompact ?? {}) }
    const legacyWeather = (rawCompact as unknown as { sessionWeather?: unknown } | null)?.sessionWeather
    merged.sessionWeather = typeof legacyWeather === 'boolean' ? (legacyWeather ? 2 : 0) : Math.max(0, Math.min(2, Number(merged.sessionWeather) || 0))
    return merged
  }, [rawCompact])
  const chartYAxis = useMemo<ChartYAxisState>(() => {
    const legacyRaw = (rawChartYAxis ?? {}) as unknown as Partial<TyreYAxisGroupState>
    const legacy: Partial<TyreYAxisGroupState> = {}
    for (const key of ['surfaceTemp', 'innerTemp', 'brakeTemp', 'tyreLife'] as const) {
      const value = legacyRaw[key]
      if (value === 'fixed' || value === 'dynamic') legacy[key] = value
    }
    return {
      overview: { ...DEFAULT_CHART_Y_AXIS.overview, ...legacy, ...(rawChartYAxis?.overview ?? {}) },
      tyres: { ...DEFAULT_CHART_Y_AXIS.tyres, ...legacy, ...(rawChartYAxis?.tyres ?? {}) },
      power: { ...DEFAULT_CHART_Y_AXIS.power, ...(rawChartYAxis?.power ?? {}) },
    }
  }, [rawChartYAxis])

  return {
    actualNativeTitlebar, bannerDuration, chartWindow, chartYAxis, compact, coreLayout, driversMode,
    fpsInFocus, fpsOutOfFocus, graphView, inputLayout, mapDimmed, mapTimeout, miscLayout,
    nativeTitlebar, powerLayout, reduceAnimations, seconds, sectorColors, titlebarUpdateInterval,
    setBannerDuration, setChartWindow, setChartYAxis, setCompact, setCoreLayout, setDriversMode,
    setFpsInFocus, setFpsOutOfFocus, setGraphView, setInputLayout, setMapDimmed,
    setMapTimeout, setMiscLayout, setNativeTitlebar, setPowerLayout, setReduceAnimations,
    setSectorColors, setTheme, setTitlebarUpdateInterval, setTyreView, setTyreWearMode, setTyresLayout,
    theme, tyreView, tyreWearMode, tyresLayout,
  }
}
