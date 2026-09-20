import { startTransition, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'
import { flushSync } from 'react-dom'
import { setAnalyzeLapEnabled, setHistoryRowMask, setTelemetrySeconds, useTelemetryStore } from '../stores/telemetryStore'
import Settings from '../components/Settings'
import type { AnalysisDriverSelection, AnalyzeFixedLapMode, SecondaryFileData } from '../components/AnalyzeScreen'
import { getChartWindowOptionGroups, TAB_OPTIONS, type ChartWindow, type Tab } from './appConfig'
import { useAppConfiguration } from './hooks/useAppConfiguration'
import { useWindowState } from './hooks/useWindowState'
import { usePlayback } from './hooks/usePlayback'
import { useRaceBanners } from './hooks/useRaceBanners'
import AppHeader from './components/AppHeader'
import AppHeaderMacOS from './components/AppHeaderMacOS'
import PageMountShell from './components/PageMountShell'
import PlaybackBar from './components/PlaybackBar'
import TabContent from './components/TabContent'
import FullscreenBanner from './components/FullscreenBanner'
import StatusOverlays from './components/StatusOverlays'
import LayoutEditor from './components/LayoutEditor'
import PlaybackDialogs from './components/PlaybackDialogs'
import RaceLeaderWatcher from './components/RaceLeaderWatcher'
import RecordingErrorDialog from './components/RecordingErrorDialog'
import UpdateAvailableDialog from './components/UpdateAvailableDialog'
import type { AvailableUpdate, RecordingErrorMsg } from '../types'
import { ChartCoordinatesProvider } from '../lib/chartCoordinates'
import { DATA_ROW, dataRequirementsForUi, visibleChartSectionsForUi } from '../lib/historyDependencies'
import {
  ChartWindowOverridesProvider,
  GRAPH_SECTION_ROW_MASK,
  type ChartReferenceLapOverrides,
  type ChartWindowOverrides,
} from '../lib/chartWindowOverrides'
import type { GraphSection } from '../lib/graphSections'

const LIVE_LAP_FAMILY_MASK = DATA_ROW.telemetry | DATA_ROW.status |
  DATA_ROW.damage | DATA_ROW.lap | DATA_ROW.motion | DATA_ROW.motionEx

type AppPageTransition = {
  ready: Promise<unknown>
  finished: Promise<unknown>
  skipTransition?: () => void
}

export default function AppShell() {
  const Header = window.platform === 'darwin' ? AppHeaderMacOS : AppHeader
  const {
    actualNativeTitlebar, bannerDuration, chartWindow, chartYAxis, compact, coreLayout, driversMode,
    fpsInFocus, fpsOutOfFocus, graphView, inputCursorSyncEnabled, inputLayout, mapDimmed, mapTimeout, miscLayout, pageLayouts,
    nativeTitlebar, powerLayout, reduceAnimations, secondaryHorizontalCrosshairEnabled, secondaryVerticalCrosshairEnabled, seconds, sectorBoundariesEnabled, sectorColors, titlebarUpdateInterval,
    setBannerDuration, setChartWindow, setChartYAxis, setCompact, setCoreLayout, setDriversMode,
    setFpsInFocus, setFpsOutOfFocus, setGraphView, setInputCursorSyncEnabled, setInputLayout, setMapDimmed,
    setMapTimeout, setMiscLayout, setNativeTitlebar, setPageLayouts, setPowerLayout, setReduceAnimations,
    setSecondaryHorizontalCrosshairEnabled, setSecondaryVerticalCrosshairEnabled, setSectorBoundariesEnabled, setSectorColors, setSessionLayout, setStandingsLayout, setTheme, setTitlebarUpdateInterval, setTyreView, setTyreWearMode, setTyresLayout,
    sessionLayout, standingsLayout, theme, tyreView, tyreWearMode, tyresLayout,
  } = useAppConfiguration()
  const [tab, setTab] = useState<Tab>('core')
  const [mountedTab, setMountedTab] = useState<Tab | null>('core')
  const [settingsOpen, setSettingsOpen] = useState(false)
  const [selectedIdx, setSelectedIdx] = useState<number | null>(null)
  const [playbackDriverIdx, setPlaybackDriverIdx] = useState<number | null>(null)
  const [recordedPlayerIdx, setRecordedPlayerIdx] = useState<number | null>(null)
  const [editOpen, setEditOpen] = useState(false)
  const [recordingError, setRecordingError] = useState<RecordingErrorMsg | null>(null)
  const [availableUpdate, setAvailableUpdate] = useState<AvailableUpdate | null>(null)
  const [udpListenerError, setUdpListenerError] = useState<string | null>(null)
  const { headerVisible, isFullscreen, isMaximized, setHeaderVisible } = useWindowState()
  const [analyzeCompareLapNum, setAnalyzeCompareLapNum] = useState<number | null>(null)
  const [analyzeCompareDriver, setAnalyzeCompareDriver] = useState<AnalysisDriverSelection | null>(null)
  const [analyzeSecondaryFile, setAnalyzeSecondaryFile] = useState<SecondaryFileData | null>(null)
  const [referenceLapNum, setReferenceLapNum] = useState<number | null>(1)
  const [analyzeFixedLapMode, setAnalyzeFixedLapMode] = useState<AnalyzeFixedLapMode>({
    enabled: false, lapA: null, lapB: null, lapADriver: null, lapBDriver: null,
  })
  const [analyzeDataMask, setAnalyzeDataMask] = useState(0)
  const [chartWindowOverrides, setChartWindowOverrides] = useState<ChartWindowOverrides>({})
  const [chartReferenceLapOverrides, setChartReferenceLapOverrides] = useState<ChartReferenceLapOverrides>({})
  const activePageTransitionRef = useRef<AppPageTransition | null>(null)
  const pageMountGenerationRef = useRef(0)
  const handlePlaybackClosed = useCallback(() => setSelectedIdx(null), [])
  const playback = usePlayback(handlePlaybackClosed)

  useLayoutEffect(() => {
    const root = document.documentElement
    if (reduceAnimations) root.dataset.reduceAnimations = 'true'
    else delete root.dataset.reduceAnimations
    return () => { delete root.dataset.reduceAnimations }
  }, [reduceAnimations])

  const setChartWindowOverride = useCallback((section: GraphSection, value: ChartWindow | null) => {
    setChartWindowOverrides(current => {
      if (value === null) {
        if (!(section in current)) return current
        const next = { ...current }
        delete next[section]
        return next
      }
      return current[section] === value ? current : { ...current, [section]: value }
    })
  }, [])
  const handleGlobalChartWindowChange = useCallback((value: ChartWindow) => {
    setChartWindowOverrides({})
    setChartReferenceLapOverrides({})
    setChartWindow(value)
  }, [setChartWindow])
  const setChartReferenceLapOverride = useCallback((section: GraphSection, lapNum: number | null) => {
    setChartReferenceLapOverrides(current => {
      if (lapNum === null) {
        if (!(section in current)) return current
        const next = { ...current }
        delete next[section]
        return next
      }
      return current[section] === lapNum ? current : { ...current, [section]: lapNum }
    })
  }, [])
  const handleGlobalReferenceLapChange = useCallback((lapNum: number | null) => {
    setChartReferenceLapOverrides({})
    setReferenceLapNum(lapNum)
  }, [])

  const schedulePageMount = useCallback((nextTab: Tab, generation: number) => {
    requestAnimationFrame(() => requestAnimationFrame(() => {
      if (pageMountGenerationRef.current !== generation) return
      startTransition(() => setMountedTab(nextTab))
    }))
  }, [])

  const handleTabChange = useCallback((nextTab: Tab) => {
    if (nextTab === tab) return
    const generation = ++pageMountGenerationRef.current
    const transitionDocument = document as Document & {
      startViewTransition?: (update: () => void) => AppPageTransition
    }
    const root = document.documentElement
    const motionReduced = reduceAnimations
      || root.dataset.reduceAnimations === 'true'

    if (motionReduced || !transitionDocument.startViewTransition) {
      activePageTransitionRef.current?.skipTransition?.()
      activePageTransitionRef.current = null
      delete root.dataset.pageTransition
      delete root.dataset.pageTransitionDirection
      delete root.dataset.pageTransitionPhase
      setMountedTab(null)
      setTab(nextTab)
      schedulePageMount(nextTab, generation)
      return
    }

    activePageTransitionRef.current?.skipTransition?.()
    root.dataset.pageTransition = 'true'
    root.dataset.pageTransitionPhase = 'preparing'
    try {
      const currentIndex = TAB_OPTIONS.findIndex(option => option.value === tab)
      const nextIndex = TAB_OPTIONS.findIndex(option => option.value === nextTab)
      root.dataset.pageTransitionDirection = nextIndex > currentIndex ? 'right' : 'left'
      const transition = transitionDocument.startViewTransition(() => {
        if (pageMountGenerationRef.current !== generation) return
        flushSync(() => {
          setMountedTab(null)
          setTab(nextTab)
        })
      })
      activePageTransitionRef.current = transition
      void transition.ready.then(() => {
        requestAnimationFrame(() => requestAnimationFrame(() => {
          if (activePageTransitionRef.current !== transition) return
          root.dataset.pageTransitionPhase = 'running'
        }))
      }, () => {})
      const clearPageTransition = () => {
        if (activePageTransitionRef.current !== transition) return
        activePageTransitionRef.current = null
        delete root.dataset.pageTransition
        delete root.dataset.pageTransitionDirection
        delete root.dataset.pageTransitionPhase
        schedulePageMount(nextTab, generation)
      }
      void transition.finished.then(clearPageTransition, clearPageTransition)
    } catch {
      activePageTransitionRef.current = null
      delete root.dataset.pageTransition
      delete root.dataset.pageTransitionDirection
      delete root.dataset.pageTransitionPhase
      if (pageMountGenerationRef.current !== generation) return
      setMountedTab(null)
      setTab(nextTab)
      schedulePageMount(nextTab, generation)
    }
  }, [reduceAnimations, schedulePageMount, tab])

  useEffect(() => window.recordingBridge.onError(setRecordingError), [])

  useEffect(() => {
    let cancelled = false
    void window.udpBridge.getStatus().then(status => {
      if (!cancelled) setUdpListenerError(status.ok ? null : status.error ?? 'UDP listener failed')
    }).catch(error => {
      console.warn('[udp] could not request listener status:', error)
    })
    const unsubscribe = window.udpBridge.onStatusChange(status => {
      setUdpListenerError(status.ok ? null : status.error ?? 'UDP listener failed')
    })
    return () => {
      cancelled = true
      unsubscribe()
    }
  }, [])

  useEffect(() => {
    let cancelled = false
    void window.updateBridge.checkOnStartup().then(update => {
      if (!cancelled && update) setAvailableUpdate(update)
    }).catch(error => {
      console.warn('[updates] could not request the startup update check:', error)
    })
    return () => { cancelled = true }
  }, [])

  useEffect(() => {
    setAnalyzeCompareLapNum(null)
    setAnalyzeCompareDriver(null)
    setAnalyzeSecondaryFile(null)
    setAnalyzeFixedLapMode({
      enabled: false, lapA: null, lapB: null, lapADriver: null, lapBDriver: null,
    })
    setReferenceLapNum(null)
  }, [playback.state?.filename])

  const handleCloseSettings = useCallback(() => {
    setSettingsOpen(false)
  }, [])

  // App is deliberately COLD: it selects only low-frequency slices. Every hot,
  // per-frame slice is read inside <TabContent/> and the other subscriber
  // components below, so a telemetry frame never re-renders App itself.
  const protocolStatus  = useTelemetryStore(s => s.protocolStatus)
  const protocolWarning = useTelemetryStore(s => s.protocolWarning)
  const recordingCurrentLapSupported = useTelemetryStore(s => s.analyzeDeltaAvailable)
  const playbackTnrdVersion = useTelemetryStore(s => s.playbackTnrdVersion)
  const playbackDriverIndex = useTelemetryStore(s => s.playbackDriverIndex)
  const participants = useTelemetryStore(s => s.participants)
  const timingPlayerIdx = useTelemetryStore(s => s.timing?.player_idx ?? null)
  const clCapability = !playback.state?.filename
    ? 'live'
    : playbackTnrdVersion === null
      ? 'loading'
      : recordingCurrentLapSupported
        ? 'supported'
        : 'legacy'
  const clAvailable = clCapability !== 'legacy'
  const recordingOpen = !!playback.state?.filename
  const driverSelectorVisible = recordingOpen && playbackTnrdVersion === 'TNRD_V6'
  const originalPlayerIdx = recordedPlayerIdx ?? playbackDriverIndex ?? timingPlayerIdx
  const driverOptions = useMemo(() => (participants?.drivers ?? []).map(driver => {
    const restricted = driver.idx !== originalPlayerIdx && driver.your_telemetry !== 1
    return {
      value: driver.idx,
      label: restricted ? `${driver.name} · Public data only` : driver.name,
      isDisabled: false,
    }
  }), [participants, originalPlayerIdx])
  useEffect(() => {
    setPlaybackDriverIdx(null)
    setRecordedPlayerIdx(null)
  }, [playback.state?.filename])
  useEffect(() => {
    if (!driverSelectorVisible) {
      setPlaybackDriverIdx(null)
      return
    }
    const initialPlayerIdx = playbackDriverIndex ?? timingPlayerIdx
    if (recordedPlayerIdx === null && initialPlayerIdx !== null) {
      setRecordedPlayerIdx(initialPlayerIdx)
      setPlaybackDriverIdx(initialPlayerIdx)
      window.playerBridge.setDriver(initialPlayerIdx, true)
    }
  }, [driverSelectorVisible, playbackDriverIndex, recordedPlayerIdx, timingPlayerIdx])
  const availableChartWindows = useMemo(() => new Set(
    getChartWindowOptionGroups(clAvailable, recordingOpen)
      .flatMap(group => group.options)
      .map(option => option.value),
  ), [clAvailable, recordingOpen])
  useEffect(() => {
    setChartWindowOverrides(current => {
      const next = Object.fromEntries(Object.entries(current)
        .filter(([, value]) => availableChartWindows.has(value))) as ChartWindowOverrides
      return Object.keys(next).length === Object.keys(current).length ? current : next
    })
    setChartReferenceLapOverrides(current => {
      const next = Object.fromEntries(Object.entries(current)
        .filter(([section]) => {
          const graphSection = section as GraphSection
          const effectiveWindow = chartWindowOverrides[graphSection] ?? chartWindow
          return effectiveWindow === 'RL' && availableChartWindows.has(effectiveWindow)
        })) as ChartReferenceLapOverrides
      return Object.keys(next).length === Object.keys(current).length ? current : next
    })
  }, [availableChartWindows, chartWindow, chartWindowOverrides])
  const chartCoordinateMode = chartWindow === 'AL' || chartWindow === 'SL'
    ? chartWindow
    : clAvailable && typeof chartWindow !== 'number' && (chartWindow !== 'RL' || recordingOpen)
      ? chartWindow
      : null
  const referenceLapOptions = useMemo(() => {
    const lapNumbers = (playback.speedRpmBlocks ?? [])
      .map(block => Number(block.lapNum))
      .filter(Number.isFinite)
    return [...new Set(lapNumbers)]
      .sort((a, b) => a - b)
      .map(value => ({ value, label: String(value) }))
  }, [playback.speedRpmBlocks])

  useEffect(() => {
    // The lap catalog arrives after the playback header. Keep RL on its Lap 1
    // default while loading instead of clearing it to the dash placeholder.
    if (referenceLapOptions.length > 0 &&
        (referenceLapNum === null || !referenceLapOptions.some(option => option.value === referenceLapNum))) {
      setReferenceLapNum(referenceLapOptions.find(option => option.value === 1)?.value ?? referenceLapOptions[0].value)
    }
  }, [referenceLapNum, referenceLapOptions])
  // Publish the visible time window to the store so it computes the right slices.
  const dataRequirements = useMemo(
    () => dataRequirementsForUi(
      tab, coreLayout, inputLayout, miscLayout, powerLayout, tyresLayout, tyreView,
      Boolean(playback.state?.filename),
      analyzeDataMask,
      pageLayouts,
    ),
    [tab, coreLayout, inputLayout, miscLayout, powerLayout, tyresLayout, tyreView, playback.state?.filename, analyzeDataMask, pageLayouts],
  )
  const visibleChartSections = useMemo(() => visibleChartSectionsForUi(
    tab, coreLayout, inputLayout, pageLayouts, miscLayout, powerLayout, tyresLayout, tyreView,
  ), [tab, coreLayout, inputLayout, pageLayouts, miscLayout, powerLayout, tyresLayout, tyreView])
  const visibleChartScopes = useMemo(() => visibleChartSections.map(section => ({
    mask: GRAPH_SECTION_ROW_MASK[section],
    window: chartWindowOverrides[section] ?? chartWindow,
  })),
  [chartWindow, chartWindowOverrides, visibleChartSections])
  useEffect(() => {
    // Analysis is always scoped to the current lap. Its distance-axis charts
    // consume the store's dedicated analyzeLap* slices, so the title-bar time
    // window must not truncate the native seek preload (15s/30s/etc.) or turn
    // it into an unnecessarily large full-session AL preload.
    const analysisLapScope = tab === 'analyze'
    const fullLapScopes = analysisLapScope
      ? []
      : visibleChartScopes.filter(scope => scope.window === 'AL' || scope.window === 'SL')
    const fullLapHistoryEnabled = fullLapScopes.length > 0
    const stintLapsEnabled = fullLapScopes.some(scope => scope.window === 'SL')
    const lapWindowScopes = visibleChartScopes.filter(scope =>
      typeof scope.window !== 'number' && scope.window !== 'AL' && scope.window !== 'SL')
    const hasLapWindow = lapWindowScopes.length > 0
    const finiteScopes = visibleChartScopes.filter(
      (scope): scope is { mask: number; window: number } => typeof scope.window === 'number')
    const finiteWindows = finiteScopes.map(scope => scope.window)
    // A page with no visible chart has no time range to request. Its stream
    // subscription is restored from the latest rows at the current cursor.
    const maxFiniteWindow = finiteWindows.length > 0 ? Math.max(...finiteWindows) : 0
    // Live Previous/Fastest selectors can be changed after a lap completes, so
    // retain every chart family for the small uncompressed lap working set.
    // Historical AL decompression remains restricted to historyMask below.
    const liveLapMask = playback.state?.filename || visibleChartScopes.length === 0
      ? 0
      : LIVE_LAP_FAMILY_MASK
    const streamMask = (stintLapsEnabled
      ? dataRequirements.streamMask | DATA_ROW.status
      : dataRequirements.streamMask) | liveLapMask
    const lapMetadataMask = fullLapHistoryEnabled || hasLapWindow ? DATA_ROW.lap : 0
    const historyMask = (stintLapsEnabled
      ? dataRequirements.historyMask | DATA_ROW.status
      : dataRequirements.historyMask) | lapMetadataMask
    const fullSessionHistoryMask = fullLapHistoryEnabled
      ? (fullLapScopes.reduce((mask, scope) => mask | scope.mask, 0) |
        DATA_ROW.lap | (stintLapsEnabled ? DATA_ROW.status : 0)) >>> 0
      : 0
    const finiteHistoryMask = finiteScopes.reduce((mask, scope) => mask | scope.mask, 0) >>> 0
    const lapWindowHistoryMask = (lapWindowScopes.reduce((mask, scope) => mask | scope.mask, 0) |
      (hasLapWindow ? DATA_ROW.lap : 0)) >>> 0
    // A mixed lap/time page seeks the current lap first. The renderer then
    // requests the older finite prefix additively only when that prefix starts
    // before the lap, so overlapping V4 blocks are not decoded unnecessarily.
    const mixedLapAndTime = hasLapWindow && finiteWindows.length > 0
    const historyWindowSeconds = analysisLapScope
      ? 0
      : fullLapHistoryEnabled
          ? -1
          : hasLapWindow ? 0 : maxFiniteWindow
    const v6HistoryTypes = [...new Set([
      ...dataRequirements.v6HistoryTypes,
      ...(stintLapsEnabled ? [13, 15] : []),
      ...(lapMetadataMask ? [24] : []),
    ])]
    window.playerBridge.setDataRequirements(
      streamMask,
      historyMask,
      historyWindowSeconds,
      [...new Set([
        ...dataRequirements.v6Types,
        ...(stintLapsEnabled ? [13, 15] : []),
        ...(lapMetadataMask ? [24] : []),
      ])],
      v6HistoryTypes,
    )
    setHistoryRowMask(
      historyMask,
      fullSessionHistoryMask,
      finiteHistoryMask,
      finiteWindows.length > 0 ? maxFiniteWindow : 0,
      lapWindowHistoryMask,
      v6HistoryTypes,
    )
    setAnalyzeLapEnabled(analysisLapScope || hasLapWindow)
    window.playerBridge.setAllLapsMode(
      fullLapHistoryEnabled,
      analysisLapScope
        ? historyMask
        : fullLapHistoryEnabled
          ? fullSessionHistoryMask
          : hasLapWindow
            ? lapWindowHistoryMask
            : historyMask,
      analysisLapScope || fullLapHistoryEnabled || hasLapWindow ? 0 : maxFiniteWindow,
    )
    setTelemetrySeconds(
      fullLapHistoryEnabled ? Infinity : maxFiniteWindow,
      !analysisLapScope && (finiteWindows.length > 0 || mixedLapAndTime),
    )
  }, [dataRequirements, seconds, tab, visibleChartScopes, playback.state?.filename])

  // A renderer that mounts after the engine already settled on a format never
  // receives the one-shot protocol_status push, so pull the last one when we
  // have no catalog. (Ported from the old useTelemetry hook.)
  useEffect(() => {
    if (!protocolStatus) window.protocolBridge.requestStatus()
  }, [protocolStatus])

  const detectedGameLabel = useMemo(() => {
    if (!protocolStatus) return 'No data yet'
    const { detected_format, active_format, override } = protocolStatus
    if (detected_format) {
      return `${detected_format}`
    }
    if (override !== 'auto' && active_format) {
      return `${active_format} (manual)`
    }
    if (active_format) {
      return `${active_format} (last session)`
    }
    return 'No data yet'
  }, [protocolStatus])

  const detectedWarningFormat = protocolWarning?.detected_format ?? null
  const forcedWarningFormat = protocolWarning?.forced_format ?? null

  const { activeBanner, handleLeaderChange } = useRaceBanners(bannerDuration)

  const handleSelectDriver = useCallback((idx: number) => {
    setSelectedIdx(prev => prev === idx ? null : idx)
  }, [])

  const handlePlaybackDriverChange = useCallback((idx: number) => {
    if (!driverSelectorVisible || idx === playbackDriverIdx) return
    const option = driverOptions.find(candidate => candidate.value === idx)
    if (!option) return
    setPlaybackDriverIdx(idx)
    setSelectedIdx(idx)
    window.playerBridge.setDriver(idx, idx === originalPlayerIdx)
    if (playback.state) playback.seekProgress(playback.state.progressPct)
  }, [driverOptions, driverSelectorVisible, originalPlayerIdx, playback, playbackDriverIdx])

  return (
    <div className="h-dvh bg-[var(--bg-base)] text-[var(--text-primary)] flex flex-col relative">
      {window.platform !== 'darwin' && (
        <FullscreenBanner banner={activeBanner} headerVisible={headerVisible} isFullscreen={isFullscreen} />
      )}

      <Header
        actualNativeTitlebar={actualNativeTitlebar}
        activeBanner={activeBanner}
        editOpen={editOpen}
        filename={playback.state?.filename}
        headerVisible={headerVisible}
        isFullscreen={isFullscreen}
        isMaximized={isMaximized}
        inputCursorSyncEnabled={inputCursorSyncEnabled}
        sectorBoundariesEnabled={sectorBoundariesEnabled}
        onClosePlayback={playback.close}
        onSelectPlaybackFile={playback.selectFile}
        chartWindow={chartWindow}
        driverOptions={driverOptions}
        driverSelectorVisible={driverSelectorVisible}
        selectedDriverIdx={playbackDriverIdx}
        clAvailable={clAvailable}
        referenceLapNum={referenceLapNum}
        referenceLapOptions={referenceLapOptions}
        setEditOpen={setEditOpen}
        setHeaderVisible={setHeaderVisible}
        setInputCursorSyncEnabled={setInputCursorSyncEnabled}
        setSectorBoundariesEnabled={setSectorBoundariesEnabled}
        setChartWindow={handleGlobalChartWindowChange}
        setSelectedDriverIdx={handlePlaybackDriverChange}
        setReferenceLapNum={handleGlobalReferenceLapChange}
        setSettingsOpen={setSettingsOpen}
        setTab={handleTabChange}
        settingsOpen={settingsOpen}
        tab={tab}
        theme={theme}
        titlebarUpdateInterval={titlebarUpdateInterval}
        udpListenerError={udpListenerError}
      />

      <StatusOverlays
        exportProgress={playback.exportProgress}
        exportStage={playback.exportStage}
        exportState={playback.exportState}
        isScanning={!!playback.state?.isScanning}
      />

      <LayoutEditor
        coreLayout={coreLayout}
        editOpen={editOpen}
        inputLayout={inputLayout}
        pageLayouts={pageLayouts}
        miscLayout={miscLayout}
        powerLayout={powerLayout}
        sessionLayout={sessionLayout}
        standingsLayout={standingsLayout}
        setCoreLayout={setCoreLayout}
        setEditOpen={setEditOpen}
        setInputLayout={setInputLayout}
        setMiscLayout={setMiscLayout}
        setPowerLayout={setPowerLayout}
        setSessionLayout={setSessionLayout}
        setStandingsLayout={setStandingsLayout}
        setTyresLayout={setTyresLayout}
        tab={tab}
        tyreView={tyreView}
        tyreWearMode={tyreWearMode}
        tyresLayout={tyresLayout}
      />

      {/* Settings Modal */}
      <Settings
        isOpen={settingsOpen}
        onClose={handleCloseSettings}
        tyreView={tyreView}
        onTyreViewChange={setTyreView}
        tyreWearMode={tyreWearMode}
        onTyreWearModeChange={setTyreWearMode}
        bannerDuration={bannerDuration}
        onBannerDurationChange={setBannerDuration}
        theme={theme}
        onThemeChange={setTheme}
        sectorColors={sectorColors}
        onSectorColorsChange={setSectorColors}
        driversMode={driversMode}
        onDriversModeChange={setDriversMode}
        mapTimeout={mapTimeout}
        onMapTimeoutChange={setMapTimeout}
        detectedGameLabel={detectedGameLabel}
        detectedWarningFormat={detectedWarningFormat}
        forcedWarningFormat={forcedWarningFormat}
        nativeTitlebar={nativeTitlebar}
        onNativeTitlebarChange={setNativeTitlebar}
        titlebarUpdateInterval={titlebarUpdateInterval}
        onTitlebarUpdateIntervalChange={setTitlebarUpdateInterval}
        reduceAnimations={reduceAnimations}
        onReduceAnimationsChange={setReduceAnimations}
        fpsInFocus={fpsInFocus}
        onFpsInFocusChange={setFpsInFocus}
        fpsOutOfFocus={fpsOutOfFocus}
        onFpsOutOfFocusChange={setFpsOutOfFocus}
        mapDimmed={mapDimmed}
        onMapDimmedChange={setMapDimmed}
        pageLayouts={pageLayouts}
        onPageLayoutsChange={setPageLayouts}
        secondaryHorizontalCrosshairEnabled={secondaryHorizontalCrosshairEnabled}
        onSecondaryHorizontalCrosshairEnabledChange={setSecondaryHorizontalCrosshairEnabled}
        secondaryVerticalCrosshairEnabled={secondaryVerticalCrosshairEnabled}
        onSecondaryVerticalCrosshairEnabledChange={setSecondaryVerticalCrosshairEnabled}
        graphView={graphView}
        onGraphViewChange={setGraphView}
        compact={compact}
        onCompactChange={setCompact}
        chartYAxis={chartYAxis}
        onChartYAxisChange={setChartYAxis}
      />

      {/* Content */}
      <RaceLeaderWatcher enabled={!playback.state?.filename} onLeaderChange={handleLeaderChange} />
      <main className="app-page-transition flex-1 min-h-0">
        <ChartWindowOverridesProvider
          globalWindow={chartWindow}
          overrides={chartWindowOverrides}
          setOverride={setChartWindowOverride}
          clAvailable={clAvailable}
          recordingOpen={recordingOpen}
          referenceLapNum={referenceLapNum}
          referenceLapOptions={referenceLapOptions}
          referenceLapOverrides={chartReferenceLapOverrides}
          setReferenceLapOverride={setChartReferenceLapOverride}
          sectorBoundariesEnabled={sectorBoundariesEnabled}
        >
        <ChartCoordinatesProvider mode={chartCoordinateMode} referenceLapNum={referenceLapNum} rowTypeMask={dataRequirements.historyMask} sectorBoundaries={sectorBoundariesEnabled}>
        {mountedTab === null ? <PageMountShell /> : <TabContent
          tab={mountedTab}
          isDark={theme !== 'light'}
          seconds={seconds}
          coreLayout={coreLayout}
          powerLayout={powerLayout}
          sessionLayout={sessionLayout}
          standingsLayout={standingsLayout}
          tyresLayout={tyresLayout}
          inputLayout={inputLayout}
          inputCursorSyncEnabled={inputCursorSyncEnabled}
          secondaryHorizontalCrosshairEnabled={secondaryHorizontalCrosshairEnabled}
          secondaryVerticalCrosshairEnabled={secondaryVerticalCrosshairEnabled}
          pageLayouts={pageLayouts}
          miscLayout={miscLayout}
          graphView={graphView}
          compact={compact}
          chartYAxis={chartYAxis}
          tyreView={tyreView}
          tyreWearMode={tyreWearMode}
          selectedIdx={selectedIdx}
          onSelectDriver={handleSelectDriver}
          reduceAnimations={reduceAnimations}
          sectorColors={sectorColors}
          driversMode={driversMode}
          mapTimeout={mapTimeout}
          mapDimmed={mapDimmed}
          currentPlaybackLapNum={playback.currentLapNum}
          playbackFilename={playback.state?.filename ?? null}
          analyzeCompareLapNum={analyzeCompareLapNum}
          onAnalyzeCompareLapChange={setAnalyzeCompareLapNum}
          analyzeCompareDriver={analyzeCompareDriver}
          onAnalyzeCompareDriverChange={setAnalyzeCompareDriver}
          analyzeSecondaryFile={analyzeSecondaryFile}
          onAnalyzeSecondaryFileChange={setAnalyzeSecondaryFile}
          analyzeFixedLapMode={analyzeFixedLapMode}
          onAnalyzeFixedLapModeChange={setAnalyzeFixedLapMode}
          onAnalyzeDataMaskChange={setAnalyzeDataMask}
        />}
        </ChartCoordinatesProvider>
        </ChartWindowOverridesProvider>
      </main>

      {/* Playback Controls Bar */}
      {playback.state && playback.state.filename && (
        <PlaybackBar
          compact={compact.playbackBar}
          currentLapNum={playback.currentLapNum}
          exportError={playback.exportError}
          exportState={playback.exportState}
          onExport={playback.exportXlsx}
          onSeekProgress={playback.seekProgress}
          onSeekBackward={playback.seekBackward}
          onSeekForward={playback.seekForward}
          onSpeedChange={playback.setSpeed}
          onTogglePlay={playback.togglePlay}
          sessionFileStart={playback.sessionFileStart}
          speedRpmBlocks={playback.speedRpmBlocks}
          state={playback.state}
        />
      )}

      <PlaybackDialogs
        confirmOpenFilePath={playback.confirmOpenFilePath}
        loadError={playback.loadError}
        setConfirmOpenFilePath={playback.setConfirmOpenFilePath}
        setLoadError={playback.setLoadError}
      />

      <RecordingErrorDialog
        error={recordingError}
        onClose={() => setRecordingError(null)}
      />

      <UpdateAvailableDialog
        update={availableUpdate}
        onClose={() => setAvailableUpdate(null)}
      />

    </div>
  )
}
