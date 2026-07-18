import React, { useCallback, useEffect, useMemo, useState } from 'react'
import { setTelemetrySeconds, useTelemetryStore } from '../stores/telemetryStore'
import Settings from '../components/Settings'
import type { AnalyzeFixedLapMode } from '../components/AnalyzeScreen'
import type { Tab } from './appConfig'
import { useAppPerformanceDiagnostics } from './hooks/useAppPerformanceDiagnostics'
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

export default function AppShell() {
  const Header = window.platform === 'darwin' ? AppHeaderMacOS : AppHeader
  const {
    actualNativeTitlebar, bannerDuration, chartYAxis, compact, coreLayout, driversMode,
    fpsInFocus, fpsOutOfFocus, graphView, inputLayout, mapDimmed, mapTimeout, miscLayout,
    nativeTitlebar, powerLayout, reduceAnimations, seconds, sectorColors,
    setBannerDuration, setChartYAxis, setCompact, setCoreLayout, setDriversMode,
    setFpsInFocus, setFpsOutOfFocus, setGraphView, setInputLayout, setMapDimmed,
    setMapTimeout, setMiscLayout, setNativeTitlebar, setPowerLayout, setReduceAnimations,
    setSeconds, setSectorColors, setTheme, setTyreView, setTyreWearMode, setTyresLayout,
    theme, tyreView, tyreWearMode, tyresLayout,
  } = useAppConfiguration()
  const [tab, setTab] = useState<Tab>('core')
  const { onAppRender, measureAppBody } = useAppPerformanceDiagnostics(tab)
  const [settingsOpen, setSettingsOpen] = useState(false)
  const [selectedIdx, setSelectedIdx] = useState<number | null>(null)
  const [speedRpmMode, setSpeedRpmMode] = useState<'default' | 'CL' | 'PL' | 'FL' | 'compare'>('default')
  const [editOpen, setEditOpen] = useState(false)
  const { headerVisible, isFullscreen, isMaximized, setHeaderVisible } = useWindowState()
  const [analyzeCompareLapNum, setAnalyzeCompareLapNum] = useState<number | null>(null)
  const [analyzeFixedLapMode, setAnalyzeFixedLapMode] = useState<AnalyzeFixedLapMode>({ enabled: false, lapA: null, lapB: null })
  const handlePlaybackClosed = useCallback(() => setSelectedIdx(null), [])
  const playback = usePlayback(handlePlaybackClosed)

  useEffect(() => {
    setAnalyzeCompareLapNum(null)
    setAnalyzeFixedLapMode({ enabled: false, lapA: null, lapB: null })
  }, [playback.state?.filename])

  const handleCloseSettings = useCallback(() => {
    setSettingsOpen(false)
  }, [])

  // App is deliberately COLD: it selects only low-frequency slices. Every hot,
  // per-frame slice is read inside <TabContent/> and the other subscriber
  // components below, so a telemetry frame never re-renders App itself.
  const protocolStatus  = useTelemetryStore(s => s.protocolStatus)
  const protocolWarning = useTelemetryStore(s => s.protocolWarning)
  // Publish the visible time window to the store so it computes the right slices.
  useEffect(() => { setTelemetrySeconds(seconds) }, [seconds])

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

  // Diagnostic: build the tree into a const first so we can measure App's own
  // function-body cost (hooks + createElement for the whole JSX) BEFORE React
  // reconciles/commits. `sync:app-body` vs `sync:react-render+commit` splits
  // "App rebuilding its giant tree every render" from "the DOM commit". App is
  // the parent of React.Profiler, so this body cost is invisible to that.
  const appTree = (
    <React.Profiler id="app" onRender={onAppRender}>
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
        seconds={seconds}
        setEditOpen={setEditOpen}
        setHeaderVisible={setHeaderVisible}
        setSeconds={setSeconds}
        setSettingsOpen={setSettingsOpen}
        setTab={setTab}
        settingsOpen={settingsOpen}
        tab={tab}
        theme={theme}
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
        <TabContent
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
          speedRpmMode={speedRpmMode}
          onSpeedRpmModeChange={setSpeedRpmMode}
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
        />
      </main>

      {/* Playback Controls Bar */}
      {playback.state && playback.state.filename && (
        <PlaybackBar
          compact={compact.playbackBar}
          currentLapNum={playback.currentLapNum}
          exportError={playback.exportError}
          exportState={playback.exportState}
          onExport={playback.exportXlsx}
          onSeekBackward={playback.seekBackward}
          onSeekForward={playback.seekForward}
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

    </div>
    </React.Profiler>
  )
  measureAppBody()
  return appTree
}
