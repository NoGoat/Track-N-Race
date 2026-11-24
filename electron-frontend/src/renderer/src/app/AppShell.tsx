import { startTransition, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'
import { flushSync } from 'react-dom'
import { setAnalyzeLapEnabled, setHistoryRowMask, setTelemetrySeconds, useTelemetryStore } from '../stores/telemetryStore'
import Settings, { type SettingsCategory } from '../components/Settings'
import type { AnalysisDriverSelection, AnalyzeFixedLapMode, SecondaryFileData } from '../components/AnalyzeScreen'
import { getChartWindowOptionGroups, TAB_OPTIONS, type ChartWindow, type Tab } from './appConfig'
import { useAppConfiguration } from './hooks/useAppConfiguration'
import { useWindowState } from './hooks/useWindowState'
import { usePlayback } from './hooks/usePlayback'
import { useSecondaryFile } from './hooks/useSecondaryFile'
import type { SecondaryLapState } from './hooks/useSecondaryLap'
import { useSecondaryState, useSecondaryStreamControl } from '../lib/secondaryStream'
import { useLapSyncedPlayback } from './hooks/useLapSyncedPlayback'
import type { MergedTimeline } from '../lib/mergedTimeline'
import { SecondaryLapProvider } from './components/FileSplit'
import { useRaceBanners } from './hooks/useRaceBanners'
import AppHeader from './components/AppHeader'
import AppHeaderMacOS from './components/AppHeaderMacOS'
import PageMountShell from './components/PageMountShell'
import PlaybackBar from './components/PlaybackBar'
import TabContent from './components/TabContent'
import FullscreenBanner from './components/FullscreenBanner'
import StatusOverlays from './components/StatusOverlays'
import SeekLoadingOverlay from './components/SeekLoadingOverlay'
import LayoutEditor from './components/LayoutEditor'
import PlaybackDialogs from './components/PlaybackDialogs'
import SecondaryFileMismatchModal from './components/SecondaryFileMismatchModal'
import { isSecondaryReferenceLap, SecondaryComparisonProvider, secondaryLapNumber, secondaryReferenceLap, type ReferenceLapOption } from '../lib/secondaryComparison'
import { fileDateLabels, recordingDateLabel, recordingSessionLabel, secondaryFileForDriver, secondaryLapV6TypeMask } from '../lib/secondaryFile'
import { PrimaryHeaderProvider, SecondaryLiveProvider, secondaryRowIndices, secondaryRowsAt, type PrimaryHeader, type SecondaryLiveRows } from '../lib/secondaryLive'
import RaceLeaderWatcher from './components/RaceLeaderWatcher'
import RecordingErrorDialog from './components/RecordingErrorDialog'
import RecordingChoiceDialog from './components/RecordingChoiceDialog'
import UpdateAvailableDialog from './components/UpdateAvailableDialog'
import FirstTimeSetup from '../components/FirstTimeSetup'
import type { AvailableUpdate, FirstTimeSetupState, RecordingErrorMsg } from '../types'
import { ChartCoordinatesProvider } from '../lib/chartCoordinates'
import { DATA_ROW, dataRequirementsForUi, visibleChartSectionsForUi } from '../lib/historyDependencies'
import {
  ChartWindowOverridesProvider,
  GRAPH_SECTION_ROW_MASK,
  type ChartReferenceLapOverrides,
  type ChartWindowOverrides,
} from '../lib/chartWindowOverrides'
import type { GraphSection } from '../lib/graphSections'
import { buildDriverOptions } from '../lib/driverOptions'

const LIVE_LAP_FAMILY_MASK = DATA_ROW.telemetry | DATA_ROW.status |
  DATA_ROW.damage | DATA_ROW.lap | DATA_ROW.motion | DATA_ROW.motionEx

type AppPageTransition = {
  ready: Promise<unknown>
  finished: Promise<unknown>
  skipTransition?: () => void
}

/** The lap of the lap-synced timeline at segment `index`: that lap, or the last lap before it (after the race). */
function syncedLapAt(timeline: MergedTimeline, index: number): number | null {
  for (let i = Math.min(index, timeline.segments.length - 1); i >= 0; i--)
    if (timeline.segments[i].kind === 'lap') return timeline.segments[i].lap_num
  return null
}

export default function AppShell() {
  const Header = window.platform === 'darwin' ? AppHeaderMacOS : AppHeader
  const {
    actualNativeTitlebar, bannerDuration, chartWindow, chartYAxis, compact, coreLayout, damageLayout, driversMode, fileSplitEnabled,
    fpsInFocus, fpsOutOfFocus, graphView, inputCursorSyncEnabled, inputLayout, mapDimmed, mapTimeout, miscLayout, pageLayouts,
    nativeTitlebar, powerLayout, reduceAnimations, secondaryHorizontalCrosshairEnabled, secondaryVerticalCrosshairEnabled, seconds, sectorBoundariesEnabled, sectorColors, titlebarUpdateInterval,
    setBannerDuration, setChartWindow, setChartYAxis, setCompact, setCoreLayout, setDamageLayout, setDriversMode, setFileSplitEnabled,
    setFpsInFocus, setFpsOutOfFocus, setGraphView, setInputCursorSyncEnabled, setInputLayout, setMapDimmed,
    setMapTimeout, setMiscLayout, setNativeTitlebar, setPageLayouts, setPowerLayout, setReduceAnimations,
    setSecondaryHorizontalCrosshairEnabled, setSecondaryVerticalCrosshairEnabled, setSectorBoundariesEnabled, setSectorColors, setSessionLayout, setStandingsLayout, setTheme, setTitlebarUpdateInterval, setTrendsLayout, setTyreView, setTyreWearMode, setTyresLayout,
    sessionLayout, standingsLayout, theme, trendsLayout, tyreView, tyreWearMode, tyresLayout,
  } = useAppConfiguration()
  const [tab, setTab] = useState<Tab>('core')
  const [mountedTab, setMountedTab] = useState<Tab | null>('core')
  const [settingsOpen, setSettingsOpen] = useState(false)
  const [settingsCategory, setSettingsCategory] = useState<SettingsCategory | null>(null)
  const [selectedIdx, setSelectedIdx] = useState<number | null>(null)
  const [playbackDriverIdx, setPlaybackDriverIdx] = useState<number | null>(null)
  const [recordedPlayerIdx, setRecordedPlayerIdx] = useState<number | null>(null)
  const [editOpen, setEditOpen] = useState(false)
  const [recordingError, setRecordingError] = useState<RecordingErrorMsg | null>(null)
  const [availableUpdate, setAvailableUpdate] = useState<AvailableUpdate | null>(null)
  const [udpListenerError, setUdpListenerError] = useState<string | null>(null)
  // First-time setup: shown on a fresh install, or when launched with --first-time-setup.
  const [setupState, setSetupState] = useState<FirstTimeSetupState | null>(() => {
    try { return window.setupBridge.getState() } catch { return null }
  })
  const [setupSession, setSetupSession] = useState(0)
  useEffect(() => window.setupBridge?.onOpen(state => {
    setSetupState(state)
    setSetupSession(session => session + 1)
  }), [])
  const handleSetupDone = useCallback(() => setSetupState(null), [])
  const { headerVisible, isFullscreen, isMaximized, setHeaderVisible } = useWindowState()
  const [analyzeCompareLapNum, setAnalyzeCompareLapNum] = useState<number | null>(null)
  const [analyzeCompareDriver, setAnalyzeCompareDriver] = useState<AnalysisDriverSelection | null>(null)
  const [analyzeSecondaryFile, setAnalyzeSecondaryFile] = useState<SecondaryFileData | null>(null)
  // File B: loaded from the title bar, read by Analysis and the other pages.
  const secondary = useSecondaryFile(analyzeSecondaryFile, setAnalyzeSecondaryFile)
  // Side-by-side A | B charts (title-bar toggle, off by default, saved in the
  // settings). Off, B's laps are offered in the chart lap selectors instead.
  // It applies only while a second file is open; closing B keeps the choice.
  const fileSplitActive = fileSplitEnabled && analyzeSecondaryFile !== null
  // The car followed in file B (title-bar driver picker); -1 is B's recorded
  // player. Each new second file starts on its player.
  const [secondaryDriverIdx, setSecondaryDriverIdx] = useState(-1)
  const [secondaryDriverFile, setSecondaryDriverFile] = useState<string | null>(null)
  const secondaryFilePath = analyzeSecondaryFile?.filePath ?? null
  if (secondaryDriverFile !== secondaryFilePath) {
    setSecondaryDriverFile(secondaryFilePath)
    setSecondaryDriverIdx(-1)
  }
  // File B as the other pages see it: through the followed car's laps.
  // Analysis keeps the file as loaded; it picks B's drivers itself.
  const followedSecondaryFile = useMemo(
    () => analyzeSecondaryFile ? secondaryFileForDriver(analyzeSecondaryFile, secondaryDriverIdx) : null,
    [analyzeSecondaryFile, secondaryDriverIdx],
  )
  const [referenceLapNum, setReferenceLapNum] = useState<number | null>(1)
  const [analyzeFixedLapMode, setAnalyzeFixedLapMode] = useState<AnalyzeFixedLapMode>({
    enabled: false, lapA: null, lapB: null, lapADriver: null, lapBDriver: null,
  })
  const [analyzeDataMask, setAnalyzeDataMask] = useState(0)
  const [chartWindowOverrides, setChartWindowOverrides] = useState<ChartWindowOverrides>({})
  const [chartReferenceLapOverrides, setChartReferenceLapOverrides] = useState<ChartReferenceLapOverrides>({})
  // File B's half of each split chart: its own window and selected lap.
  const [secondaryWindowOverrides, setSecondaryWindowOverrides] = useState<ChartWindowOverrides>({})
  const [secondaryReferenceLapOverrides, setSecondaryReferenceLapOverrides] = useState<ChartReferenceLapOverrides>({})
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
    setSecondaryWindowOverrides({})
    setSecondaryReferenceLapOverrides({})
    setChartWindow(value)
  }, [setChartWindow])
  const setSecondaryWindowOverride = useCallback((section: GraphSection, value: ChartWindow | null) => {
    setSecondaryWindowOverrides(current => {
      if (value === null) {
        if (!(section in current)) return current
        const next = { ...current }
        delete next[section]
        return next
      }
      return current[section] === value ? current : { ...current, [section]: value }
    })
  }, [])
  const setSecondaryReferenceLapOverride = useCallback((section: GraphSection, lapNum: number | null) => {
    setSecondaryReferenceLapOverrides(current => {
      if (lapNum === null) {
        if (!(section in current)) return current
        const next = { ...current }
        delete next[section]
        return next
      }
      return current[section] === lapNum ? current : { ...current, [section]: lapNum }
    })
  }, [])
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

  // Opening or closing a recording (and the first render) starts with fresh
  // comparison and driver selections.
  const [selectionsFile, setSelectionsFile] = useState<{ filename: string | null | undefined } | null>(null)
  if (selectionsFile === null || selectionsFile.filename !== playback.state?.filename) {
    setSelectionsFile({ filename: playback.state?.filename })
    setAnalyzeCompareLapNum(null)
    setAnalyzeCompareDriver(null)
    setAnalyzeSecondaryFile(null)
    setAnalyzeFixedLapMode({
      enabled: false, lapA: null, lapB: null, lapADriver: null, lapBDriver: null,
    })
    setReferenceLapNum(null)
    setPlaybackDriverIdx(null)
    setRecordedPlayerIdx(null)
  }

  const handleCloseSettings = useCallback(() => {
    setSettingsOpen(false)
    setSettingsCategory(null)
  }, [])

  const handleOpenUdpSettings = useCallback(() => {
    setSettingsCategory('network')
    setSettingsOpen(true)
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
  // A driver-only recording (or an older player-only one) holds one driver's
  // data, while its participants roster still names every car.
  const singleDriverRecording = useTelemetryStore(s => s.playbackAnalysisDrivers.length === 1)
  const driverSelectorDisabled = driverSelectorVisible && singleDriverRecording
  const originalPlayerIdx = recordedPlayerIdx ?? playbackDriverIndex ?? timingPlayerIdx
  // Every participants packet replaces the store's roster object, so this list
  // would otherwise get a new identity several times a second and re-render the
  // whole title bar through AppHeader's memo. Reuse the previous array whenever
  // the rendered options are unchanged.
  const nextDriverOptions = useMemo(() => buildDriverOptions(participants?.drivers ?? [], {
    playerIdx: originalPlayerIdx,
    markRestricted: true,
    disableOthers: driverSelectorDisabled,
  }), [participants, originalPlayerIdx, driverSelectorDisabled])
  const [driverOptions, setDriverOptions] = useState(nextDriverOptions)
  if (nextDriverOptions !== driverOptions && !(driverOptions.length === nextDriverOptions.length &&
      driverOptions.every((option, index) => {
        const next = nextDriverOptions[index]
        return (Object.keys(option) as (keyof typeof option)[]).every(key => option[key] === next[key])
      }))) {
    setDriverOptions(nextDriverOptions)
  }
  if (!driverSelectorVisible && playbackDriverIdx !== null) setPlaybackDriverIdx(null)
  const initialPlayerIdx = playbackDriverIndex ?? timingPlayerIdx
  if (driverSelectorVisible && recordedPlayerIdx === null && initialPlayerIdx !== null) {
    setRecordedPlayerIdx(initialPlayerIdx)
    setPlaybackDriverIdx(initialPlayerIdx)
  }
  // recordedPlayerIdx is only ever set right above, once per recording: tell
  // the engine which driver the recording starts on.
  useEffect(() => {
    if (recordedPlayerIdx !== null) window.playerBridge.setDriver(recordedPlayerIdx, true)
  }, [recordedPlayerIdx])
  const availableChartWindows = useMemo(() => new Set(
    getChartWindowOptionGroups(clAvailable, recordingOpen)
      .flatMap(group => group.options)
      .map(option => option.value),
  ), [clAvailable, recordingOpen])
  // Drop overrides for windows the current source can't show.
  const prunedWindowOverrides = Object.fromEntries(Object.entries(chartWindowOverrides)
    .filter(([, value]) => availableChartWindows.has(value))) as ChartWindowOverrides
  if (Object.keys(prunedWindowOverrides).length !== Object.keys(chartWindowOverrides).length)
    setChartWindowOverrides(prunedWindowOverrides)
  const prunedReferenceLapOverrides = Object.fromEntries(Object.entries(chartReferenceLapOverrides)
    .filter(([section]) => {
      const graphSection = section as GraphSection
      const effectiveWindow = chartWindowOverrides[graphSection] ?? chartWindow
      return effectiveWindow === 'RL' && availableChartWindows.has(effectiveWindow)
    })) as ChartReferenceLapOverrides
  if (Object.keys(prunedReferenceLapOverrides).length !== Object.keys(chartReferenceLapOverrides).length)
    setChartReferenceLapOverrides(prunedReferenceLapOverrides)
  const chartCoordinateMode = chartWindow === 'AL' || chartWindow === 'SL'
    ? chartWindow
    : clAvailable && typeof chartWindow !== 'number' && (chartWindow !== 'RL' || recordingOpen)
      ? chartWindow
      : null
  const playbackFilename = playback.state?.filename ?? null
  const referenceLapOptions = useMemo((): ReferenceLapOption[] => {
    const lapNumbers = (playback.speedRpmBlocks ?? [])
      .map(block => Number(block.lapNum))
      .filter(Number.isFinite)
    const own = [...new Set(lapNumbers)].sort((a, b) => a - b)
    if (!followedSecondaryFile || own.length === 0) return own.map(value => ({ value, label: String(value) }))
    // File B open: its laps follow A's, grouped under each file's date.
    const [dateA, dateB] = fileDateLabels(playbackFilename, followedSecondaryFile.filename)
    const secondaryLaps = new Set<number>()
    for (const block of followedSecondaryFile.blocks) secondaryLaps.add(Number(block.lapNum))
    for (const key of Object.keys(followedSecondaryFile.lapTimesByNum)) secondaryLaps.add(Number(key))
    return [
      ...own.map(value => ({ value, label: String(value), group: dateA ?? 'File A' })),
      ...[...secondaryLaps].filter(lap => Number.isFinite(lap) && lap > 0).sort((a, b) => a - b)
        .map(lap => ({ value: secondaryReferenceLap(lap), label: String(lap), group: dateB ?? 'File B' })),
    ]
  }, [followedSecondaryFile, playback.speedRpmBlocks, playbackFilename])

  // File B's half: "Selected" picks from B's own laps. It starts on the title
  // bar's selected lap (one of B's laps picked there, or the same lap number),
  // else B's first lap.
  const secondaryReferenceLapOptions = useMemo((): ReferenceLapOption[] => {
    const laps = new Set<number>()
    for (const block of followedSecondaryFile?.blocks ?? []) if (block.lapNum >= 1) laps.add(Number(block.lapNum))
    return [...laps].sort((a, b) => a - b).map(lap => ({ value: lap, label: String(lap) }))
  }, [followedSecondaryFile])
  const secondaryReferenceLapNum = referenceLapNum === null ? secondaryReferenceLapOptions[0]?.value ?? null
    : isSecondaryReferenceLap(referenceLapNum) ? secondaryLapNumber(referenceLapNum)
      : secondaryReferenceLapOptions.some(option => option.value === referenceLapNum) ? referenceLapNum
        : secondaryReferenceLapOptions[0]?.value ?? null

  // A per-chart pick of one of B's laps goes once B closes.
  if (!analyzeSecondaryFile && Object.values(chartReferenceLapOverrides).some(isSecondaryReferenceLap)) {
    setChartReferenceLapOverrides(Object.fromEntries(Object.entries(chartReferenceLapOverrides)
      .filter(([, lap]) => !isSecondaryReferenceLap(lap))) as ChartReferenceLapOverrides)
  }

  // The lap catalog arrives after the playback header. Keep RL on its Lap 1
  // default while loading instead of clearing it to the dash placeholder.
  if (referenceLapOptions.length > 0 &&
      (referenceLapNum === null || !referenceLapOptions.some(option => option.value === referenceLapNum))) {
    setReferenceLapNum(referenceLapOptions.find(option => option.value === 1)?.value ?? referenceLapOptions[0].value)
  }
  // Publish the visible time window to the store so it computes the right slices.
  const dataRequirements = useMemo(
    () => dataRequirementsForUi(
      tab, coreLayout, inputLayout, miscLayout, powerLayout, tyresLayout, tyreView,
      Boolean(playback.state?.filename),
      analyzeDataMask,
      pageLayouts,
      graphView,
    ),
    [tab, coreLayout, inputLayout, miscLayout, powerLayout, tyresLayout, tyreView, playback.state?.filename, analyzeDataMask, pageLayouts, graphView],
  )
  // File B's laps are read with the V6 fields file A is subscribed to on this
  // page (chart history and card stream fields), not every field of each row type.
  const secondaryV6TypeMask = useMemo(
    () => secondaryLapV6TypeMask(dataRequirements.v6Types, dataRequirements.v6HistoryTypes),
    [dataRequirements],
  )
  const visibleChartSections = useMemo(() => visibleChartSectionsForUi(
    tab, coreLayout, inputLayout, pageLayouts, miscLayout, powerLayout, tyresLayout, tyreView,
  ), [tab, coreLayout, inputLayout, pageLayouts, miscLayout, powerLayout, tyresLayout, tyreView])
  const visibleChartScopes = useMemo((): Array<{ mask: number; window: ChartWindow }> => tab === 'stint'
    // Trends' per-lap measurements and graphs cover the whole session,
    // whatever the title-bar window. Each graph's range choice only crops it.
    ? [{ mask: DATA_ROW.status | DATA_ROW.damage, window: 'AL' }]
    // File A's half keeps its own window with file B open; B's half reads B's
    // data itself (ChartWindowScope).
    : visibleChartSections.map(section => ({ mask: GRAPH_SECTION_ROW_MASK[section], window: chartWindowOverrides[section] ?? chartWindow })),
  [chartWindow, chartWindowOverrides, tab, visibleChartSections])
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
    // Aero (7), TyreState (13) and BrakeBias (20) are edge-encoded in V6: the
    // writer only records a sample when the value changes. Every other type is
    // sampled continuously, so if a seek flush omits it the resumed 60Hz stream
    // refills it within a frame and nobody notices. These three have no such
    // safety net — a value whose last change was laps ago is never re-sent, so
    // leaving them out of the history request means the field stays missing
    // until the car next changes it. That is why the wing card read blank after
    // every seek. Request them whenever any history is requested.
    const v6HistoryTypes = [...new Set([
      ...dataRequirements.v6HistoryTypes,
      ...(historyMask ? [7, 13, 20] : []),
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

  // V6 playback reads a car's private status (ERS, fuel, DRS, brake bias) only
  // for the car selected in Standings; the rest of the grid streams lap timing
  // and tyres. Re-sent per recording, since a load starts with no focus car.
  const focusDriverIdx = recordingOpen && tab === 'timing_tower' ? selectedIdx : null
  const recordingFilename = playback.state?.filename
  useEffect(() => {
    if (!recordingFilename) return
    window.playerBridge.setFocusDriver(focusDriverIdx ?? -1)
  }, [recordingFilename, focusDriverIdx])

  // usePlayback returns a fresh object on every render, so reading it through a
  // ref keeps this handler stable for AppHeader's memo.
  const playbackRef = useRef(playback)
  useLayoutEffect(() => { playbackRef.current = playback })

  // A new or closed file A drops file B (the state resets above); keep the
  // engine's copy in step. Nothing to close before a first file A.
  const previousRecordingRef = useRef(recordingFilename)
  useEffect(() => {
    const previous = previousRecordingRef.current
    previousRecordingRef.current = recordingFilename
    if (previous && previous !== recordingFilename) window.analysisBridge.closeFile()
  }, [recordingFilename])

  // Closing file A while file B is open makes B the only recording. A is closed
  // the ordinary way first and B is opened only once the closed state arrives:
  // loading over an open clip lets A's late playback_close clear B's file in main.
  const secondaryRef = useRef(secondary)
  useLayoutEffect(() => { secondaryRef.current = secondary })
  const [promotePath, setPromotePath] = useState<string | null>(null)
  // Two files open: one lap-synced timeline drives playback (Laps 4); the
  // engine plays it and places file B from the same clock.
  const lapSync = useLapSyncedPlayback(followedSecondaryFile, playback.speedRpmBlocks, playback.state, playback.sessionFileStart)
  // The lap both files are on: the lap-synced timeline's. A merged lap lasts as
  // long as the slower file's, so while the quicker file waits at its line both
  // halves, the cards and B's data stay on that lap; after the race (A's
  // cool-down) it stays on the last lap. Without a timeline, A's own lap.
  const syncedLapNum = lapSync ? syncedLapAt(lapSync.timeline, lapSync.position.segmentIndex) ?? playback.currentLapNum : playback.currentLapNum
  // File B's rows stream while B is open (lib/secondaryStream.ts), for the
  // car followed in it, as file A's do in its playback.
  useSecondaryStreamControl(followedSecondaryFile !== null, followedSecondaryFile?.driverIndex ?? -1)
  const secondaryStream = useSecondaryState()
  // File B's copy of the lap both files are on, for the side-by-side chart
  // pages and the Tyres rack (shown whether or not side-by-side is on): B's
  // lap so far, from its stream. A lap B does not have shows B as missing ('—'),
  // still in the two-file layout.
  const secondaryShown = fileSplitActive || (analyzeSecondaryFile !== null && tab === 'tyres')
  const secondaryStreamLap = secondaryStream.lap
  const secondaryLap = useMemo<SecondaryLapState | null>(() => {
    if (!followedSecondaryFile || !secondaryShown) return null
    const missing = syncedLapNum !== null && !followedSecondaryFile.blocks.some(block => block.lapNum === syncedLapNum)
    const block = secondaryStreamLap ? followedSecondaryFile.blocks.find(candidate => candidate.lapNum === secondaryStreamLap.lapNum) : undefined
    return {
      primaryFilename: recordingFilename ?? null,
      file: followedSecondaryFile,
      lapNum: syncedLapNum,
      // The whole lap's span, for the axes; its rows run to B's playhead.
      lapData: missing || !secondaryStreamLap ? null
        : block ? { ...secondaryStreamLap, endSessionTime: block.endSessionTime } : secondaryStreamLap,
      missing,
    }
  }, [followedSecondaryFile, secondaryShown, syncedLapNum, secondaryStreamLap, recordingFilename])
  const secondaryLapData = secondaryLap?.lapData ?? null
  // B's live values: its newest rows, at its moment on the playhead.
  const secondaryIndices = secondaryLapData ? secondaryRowIndices(secondaryLapData) : null
  const secondaryIndexKey = secondaryIndices ? `${secondaryIndices.telemetry}:${secondaryIndices.status}:${secondaryIndices.lap}:${secondaryIndices.damage}` : ''
  const secondaryLive = useMemo<SecondaryLiveRows | null>(() => {
    // Present whenever B is shown (secondaryLap), even with no rows to read yet.
    if (!secondaryLap || !followedSecondaryFile) return null
    const [dateA, dateB] = fileDateLabels(recordingFilename ?? null, followedSecondaryFile.filename)
    const lastLap = (blocks: readonly { lapNum: number }[] | null | undefined) => (blocks ?? []).reduce((max, block) => Math.max(max, Number(block.lapNum) || 0), 0)
    const detail = (filename: string | null | undefined, laps: number) =>
      [(recordingSessionLabel(filename) ?? 'Session').toLowerCase(), ...laps > 0 ? [`${laps} laps`] : []].join(' · ')
    const followedA = playbackDriverIdx ?? recordedPlayerIdx
    return {
      split: fileSplitActive,
      dateA: dateA ?? 'File A', dateB: dateB ?? 'File B',
      headerA: { driver: driverOptions.find(option => option.value === followedA)?.label ?? null, detail: detail(recordingFilename, lastLap(playback.speedRpmBlocks)) },
      headerB: {
        driver: followedSecondaryFile.analysisDrivers.find(driver => followedSecondaryFile.driverIndex >= 0
          ? driver.driverIndex === followedSecondaryFile.driverIndex : driver.isPlayer)?.driverName ?? null,
        detail: detail(followedSecondaryFile.filename, lastLap(followedSecondaryFile.blocks)),
      },
      ...secondaryLapData && secondaryIndices
        ? secondaryRowsAt(secondaryLapData, secondaryIndices)
        : { latest: null, status: null, lap: null, damage: null },
    }
    // Rebuilt only when B moves to another row, not on every frame.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [secondaryLap !== null, secondaryLapData, secondaryIndexKey, followedSecondaryFile, recordingFilename, driverOptions, playbackDriverIdx, recordedPlayerIdx, playback.speedRpmBlocks, fileSplitActive])
  // File A's header cell on its own (one file, or live), for the Tyres rack.
  const primaryFollowed = playbackDriverIdx ?? recordedPlayerIdx
  const primaryDriverName = driverOptions.find(option => option.value === primaryFollowed)?.label ?? null
  const primaryLastLap = (playback.speedRpmBlocks ?? []).reduce((max, block) => Math.max(max, Number(block.lapNum) || 0), 0)
  const primaryHeader = useMemo<PrimaryHeader>(() => ({
    date: recordingFilename ? recordingDateLabel(recordingFilename) ?? 'Recording' : 'Live',
    header: {
      driver: primaryDriverName,
      detail: [(recordingFilename ? recordingSessionLabel(recordingFilename) ?? 'Session' : 'Live session').toLowerCase(),
        ...primaryLastLap > 0 ? [`${primaryLastLap} laps`] : []].join(' · '),
    },
  }), [recordingFilename, primaryDriverName, primaryLastLap])
  const handleClosePlayback = useCallback(() => {
    const promoted = secondaryRef.current.file?.filePath ?? null
    if (promoted) {
      secondaryRef.current.close()
      setPromotePath(promoted)
    }
    playbackRef.current.close()
  }, [])
  useEffect(() => {
    if (!promotePath || recordingFilename) return
    setPromotePath(null)
    playbackRef.current.loadFile(promotePath)
  }, [promotePath, recordingFilename])
  const handlePlaybackDriverChange = useCallback((idx: number) => {
    if (!driverSelectorVisible || idx === playbackDriverIdx) return
    const option = driverOptions.find(candidate => candidate.value === idx)
    if (!option || option.isDisabled) return
    setPlaybackDriverIdx(idx)
    setSelectedIdx(idx)
    window.playerBridge.setDriver(idx, idx === originalPlayerIdx)
    const current = playbackRef.current
    if (current.state) current.seekProgress(current.state.progressPct)
  }, [driverOptions, driverSelectorVisible, originalPlayerIdx, playbackDriverIdx])

  return (
    <div className="h-dvh bg-[var(--bg-base)] text-[var(--text-primary)] flex flex-col relative">
      {window.platform !== 'darwin' && (
        <FullscreenBanner banner={activeBanner} headerVisible={headerVisible} isFullscreen={isFullscreen} />
      )}

      <Header
        actualNativeTitlebar={actualNativeTitlebar}
        activeBanner={activeBanner}
        editOpen={editOpen}
        filename={playback.state?.filename ?? undefined}
        headerVisible={headerVisible}
        isFullscreen={isFullscreen}
        isMaximized={isMaximized}
        inputCursorSyncEnabled={inputCursorSyncEnabled}
        sectorBoundariesEnabled={sectorBoundariesEnabled}
        fileSplitEnabled={fileSplitEnabled}
        setFileSplitEnabled={setFileSplitEnabled}
        onClosePlayback={handleClosePlayback}
        secondaryFile={secondary.file}
        secondaryLoading={secondary.loading}
        secondaryError={secondary.error}
        onSelectSecondaryFile={secondary.select}
        onCloseSecondaryFile={secondary.close}
        onSelectPlaybackFile={playback.selectFile}
        chartWindow={chartWindow}
        driverOptions={driverOptions}
        driverSelectorVisible={driverSelectorVisible}
        driverSelectorDisabled={driverSelectorDisabled}
        selectedDriverIdx={playbackDriverIdx}
        secondaryDriverIdx={secondaryDriverIdx}
        setSecondaryDriverIdx={setSecondaryDriverIdx}
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
        onOpenUdpSettings={handleOpenUdpSettings}
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
        secondaryLoading={secondary.loading}
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
        setTrendsLayout={setTrendsLayout}
        damageLayout={damageLayout}
        setDamageLayout={setDamageLayout}
        tab={tab}
        trendsLayout={trendsLayout}
        tyreView={tyreView}
        tyreWearMode={tyreWearMode}
        tyresLayout={tyresLayout}
      />

      {/* Settings Modal */}
      <Settings
        isOpen={settingsOpen}
        openCategory={settingsCategory}
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
      <main className="app-page-transition relative flex-1 min-h-0">
        <SecondaryComparisonProvider file={followedSecondaryFile} primaryFilename={recordingFilename ?? null} background={!fileSplitActive} v6TypeMask={secondaryV6TypeMask}>
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
          secondaryOverrides={secondaryWindowOverrides}
          setSecondaryOverride={setSecondaryWindowOverride}
          secondaryReferenceLapNum={secondaryReferenceLapNum}
          secondaryReferenceLapOptions={secondaryReferenceLapOptions}
          secondaryReferenceLapOverrides={secondaryReferenceLapOverrides}
          setSecondaryReferenceLapOverride={setSecondaryReferenceLapOverride}
        >
        <ChartCoordinatesProvider mode={chartCoordinateMode} referenceLapNum={referenceLapNum} rowTypeMask={dataRequirements.historyMask} sectorBoundaries={sectorBoundariesEnabled}>
        <SecondaryLapProvider value={fileSplitActive ? secondaryLap : null}>
        <SecondaryLiveProvider value={secondaryLive}>
        <PrimaryHeaderProvider value={primaryHeader}>
        {mountedTab === null ? <PageMountShell /> : <TabContent
          tab={mountedTab}
          isDark={theme !== 'light'}
          seconds={seconds}
          coreLayout={coreLayout}
          powerLayout={powerLayout}
          sessionLayout={sessionLayout}
          standingsLayout={standingsLayout}
          tyresLayout={tyresLayout}
          trendsLayout={trendsLayout}
          damageLayout={damageLayout}
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
          analyzeFixedLapMode={analyzeFixedLapMode}
          onAnalyzeFixedLapModeChange={setAnalyzeFixedLapMode}
          onAnalyzeDataMaskChange={setAnalyzeDataMask}
        />}
        </PrimaryHeaderProvider>
        </SecondaryLiveProvider>
        </SecondaryLapProvider>
        </ChartCoordinatesProvider>
        </ChartWindowOverridesProvider>
        </SecondaryComparisonProvider>
        <SeekLoadingOverlay />
      </main>

      {/* Playback Controls Bar */}
      {playback.state && playback.state.filename && (
        <PlaybackBar
          compact={compact.playbackBar}
          currentLapNum={playback.currentLapNum}
          exportError={playback.exportError}
          exportState={playback.exportState}
          onExport={playback.exportParquet}
          onSeekProgress={playback.seekProgress}
          onSeekBackward={playback.seekBackward}
          onSeekForward={playback.seekForward}
          onSpeedChange={playback.setSpeed}
          onTogglePlay={playback.togglePlay}
          sessionFileStart={playback.sessionFileStart}
          speedRpmBlocks={playback.speedRpmBlocks}
          state={playback.state}
          merged={lapSync}
        />
      )}

      <SecondaryFileMismatchModal
        pending={secondary.pendingMismatch}
        onConfirm={secondary.confirmMismatch}
        onCancel={secondary.cancelMismatch}
      />

      <PlaybackDialogs
        confirmOpenFilePath={playback.confirmOpenFilePath}
        loadError={playback.loadError}
        setConfirmOpenFilePath={playback.setConfirmOpenFilePath}
        setLoadError={playback.setLoadError}
      />

      <RecordingChoiceDialog />

      <RecordingErrorDialog
        error={recordingError}
        onClose={() => setRecordingError(null)}
      />

      <UpdateAvailableDialog
        update={availableUpdate}
        onClose={() => setAvailableUpdate(null)}
      />

      {setupState?.show && (
        <FirstTimeSetup
          key={setupSession}
          state={setupState}
          theme={theme}
          onThemeChange={setTheme}
          compact={compact}
          onCompactChange={setCompact}
          onDone={handleSetupDone}
        />
      )}

    </div>
  )
}
