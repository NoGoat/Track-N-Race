import { useCallback, useEffect, useMemo, useState } from 'react'
import { setHistoryRowMask, setTelemetrySeconds, useTelemetryStore } from '../stores/telemetryStore'
import Settings from '../components/Settings'
import type { AnalyzeFixedLapMode } from '../components/AnalyzeScreen'
import type { Tab } from './appConfig'
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
import type { RecordingErrorMsg } from '../types'
import { ChartCoordinatesProvider } from '../lib/chartCoordinates'
import { dataRequirementsForUi } from '../lib/historyDependencies'

export default function AppShell() {
  const Header = window.platform === 'darwin' ? AppHeaderMacOS : AppHeader
  const {
    actualNativeTitlebar, bannerDuration, chartWindow, chartYAxis, compact, coreLayout, driversMode,
    fpsInFocus, fpsOutOfFocus, graphView, inputLayout, mapDimmed, mapTimeout, miscLayout,
    nativeTitlebar, powerLayout, reduceAnimations, seconds, sectorColors, titlebarUpdateInterval,
    setBannerDuration, setChartWindow, setChartYAxis, setCompact, setCoreLayout, setDriversMode,
    setFpsInFocus, setFpsOutOfFocus, setGraphView, setInputLayout, setMapDimmed,
    setMapTimeout, setMiscLayout, setNativeTitlebar, setPowerLayout, setReduceAnimations,
    setSectorColors, setTheme, setTitlebarUpdateInterval, setTyreView, setTyreWearMode, setTyresLayout,
    theme, tyreView, tyreWearMode, tyresLayout,
  } = useAppConfiguration()
  const [tab, setTab] = useState<Tab>('core')
  const [settingsOpen, setSettingsOpen] = useState(false)
  const [selectedIdx, setSelectedIdx] = useState<number | null>(null)
  const [editOpen, setEditOpen] = useState(false)
  const [recordingError, setRecordingError] = useState<RecordingErrorMsg | null>(null)
  const { headerVisible, isFullscreen, isMaximized, setHeaderVisible } = useWindowState()
  const [analyzeCompareLapNum, setAnalyzeCompareLapNum] = useState<number | null>(null)
  const [referenceLapNum, setReferenceLapNum] = useState<number | null>(null)
  const [analyzeFixedLapMode, setAnalyzeFixedLapMode] = useState<AnalyzeFixedLapMode>({ enabled: false, lapA: null, lapB: null })
  const [analyzeDataMask, setAnalyzeDataMask] = useState(0)
  const handlePlaybackClosed = useCallback(() => setSelectedIdx(null), [])
  const playback = usePlayback(handlePlaybackClosed)

  useEffect(() => window.recordingBridge.onError(setRecordingError), [])

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
  const chartCoordinateMode = chartWindow === 'AL'
    ? 'AL'
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
    if (referenceLapNum !== null && !referenceLapOptions.some(option => option.value === referenceLapNum)) {
      setReferenceLapNum(null)
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
  useEffect(() => {
    const enabled = chartWindow === 'AL'
    setHistoryRowMask(dataRequirements.historyMask)
    window.playerBridge.setDataRequirements(
      dataRequirements.streamMask,
      dataRequirements.historyMask,
      chartWindow === 'AL' ? -1 : typeof chartWindow === 'number' ? seconds : 0,
    )
    window.playerBridge.setAllLapsMode(enabled, dataRequirements.historyMask, typeof chartWindow === 'number' ? seconds : 0)
    setTelemetrySeconds(enabled ? Infinity : seconds, typeof chartWindow === 'number')
  }, [chartWindow, dataRequirements, seconds])

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
        onClosePlayback={playback.close}
        onSelectPlaybackFile={playback.selectFile}
        chartWindow={chartWindow}
        clAvailable={clAvailable}
        referenceLapNum={referenceLapNum}
        referenceLapOptions={referenceLapOptions}
        setEditOpen={setEditOpen}
        setHeaderVisible={setHeaderVisible}
        setChartWindow={setChartWindow}
        setReferenceLapNum={setReferenceLapNum}
        setSettingsOpen={setSettingsOpen}
        setTab={setTab}
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
        miscLayout={miscLayout}
        powerLayout={powerLayout}
        setCoreLayout={setCoreLayout}
        setEditOpen={setEditOpen}
        setInputLayout={setInputLayout}
        setMiscLayout={setMiscLayout}
        setPowerLayout={setPowerLayout}
        setTyresLayout={setTyresLayout}
        tab={tab}
        tyreView={tyreView}
        tyreWearMode={tyreWearMode}
        tyresLayout={tyresLayout}
      />

      {/* Settings Modal */}
      {settingsOpen && (
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
          graphView={graphView}
          onGraphViewChange={setGraphView}
          compact={compact}
          onCompactChange={setCompact}
          chartYAxis={chartYAxis}
          onChartYAxisChange={setChartYAxis}
        />
      )}

      {/* Content */}
      <RaceLeaderWatcher enabled={!playback.state?.filename} onLeaderChange={handleLeaderChange} />
      <main className="flex-1 min-h-0">
        <ChartCoordinatesProvider mode={chartCoordinateMode} referenceLapNum={referenceLapNum} rowTypeMask={dataRequirements.historyMask}>
        <TabContent key={chartCoordinateMode ?? 'time-window'}
          tab={tab}
          isDark={theme === 'dark'}
          seconds={seconds}
          coreLayout={coreLayout}
          powerLayout={powerLayout}
          tyresLayout={tyresLayout}
          inputLayout={inputLayout}
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

    </div>
  )
}
