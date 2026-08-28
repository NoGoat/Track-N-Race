import { useCallback, useEffect, useLayoutEffect, useMemo, useState } from 'react'
import { flushSync } from 'react-dom'
import { setAnalyzeLapEnabled, setHistoryRowMask, setTelemetrySeconds, useTelemetryStore } from '../stores/telemetryStore'
import Settings from '../components/Settings'
import type { AnalyzeFixedLapMode } from '../components/AnalyzeScreen'
import { getChartWindowOptionGroups, TAB_OPTIONS, type ChartWindow, type Tab } from './appConfig'
import { useAppConfiguration } from './hooks/useAppConfiguration'
import { useWindowState } from './hooks/useWindowState'
import { usePlayback } from './hooks/usePlayback'
import { useRaceBanners } from './hooks/useRaceBanners'
import AppHeader from './components/AppHeader'
import AppHeaderMacOS from './components/AppHeaderMacOS'
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
  type ChartReferenceLapOverrides,
  type ChartWindowOverrides,
} from '../lib/chartWindowOverrides'
import type { GraphSection } from '../lib/graphSections'

export default function AppShell() {
  const Header = window.platform === 'darwin' ? AppHeaderMacOS : AppHeader
  const {
    actualNativeTitlebar, bannerDuration, chartWindow, chartYAxis, compact, coreLayout, driversMode,
    fpsInFocus, fpsOutOfFocus, graphView, inputCursorSyncEnabled, inputLayout, mapDimmed, mapTimeout, miscLayout, pageLayouts,
    nativeTitlebar, powerLayout, reduceAnimations, seconds, sectorColors, titlebarUpdateInterval,
    setBannerDuration, setChartWindow, setChartYAxis, setCompact, setCoreLayout, setDriversMode,
    setFpsInFocus, setFpsOutOfFocus, setGraphView, setInputCursorSyncEnabled, setInputLayout, setMapDimmed,
    setMapTimeout, setMiscLayout, setNativeTitlebar, setPageLayouts, setPowerLayout, setReduceAnimations,
    setSectorColors, setSessionLayout, setStandingsLayout, setTheme, setTitlebarUpdateInterval, setTyreView, setTyreWearMode, setTyresLayout,
    sessionLayout, standingsLayout, theme, tyreView, tyreWearMode, tyresLayout,
  } = useAppConfiguration()
  const [tab, setTab] = useState<Tab>('core')
  const [settingsOpen, setSettingsOpen] = useState(false)
  const [selectedIdx, setSelectedIdx] = useState<number | null>(null)
  const [editOpen, setEditOpen] = useState(false)
  const [recordingError, setRecordingError] = useState<RecordingErrorMsg | null>(null)
  const [availableUpdate, setAvailableUpdate] = useState<AvailableUpdate | null>(null)
  const { headerVisible, isFullscreen, isMaximized, setHeaderVisible } = useWindowState()
  const [analyzeCompareLapNum, setAnalyzeCompareLapNum] = useState<number | null>(null)
  const [referenceLapNum, setReferenceLapNum] = useState<number | null>(1)
  const [analyzeFixedLapMode, setAnalyzeFixedLapMode] = useState<AnalyzeFixedLapMode>({ enabled: false, lapA: null, lapB: null })
  const [analyzeDataMask, setAnalyzeDataMask] = useState(0)
  const [chartWindowOverrides, setChartWindowOverrides] = useState<ChartWindowOverrides>({})
  const [chartReferenceLapOverrides, setChartReferenceLapOverrides] = useState<ChartReferenceLapOverrides>({})
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

  const handleTabChange = useCallback((nextTab: Tab) => {
    if (nextTab === tab) return
    const transitionDocument = document as Document & {
      startViewTransition?: (update: () => void) => { finished: Promise<unknown> }
    }
    const motionReduced = reduceAnimations
      || document.documentElement.dataset.reduceAnimations === 'true'

    if (motionReduced || !transitionDocument.startViewTransition) {
      setTab(nextTab)
      return
    }

    try {
      const root = document.documentElement
      const currentIndex = TAB_OPTIONS.findIndex(option => option.value === tab)
      const nextIndex = TAB_OPTIONS.findIndex(option => option.value === nextTab)
      root.dataset.pageTransition = 'true'
      root.dataset.pageTransitionDirection = nextIndex > currentIndex ? 'right' : 'left'
      const transition = transitionDocument.startViewTransition(() => {
        flushSync(() => setTab(nextTab))
      })
      const clearPageTransition = () => {
        delete root.dataset.pageTransition
        delete root.dataset.pageTransitionDirection
      }
      void transition.finished.then(clearPageTransition, clearPageTransition)
    } catch {
      delete document.documentElement.dataset.pageTransition
      delete document.documentElement.dataset.pageTransitionDirection
      setTab(nextTab)
    }
  }, [reduceAnimations, tab])

  useEffect(() => window.recordingBridge.onError(setRecordingError), [])

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
    setAnalyzeFixedLapMode({ enabled: false, lapA: null, lapB: null })
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
  const clCapability = !playback.state?.filename
    ? 'live'
    : playbackTnrdVersion === null
      ? 'loading'
      : recordingCurrentLapSupported
        ? 'supported'
        : 'legacy'
  const clAvailable = clCapability !== 'legacy'
  const recordingOpen = !!playback.state?.filename
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
    ),
    [tab, coreLayout, inputLayout, miscLayout, powerLayout, tyresLayout, tyreView, playback.state?.filename, analyzeDataMask],
  )
  const visibleChartSections = useMemo(() => visibleChartSectionsForUi(
    tab, coreLayout, inputLayout, pageLayouts, miscLayout, powerLayout, tyresLayout, tyreView,
  ), [tab, coreLayout, inputLayout, pageLayouts, miscLayout, powerLayout, tyresLayout, tyreView])
  const visibleChartWindows = useMemo(() => visibleChartSections.length > 0
    ? visibleChartSections.map(section => chartWindowOverrides[section] ?? chartWindow)
    : [chartWindow],
  [chartWindow, chartWindowOverrides, visibleChartSections])
  useEffect(() => {
    // Analysis is always scoped to the current lap. Its distance-axis charts
    // consume the store's dedicated analyzeLap* slices, so the title-bar time
    // window must not truncate the native seek preload (15s/30s/etc.) or turn
    // it into an unnecessarily large full-session AL preload.
    const analysisLapScope = tab === 'analyze'
    const fullLapHistoryEnabled = !analysisLapScope && visibleChartWindows.some(value => value === 'AL' || value === 'SL')
    const stintLapsEnabled = !analysisLapScope && visibleChartWindows.some(value => value === 'SL')
    const hasLapWindow = visibleChartWindows.some(value => typeof value !== 'number' && value !== 'AL' && value !== 'SL')
    const finiteWindows = visibleChartWindows.filter((value): value is number => typeof value === 'number')
    const maxFiniteWindow = finiteWindows.length > 0 ? Math.max(...finiteWindows) : seconds
    const streamMask = stintLapsEnabled ? dataRequirements.streamMask | DATA_ROW.status : dataRequirements.streamMask
    const historyMask = stintLapsEnabled ? dataRequirements.historyMask | DATA_ROW.status : dataRequirements.historyMask
    // A mixed lap/time page seeks the current lap first. The renderer then
    // requests the older finite prefix additively only when that prefix starts
    // before the lap, so overlapping V4 blocks are not decoded unnecessarily.
    const mixedLapAndTime = hasLapWindow && finiteWindows.length > 0
    const historyWindowSeconds = analysisLapScope
      ? 0
      : fullLapHistoryEnabled
        ? -1
        : hasLapWindow ? 0 : maxFiniteWindow
    setHistoryRowMask(historyMask)
    setAnalyzeLapEnabled(analysisLapScope || hasLapWindow)
    window.playerBridge.setDataRequirements(
      streamMask,
      historyMask,
      historyWindowSeconds,
    )
    window.playerBridge.setAllLapsMode(
      fullLapHistoryEnabled,
      historyMask,
      analysisLapScope ? 0 : hasLapWindow ? 0 : maxFiniteWindow,
    )
    setTelemetrySeconds(
      fullLapHistoryEnabled ? Infinity : maxFiniteWindow,
      !analysisLapScope && (finiteWindows.length > 0 || mixedLapAndTime),
    )
  }, [dataRequirements, seconds, tab, visibleChartWindows])

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
        onClosePlayback={playback.close}
        onSelectPlaybackFile={playback.selectFile}
        chartWindow={chartWindow}
        clAvailable={clAvailable}
        referenceLapNum={referenceLapNum}
        referenceLapOptions={referenceLapOptions}
        setEditOpen={setEditOpen}
        setHeaderVisible={setHeaderVisible}
        setInputCursorSyncEnabled={setInputCursorSyncEnabled}
        setChartWindow={handleGlobalChartWindowChange}
        setReferenceLapNum={handleGlobalReferenceLapChange}
        setSettingsOpen={setSettingsOpen}
        setTab={handleTabChange}
        settingsOpen={settingsOpen}
        tab={tab}
        theme={theme}
        titlebarUpdateInterval={titlebarUpdateInterval}
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
        >
        <ChartCoordinatesProvider mode={chartCoordinateMode} referenceLapNum={referenceLapNum} rowTypeMask={dataRequirements.historyMask}>
        <TabContent
          tab={tab}
          isDark={theme !== 'light'}
          seconds={seconds}
          coreLayout={coreLayout}
          powerLayout={powerLayout}
          sessionLayout={sessionLayout}
          standingsLayout={standingsLayout}
          tyresLayout={tyresLayout}
          inputLayout={inputLayout}
          inputCursorSyncEnabled={inputCursorSyncEnabled}
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
          analyzeFixedLapMode={analyzeFixedLapMode}
          onAnalyzeFixedLapModeChange={setAnalyzeFixedLapMode}
          onAnalyzeDataMaskChange={setAnalyzeDataMask}
        />
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
