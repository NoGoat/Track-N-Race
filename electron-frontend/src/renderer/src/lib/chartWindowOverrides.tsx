import { createContext, memo, useCallback, useContext, useMemo, type ReactNode } from 'react'
import { createPortal } from 'react-dom'
import { type GroupBase, type SingleValue } from 'react-select'
import Select from './AnimatedSelect'
import { ChartCoordinatesProvider, SecondaryChartCoordinatesProvider, lapStartSessionTime } from './chartCoordinates'
import { LapDataSideProvider, useLapDataSide, type LapDataSide } from './lapDataSide'
import { ChartSplit, useChartHeaderSlotB, useChartSplit, useInSplitRegion, useSecondaryLapState } from './chartSplit'
import { formatReferenceLapGroup, formatReferenceLapOption, groupReferenceLapOptions, useSecondaryLapData, type ReferenceLapOption } from './secondaryComparison'
import { useSecondaryHistoryNeed, useSecondaryState } from './secondaryStream'
import { buildSelectStyles } from './selectStyles'
import { selectComponents } from './selectComponents'
import { DATA_ROW } from './historyDependencies'
import type { GraphSection } from './graphSections'
import {
  getChartWindowOptionGroups,
  type ChartWindow,
  type ChartWindowOption,
} from '../app/appConfig'
import AnimatedAutoWidth from '../app/components/AnimatedAutoWidth'

export type ChartWindowOverrides = Partial<Record<GraphSection, ChartWindow>>
export type ChartReferenceLapOverrides = Partial<Record<GraphSection, number>>
type LapOption = ReferenceLapOption

interface OverrideContextValue {
  globalWindow: ChartWindow
  overrides: ChartWindowOverrides
  setOverride: (section: GraphSection, value: ChartWindow | null) => void
  clAvailable: boolean
  recordingOpen: boolean
  referenceLapNum: number | null
  referenceLapOptions: LapOption[]
  referenceLapOverrides: ChartReferenceLapOverrides
  setReferenceLapOverride: (section: GraphSection, lapNum: number | null) => void
  sectorBoundariesEnabled: boolean
  /*
   * File B's half of each split chart picks its own window and selected lap
   * (B's own lap numbers); without an override it uses the title-bar window
   * and secondaryReferenceLapNum, as A's half does.
   */
  secondaryOverrides?: ChartWindowOverrides
  setSecondaryOverride?: (section: GraphSection, value: ChartWindow | null) => void
  secondaryReferenceLapNum?: number | null
  secondaryReferenceLapOptions?: LapOption[]
  secondaryReferenceLapOverrides?: ChartReferenceLapOverrides
  setSecondaryReferenceLapOverride?: (section: GraphSection, lapNum: number | null) => void
}

const OverrideContext = createContext<OverrideContextValue | null>(null)

interface ScopeContextValue {
  section: GraphSection
  window: ChartWindow
  windowSeconds: number
  referenceLapNum: number | null
  /** File B's half of a split chart: its selector binds B's own window. */
  secondary?: boolean
}

const ScopeContext = createContext<ScopeContextValue | null>(null)

export const GRAPH_SECTION_ROW_MASK: Record<GraphSection, number> = {
  overviewTelemetry: DATA_ROW.telemetry | DATA_ROW.status,
  overviewTyreSurface: DATA_ROW.telemetry,
  overviewTyreInner: DATA_ROW.telemetry,
  overviewTyreBrake: DATA_ROW.telemetry,
  overviewTyreWear: DATA_ROW.damage,
  overviewTyreCardFL: 0,
  overviewTyreCardFR: 0,
  overviewTyreCardRL: 0,
  overviewTyreCardRR: 0,
  tyreSurface: DATA_ROW.telemetry,
  tyreInner: DATA_ROW.telemetry,
  tyreBrake: DATA_ROW.telemetry,
  tyreWear: DATA_ROW.damage,
  tyreCardFL: 0,
  tyreCardFR: 0,
  tyreCardRL: 0,
  tyreCardRR: 0,
  inputGear: DATA_ROW.telemetry,
  inputThrottleBrake: DATA_ROW.telemetry,
  inputThrottleBrakeOverlay: DATA_ROW.telemetry,
  inputAccelerator: DATA_ROW.telemetry,
  inputBrake: DATA_ROW.telemetry,
  inputSteering: DATA_ROW.telemetry,
  powerSplit: DATA_ROW.status,
  powerHarvest: DATA_ROW.status,
  powerStore: DATA_ROW.status,
  powerFuel: DATA_ROW.status,
  miscGForce: DATA_ROW.motion,
  miscGLateral: DATA_ROW.motion,
  miscGLongitudinal: DATA_ROW.motion,
  miscRideHeight: DATA_ROW.motionEx,
  miscRideFront: DATA_ROW.motionEx,
  miscRideRear: DATA_ROW.motionEx,
}

const selectStyles = buildSelectStyles(true, {
  controlHeight: 22,
  labelStyleGroupHeadings: true,
  menuWidth: '7rem',
  scrollableMenu: true,
})
const lapSelectStyles = buildSelectStyles(true, {
  controlHeight: 22,
  menuWidth: '4rem',
})

export function ChartWindowOverridesProvider({
  globalWindow, overrides, setOverride, clAvailable, recordingOpen, referenceLapNum,
  referenceLapOptions, referenceLapOverrides, setReferenceLapOverride, sectorBoundariesEnabled,
  secondaryOverrides, setSecondaryOverride, secondaryReferenceLapNum, secondaryReferenceLapOptions,
  secondaryReferenceLapOverrides, setSecondaryReferenceLapOverride, children,
}: OverrideContextValue & { children: React.ReactNode }) {
  const value = useMemo(() => ({
    globalWindow, overrides, setOverride, clAvailable, recordingOpen, referenceLapNum,
    referenceLapOptions, referenceLapOverrides, setReferenceLapOverride, sectorBoundariesEnabled,
    secondaryOverrides, setSecondaryOverride, secondaryReferenceLapNum, secondaryReferenceLapOptions,
    secondaryReferenceLapOverrides, setSecondaryReferenceLapOverride,
  }), [clAvailable, globalWindow, overrides, recordingOpen, referenceLapNum, referenceLapOptions,
    referenceLapOverrides, sectorBoundariesEnabled, setOverride, setReferenceLapOverride,
    secondaryOverrides, setSecondaryOverride, secondaryReferenceLapNum, secondaryReferenceLapOptions,
    secondaryReferenceLapOverrides, setSecondaryReferenceLapOverride])
  return <OverrideContext.Provider value={value}>{children}</OverrideContext.Provider>
}

export function ChartWindowScope({ section, children }: { section: GraphSection; children: React.ReactNode }) {
  const inSplitRegion = useInSplitRegion()
  const split = useChartSplit()
  const side = useLapDataSide()
  // On a split page every chart sits in a ChartSplit, open B or not, so the
  // side-by-side toggle never remounts file A's chart.
  if (inSplitRegion && !side) {
    return <ChartSplit secondary={split}><ChartWindowScopeInner section={section}>{children}</ChartWindowScopeInner></ChartSplit>
  }
  return <ChartWindowScopeInner section={section}>{children}</ChartWindowScopeInner>
}

/** The windows on offer, and the one in force: the override when it is on offer, else the title bar's. */
function effectiveWindow(clAvailable: boolean, recordingOpen: boolean, globalWindow: ChartWindow, override: ChartWindow | undefined): ChartWindow {
  const available = getChartWindowOptionGroups(clAvailable, recordingOpen).flatMap(group => group.options).map(option => option.value)
  const titlebarWindow = available.includes(globalWindow) ? globalWindow : 30
  return override !== undefined && available.includes(override) ? override : titlebarWindow
}

function ChartWindowScopeInner({ section, children }: { section: GraphSection; children: React.ReactNode }) {
  const context = useContext(OverrideContext)
  const clAvailable = context?.clAvailable ?? false
  const recordingOpen = context?.recordingOpen ?? false
  const globalWindow = context?.globalWindow ?? 30
  const override = context?.overrides[section]
  const lapOverride = context?.referenceLapOverrides[section]
  const contextReferenceLapNum = context?.referenceLapNum ?? null
  const side = useLapDataSide()
  // The chart bodies around this scope re-render with every telemetry frame.
  // Keep the scope value referentially stable so the memoized window and lap
  // selectors below only re-render when the scope itself actually changes.
  const scope = useMemo<ScopeContextValue>(() => {
    const window = effectiveWindow(clAvailable, recordingOpen, globalWindow, override)
    return {
      section,
      window,
      windowSeconds: typeof window === 'number' ? window : 30,
      referenceLapNum: lapOverride ?? contextReferenceLapNum,
    }
  }, [clAvailable, contextReferenceLapNum, globalWindow, lapOverride, override, recordingOpen, section])
  if (!context) return children
  // Two files open: B's half has its own window (SecondaryWindowScope).
  if (side?.side === 'B') return <SecondaryWindowScope section={section} context={context}>{children}</SecondaryWindowScope>
  const { window, referenceLapNum } = scope
  const validLapMode = typeof window !== 'number' && window !== 'AL' && window !== 'SL' &&
    clAvailable && (window !== 'RL' || recordingOpen)
  const mode = window === 'AL' || window === 'SL' ? window : validLapMode ? window : null
  return (
    <ScopeContext.Provider value={scope}>
      <ChartCoordinatesProvider
        mode={mode}
        referenceLapNum={referenceLapNum}
        rowTypeMask={GRAPH_SECTION_ROW_MASK[section]}
        sectorBoundaries={context.sectorBoundariesEnabled}
      >
        {children}
      </ChartCoordinatesProvider>
    </ScopeContext.Provider>
  )
}


/**
 * File B's half of a split chart in the window B's own selector picked. Its
 * rows (through the side context) and axes (SecondaryChartCoordinatesProvider):
 *  - Current: B's lap with A's lap number (laps stay in step), drawn up to B's
 *    playhead.
 *  - Previous, Fastest, Selected: that lap, with B's previous lap, fastest lap
 *    or the lap picked as the comparison trace.
 *  - Stint Laps, All Laps: B's whole race so far, from B's stream (which
 *    holds it while this window is shown); Stint Laps from the start of B's
 *    stint.
 *  - a time window: B's history the window's length back from its playhead,
 *    from B's stream likewise.
 */
const EMPTY_BLOCKS: never[] = []

function SecondaryWindowScope({ section, context, children }: { section: GraphSection; context: OverrideContextValue; children: ReactNode }) {
  const secondary = useSecondaryLapState()
  const file = secondary?.file ?? null
  const lapNum = secondary?.lapNum ?? null
  const currentLap = secondary?.lapData ?? null
  const override = context.secondaryOverrides?.[section]
  const lapOverride = context.secondaryReferenceLapOverrides?.[section]
  const referenceLapNum = lapOverride ?? context.secondaryReferenceLapNum ?? null
  const window = effectiveWindow(context.clAvailable, context.recordingOpen, context.globalWindow, override)
  const scope = useMemo<ScopeContextValue>(() => ({
    section, window, windowSeconds: typeof window === 'number' ? window : 30, referenceLapNum, secondary: true,
  }), [referenceLapNum, section, window])
  const rowTypeMask = GRAPH_SECTION_ROW_MASK[section]
  const distance = window === 'CL' || window === 'PL' || window === 'FL' || window === 'RL'
  const allLaps = window === 'AL' || window === 'SL'
  // The comparison lap, from B's own laps.
  const comparisonLapNum = window === 'PL' ? (lapNum !== null && lapNum > 1 ? lapNum - 1 : null)
    : window === 'FL' ? file?.fastestLapNum ?? null
      : window === 'RL' ? referenceLapNum : null
  const comparisonLapData = useSecondaryLapData(distance ? comparisonLapNum : null, rowTypeMask)
  // B's lap starts, for the All Laps ticks and the stint's start.
  const lapBoundaries = useMemo(() => (file?.blocks ?? [])
    .filter(block => block.lapNum >= 1)
    .map(block => ({ lapNum: block.lapNum, sessionTime: block.startSessionTime }))
    .sort((a, b) => a.sessionTime - b.sessionTime), [file])
  // Windows longer than B's lap draw from the history B's stream holds for
  // them: this chart's families, its length back (or the whole race).
  const timeWindow = !distance && !allLaps
  useSecondaryHistoryNeed(allLaps || timeWindow ? {
    mask: (rowTypeMask | DATA_ROW.lap) >>> 0,
    seconds: allLaps ? -1 : typeof window === 'number' ? window : 30,
  } : null)
  const streamed = useSecondaryState()
  const history = allLaps || timeWindow ? streamed.history : null
  // Stint Laps start at B's stint, from B's streamed trends.
  const stintStartTime = window === 'SL' && streamed.trends
    ? lapStartSessionTime(lapBoundaries, streamed.trends.stintStartLap) ?? -Infinity : -Infinity
  const sideValue = useMemo<LapDataSide>(() => ({
    side: 'B', lapNum, lapData: distance ? currentLap : history,
  }), [currentLap, distance, lapNum, history])
  const mode = allLaps ? window : distance ? window : null
  return (
    <ScopeContext.Provider value={scope}>
      <LapDataSideProvider value={sideValue}>
        <SecondaryChartCoordinatesProvider
          mode={mode as Exclude<ChartWindow, number> | null}
          lapData={currentLap}
          comparisonLapData={comparisonLapData}
          lapBoundaries={lapBoundaries}
          stintStartTime={stintStartTime}
          historyKey={`${file?.filePath ?? ''}:${file?.driverIndex ?? ''}`}
          sectorBoundaries={context.sectorBoundariesEnabled}
          lapBlocks={file?.blocks ?? EMPTY_BLOCKS}
          fastestLapNum={file?.fastestLapNum ?? null}
        >
          {children}
        </SecondaryChartCoordinatesProvider>
      </LapDataSideProvider>
      {/* The lap modes need B's lap with A's lap number; time windows and All Laps do not. */}
      {distance && secondary?.missing && (
        <div className="absolute inset-0 flex items-center justify-center bg-[var(--bg-panel)]/85 text-[11px] font-mono text-[var(--text-secondary)] pointer-events-none">
          No lap {lapNum} in this file
        </div>
      )}
    </ScopeContext.Provider>
  )
}

export function useChartWindowSeconds(fallback = 30): number {
  return useContext(ScopeContext)?.windowSeconds ?? fallback
}

/** What one half's selector reads and changes. */
interface WindowBinding {
  window: ChartWindow
  referenceLapNum: number | null
  lapOptions: LapOption[]
  setWindow: (value: ChartWindow) => void
  setReferenceLap: (lapNum: number) => void
}

const ChartWindowOverrideSelectInner = memo(function ChartWindowOverrideSelectInner({
  clAvailable, recordingOpen, binding,
}: { clAvailable: boolean; recordingOpen: boolean; binding: WindowBinding }) {
  const groups = useMemo(
    () => getChartWindowOptionGroups(clAvailable, recordingOpen),
    [clAvailable, recordingOpen],
  )
  const options = useMemo(() => groups.flatMap(group => group.options), [groups])
  const value = options.find(option => option.value === binding.window) ?? null
  const { setWindow, setReferenceLap } = binding
  const handleChange = useCallback((option: SingleValue<ChartWindowOption>) => {
    if (option) setWindow(option.value)
  }, [setWindow])
  const lapMenu = useMemo(() => groupReferenceLapOptions(binding.lapOptions), [binding.lapOptions])
  const lapValue = binding.lapOptions.find(option => option.value === binding.referenceLapNum) ?? null
  const handleLapChange = useCallback((option: SingleValue<LapOption>) => {
    if (option) setReferenceLap(option.value)
  }, [setReferenceLap])
  return (
    <div className="flex shrink-0 items-center gap-1 normal-case tracking-normal">
      <AnimatedAutoWidth measureKey={String(binding.window)}>
      <Select<ChartWindowOption, false, GroupBase<ChartWindowOption>>
        value={value}
        options={groups}
        onChange={handleChange}
        styles={selectStyles}
        components={selectComponents}
        isSearchable={false}
        menuPortalTarget={document.body}
        menuPosition="fixed"
        menuShouldScrollIntoView={false}
        aria-label="Chart window override"
      />
      </AnimatedAutoWidth>
      <div className={`chart-lap-slot ${binding.window === 'RL' ? 'chart-lap-slot--visible' : ''}`}>
        <div className="chart-lap-slot__inner">
          <AnimatedAutoWidth measureKey={String(binding.referenceLapNum ?? '')}>
          <Select<LapOption, false, GroupBase<LapOption>>
            value={lapValue}
            options={lapMenu as LapOption[] | GroupBase<LapOption>[]}
            formatOptionLabel={formatReferenceLapOption}
            formatGroupLabel={formatReferenceLapGroup}
            onChange={handleLapChange}
            placeholder="1"
            styles={lapSelectStyles}
            components={selectComponents}
            isSearchable={false}
            menuPortalTarget={document.body}
            menuPosition="fixed"
            menuShouldScrollIntoView={false}
            aria-label="Selected comparison lap"
          />
          </AnimatedAutoWidth>
        </div>
      </div>
    </div>
  )
})

export const ChartWindowOverrideSelect = memo(function ChartWindowOverrideSelect() {
  const context = useContext(OverrideContext)
  const scope = useContext(ScopeContext)
  if (!context || !scope) return null
  return scope.secondary
    ? <SecondaryWindowSelect context={context} scope={scope} />
    : <PrimaryWindowSelect context={context} scope={scope} />
})

function PrimaryWindowSelect({ context, scope }: { context: OverrideContextValue; scope: ScopeContextValue }) {
  const { setOverride, globalWindow, setReferenceLapOverride, referenceLapNum } = context
  const section = scope.section
  const setWindow = useCallback((value: ChartWindow) => setOverride(section, value === globalWindow ? null : value),
    [globalWindow, section, setOverride])
  const setReferenceLap = useCallback((lapNum: number) => setReferenceLapOverride(section, lapNum === referenceLapNum ? null : lapNum),
    [referenceLapNum, section, setReferenceLapOverride])
  const binding = useMemo<WindowBinding>(() => ({
    window: scope.window, referenceLapNum: scope.referenceLapNum, lapOptions: context.referenceLapOptions, setWindow, setReferenceLap,
  }), [context.referenceLapOptions, scope.referenceLapNum, scope.window, setReferenceLap, setWindow])
  return <ChartWindowOverrideSelectInner clAvailable={context.clAvailable} recordingOpen={context.recordingOpen} binding={binding} />
}

/** B's half: its own selector, drawn in the shared header over B's half. */
function SecondaryWindowSelect({ context, scope }: { context: OverrideContextValue; scope: ScopeContextValue }) {
  const slot = useChartHeaderSlotB()
  const { setSecondaryOverride, globalWindow, setSecondaryReferenceLapOverride, secondaryReferenceLapNum } = context
  const section = scope.section
  const setWindow = useCallback((value: ChartWindow) => setSecondaryOverride?.(section, value === globalWindow ? null : value),
    [globalWindow, section, setSecondaryOverride])
  const setReferenceLap = useCallback((lapNum: number) =>
    setSecondaryReferenceLapOverride?.(section, lapNum === secondaryReferenceLapNum ? null : lapNum),
  [secondaryReferenceLapNum, section, setSecondaryReferenceLapOverride])
  const binding = useMemo<WindowBinding>(() => ({
    window: scope.window, referenceLapNum: scope.referenceLapNum, lapOptions: context.secondaryReferenceLapOptions ?? [], setWindow, setReferenceLap,
  }), [context.secondaryReferenceLapOptions, scope.referenceLapNum, scope.window, setReferenceLap, setWindow])
  if (!slot) return null
  return createPortal(
    <ChartWindowOverrideSelectInner clAvailable={context.clAvailable} recordingOpen={context.recordingOpen} binding={binding} />,
    slot,
  )
}
