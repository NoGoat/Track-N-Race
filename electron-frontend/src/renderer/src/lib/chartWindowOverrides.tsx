import { createContext, memo, useCallback, useContext, useMemo } from 'react'
import { type GroupBase, type SingleValue } from 'react-select'
import Select from './AnimatedSelect'
import { ChartCoordinatesProvider } from './chartCoordinates'
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
type LapOption = { value: number; label: string }

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
}

const OverrideContext = createContext<OverrideContextValue | null>(null)

interface ScopeContextValue {
  section: GraphSection
  window: ChartWindow
  windowSeconds: number
  referenceLapNum: number | null
}

const ScopeContext = createContext<ScopeContextValue | null>(null)

const SECTION_ROW_MASK: Record<GraphSection, number> = {
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
  referenceLapOptions, referenceLapOverrides, setReferenceLapOverride, children,
}: OverrideContextValue & { children: React.ReactNode }) {
  const value = useMemo(() => ({
    globalWindow, overrides, setOverride, clAvailable, recordingOpen, referenceLapNum,
    referenceLapOptions, referenceLapOverrides, setReferenceLapOverride,
  }), [clAvailable, globalWindow, overrides, recordingOpen, referenceLapNum, referenceLapOptions,
    referenceLapOverrides, setOverride, setReferenceLapOverride])
  return <OverrideContext.Provider value={value}>{children}</OverrideContext.Provider>
}

export function ChartWindowScope({ section, children }: { section: GraphSection; children: React.ReactNode }) {
  const context = useContext(OverrideContext)
  if (!context) return children
  const availableWindows = getChartWindowOptionGroups(context.clAvailable, context.recordingOpen)
    .flatMap(group => group.options)
    .map(option => option.value)
  const isAvailable = (value: ChartWindow): boolean => availableWindows.includes(value)
  const titlebarWindow = isAvailable(context.globalWindow) ? context.globalWindow : 30
  const override = context.overrides[section]
  const window = override !== undefined && isAvailable(override) ? override : titlebarWindow
  const validLapMode = typeof window !== 'number' && window !== 'AL' && window !== 'SL' &&
    context.clAvailable && (window !== 'RL' || context.recordingOpen)
  const mode = window === 'AL' || window === 'SL' ? window : validLapMode ? window : null
  const windowSeconds = typeof window === 'number' ? window : 30
  const referenceLapNum = context.referenceLapOverrides[section] ?? context.referenceLapNum
  return (
    <ScopeContext.Provider value={{ section, window, windowSeconds, referenceLapNum }}>
      <ChartCoordinatesProvider
        mode={mode}
        referenceLapNum={referenceLapNum}
        rowTypeMask={SECTION_ROW_MASK[section]}
      >
        {children}
      </ChartCoordinatesProvider>
    </ScopeContext.Provider>
  )
}

export function useChartWindowSeconds(fallback = 30): number {
  return useContext(ScopeContext)?.windowSeconds ?? fallback
}

const ChartWindowOverrideSelectInner = memo(function ChartWindowOverrideSelectInner({
  context, scope,
}: { context: OverrideContextValue; scope: ScopeContextValue }) {
  const groups = useMemo(
    () => getChartWindowOptionGroups(context.clAvailable, context.recordingOpen),
    [context.clAvailable, context.recordingOpen],
  )
  const options = useMemo(() => groups.flatMap(group => group.options), [groups])
  const value = options.find(option => option.value === scope.window) ?? null
  const handleChange = useCallback((option: SingleValue<ChartWindowOption>) => {
    if (!option) return
    context.setOverride(scope.section, option.value === context.globalWindow ? null : option.value)
  }, [context, scope.section])
  const lapValue = context.referenceLapOptions.find(option => option.value === scope.referenceLapNum) ?? null
  const handleLapChange = useCallback((option: SingleValue<LapOption>) => {
    if (!option) return
    context.setReferenceLapOverride(
      scope.section,
      option.value === context.referenceLapNum ? null : option.value,
    )
  }, [context, scope.section])
  return (
    <div className="flex shrink-0 items-center gap-1 normal-case tracking-normal">
      <AnimatedAutoWidth measureKey={String(scope.window)}>
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
      <div className={`chart-lap-slot ${scope.window === 'RL' ? 'chart-lap-slot--visible' : ''}`}>
        <div className="chart-lap-slot__inner">
          <AnimatedAutoWidth measureKey={String(scope.referenceLapNum ?? '')}>
          <Select<LapOption, false>
            value={lapValue}
            options={context.referenceLapOptions}
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
  return <ChartWindowOverrideSelectInner context={context} scope={scope} />
})
