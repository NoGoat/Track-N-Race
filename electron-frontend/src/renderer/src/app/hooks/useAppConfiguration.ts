import { useCallback, useEffect, useMemo, useState } from 'react'
import { flushSync } from 'react-dom'
import { useAppConfig } from '../../hooks/useAppConfig'
import { configureChartFrameRates, type ChartFrameRate } from '../../lib/timechart/frameRate'
import { DEFAULT_CHART_Y_AXIS, DEFAULT_COMPACT, DEFAULT_GRAPH_VIEW, type ChartYAxisState, type CompactState, type DensityMode, type GraphViewState, type TyreYAxisGroupState } from '../../lib/graphSections'

function normalizeDensity(val: unknown): DensityMode {
  if (val === true || val === 'compact') return 'compact'
  if (val === 'spacious') return 'spacious'
  return 'normal'
}
import { DEFAULT_CORE_LAYOUT, DEFAULT_INPUT_LAYOUT, DEFAULT_MISC_LAYOUT, DEFAULT_PAGE_LAYOUTS, DEFAULT_POWER_LAYOUT, DEFAULT_SESSION_LAYOUT, DEFAULT_STANDINGS_LAYOUT, DEFAULT_TYRES_LAYOUT, type ChartWindow, type CoreLayout, type InputLayout, type MiscLayout, type PageLayouts, type PowerLayout, type SessionLayout, type StandingsLayout, type Theme, type TitlebarUpdateInterval, type TyresLayout } from '../appConfig'

export function useAppConfiguration() {
  const [actualNativeTitlebar] = useState(() => window.electronStore.get('nativeTitlebar', false) as boolean)
  const [theme, rawSetTheme] = useAppConfig<Theme>('theme', 'dark')
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
  const [rawPageLayouts, setPageLayouts] = useAppConfig<PageLayouts>('pageLayouts', DEFAULT_PAGE_LAYOUTS)
  const [miscLayout, setMiscLayout] = useAppConfig<MiscLayout>('miscLayout', DEFAULT_MISC_LAYOUT)
  const [rawPowerLayout, setPowerLayout] = useAppConfig<PowerLayout>('powerLayout', DEFAULT_POWER_LAYOUT)
  const [rawSessionLayout, setSessionLayout] = useAppConfig<SessionLayout>('sessionLayout', DEFAULT_SESSION_LAYOUT)
  const [rawStandingsLayout, setStandingsLayout] = useAppConfig<StandingsLayout>('standingsLayout', DEFAULT_STANDINGS_LAYOUT)
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

  const setTheme = useCallback((nextTheme: Theme) => {
    if (nextTheme === theme) return
    const transitionDocument = document as Document & {
      startViewTransition?: (update: () => void) => { finished: Promise<unknown> }
    }
    const motionReduced = reduceAnimations
      || document.documentElement.dataset.reduceAnimations === 'true'

    if (motionReduced || !transitionDocument.startViewTransition) {
      document.documentElement.setAttribute('data-theme', nextTheme)
      rawSetTheme(nextTheme)
      return
    }

    try {
      const root = document.documentElement
      root.dataset.themeTransition = 'true'
      const transition = transitionDocument.startViewTransition(() => {
        flushSync(() => {
          root.setAttribute('data-theme', nextTheme)
          rawSetTheme(nextTheme)
        })
      })
      const clearThemeTransition = () => {
        delete root.dataset.themeTransition
      }
      void transition.finished.then(clearThemeTransition, clearThemeTransition)
    } catch {
      delete document.documentElement.dataset.themeTransition
      document.documentElement.setAttribute('data-theme', nextTheme)
      rawSetTheme(nextTheme)
    }
  }, [reduceAnimations, theme, rawSetTheme])

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
  const sessionLayout = useMemo<SessionLayout>(() => ({
    ...DEFAULT_SESSION_LAYOUT,
    ...(rawSessionLayout ?? {}),
    header: { ...DEFAULT_SESSION_LAYOUT.header, ...(rawSessionLayout?.header ?? {}) },
    sidebarPct: typeof rawSessionLayout?.sidebarPct === 'number'
      ? Math.max(15, Math.min(60, rawSessionLayout.sidebarPct))
      : DEFAULT_SESSION_LAYOUT.sidebarPct,
    statsCards: { ...DEFAULT_SESSION_LAYOUT.statsCards, ...(rawSessionLayout?.statsCards ?? {}) },
  }), [rawSessionLayout])
  const standingsLayout = useMemo<StandingsLayout>(() => ({
    ...DEFAULT_STANDINGS_LAYOUT,
    ...(rawStandingsLayout ?? {}),
    sidebarPct: typeof rawStandingsLayout?.sidebarPct === 'number'
      ? Math.max(15, Math.min(60, rawStandingsLayout.sidebarPct))
      : DEFAULT_STANDINGS_LAYOUT.sidebarPct,
    cards: { ...DEFAULT_STANDINGS_LAYOUT.cards, ...(rawStandingsLayout?.cards ?? {}) },
  }), [rawStandingsLayout])
  const graphView = useMemo<GraphViewState>(() => ({ ...DEFAULT_GRAPH_VIEW, ...(rawGraphView ?? {}) }), [rawGraphView])
  const pageLayouts = useMemo<PageLayouts>(() => ({
    input: rawPageLayouts?.input === 'vertical' ? 'vertical' : DEFAULT_PAGE_LAYOUTS.input,
  }), [rawPageLayouts])
  const compact = useMemo<CompactState>(() => {
    const raw = (rawCompact ?? {}) as unknown as Record<string, unknown>
    const legacyWeather = raw.sessionWeather
    const sessionWeather = typeof legacyWeather === 'boolean' ? (legacyWeather ? 2 : 0) : Math.max(0, Math.min(4, Number(legacyWeather) || 0))
    const legacyHeader = raw.sessionHeader
    const sessionHeader = typeof legacyHeader === 'boolean' ? (legacyHeader ? 1 : 0) : Math.max(0, Math.min(3, Number(legacyHeader) || 0))
    const overviewTyres = Math.max(0, Math.min(6, Number(raw.overviewTyres) || 0))
    return {
      overviewStats:     normalizeDensity(raw.overviewStats),
      overviewDamage:    normalizeDensity(raw.overviewDamage),
      overviewTyres,
      standingsTable:    normalizeDensity(raw.standingsTable),
      standingsTiming:   normalizeDensity(raw.standingsTiming),
      standingsErs:      normalizeDensity(raw.standingsErs),
      standingsStrategy: normalizeDensity(raw.standingsStrategy),
      sessionCards:      normalizeDensity(raw.sessionCards),
      sessionProximity:  normalizeDensity(raw.sessionProximity),
      sessionEvents:     normalizeDensity(raw.sessionEvents),
      sessionWeather,
      sessionHeader,
      powerCards:        normalizeDensity(raw.powerCards),
      strategySummary:   normalizeDensity(raw.strategySummary),
      playbackBar:       normalizeDensity(raw.playbackBar),
    }
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
    fpsInFocus, fpsOutOfFocus, graphView, inputLayout, mapDimmed, mapTimeout, miscLayout, pageLayouts,
    nativeTitlebar, powerLayout, reduceAnimations, seconds, sectorColors, sessionLayout, standingsLayout, titlebarUpdateInterval,
    setBannerDuration, setChartWindow, setChartYAxis, setCompact, setCoreLayout, setDriversMode,
    setFpsInFocus, setFpsOutOfFocus, setGraphView, setInputLayout, setMapDimmed,
    setMapTimeout, setMiscLayout, setNativeTitlebar, setPageLayouts, setPowerLayout, setReduceAnimations,
    setSectorColors, setSessionLayout, setStandingsLayout, setTheme, setTitlebarUpdateInterval, setTyreView, setTyreWearMode, setTyresLayout,
    theme, tyreView, tyreWearMode, tyresLayout,
  }
}
