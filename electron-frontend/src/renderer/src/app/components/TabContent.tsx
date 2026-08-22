import { memo } from 'react'
import { useTelemetryStore } from '../../stores/telemetryStore'
import LiveStats from '../../components/LiveStats'
import SpeedRpmChart from '../../components/SpeedRpmChart'
import GearChart from '../../components/GearChart'
import InputsChart from '../../components/InputsChart'
import GForceChart from '../../components/GForceChart'
import RideHeightChart from '../../components/RideHeightChart'
import SteeringChart from '../../components/SteeringChart'
import RacePanel from '../../components/RacePanel'
import TimingTower from '../../components/TimingTower'
import ThermalPanel from '../../components/ThermalPanel'
import DamagePanel from '../../components/DamagePanel'
import PowerBreakdownChart from '../../components/PowerBreakdownChart'
import PowerStatsBar from '../../components/PowerStatsBar'
import TyresPanel from '../../components/TyresPanel'
import SessionPanel from '../../components/SessionPanel'
import StrategyPanel from '../../components/StrategyPanel'
import AnalyzeScreen, { type AnalyzeFixedLapMode } from '../../components/AnalyzeScreen'
import type { GraphViewState, CompactState, ChartYAxisState } from '../../lib/graphSections'
import type { CoreLayout, InputLayout, MiscLayout, PowerLayout, Tab, TyresLayout } from '../appConfig'
import { useChartCoordinates } from '../../lib/chartCoordinates'

// The tab content is the only part of the UI that consumes the hot (per-frame)
// telemetry slices. Extracting it into its own store-subscribing component is
// what lets App itself stay cold: App renders the header/nav (which never touch
// hot data) plus <TabContent/>, and only TabContent + the active tab's leaves
// re-render at the telemetry rate. The leaves keep their existing prop shapes —
// TabContent selects the slices from the store and passes them down, exactly as
// App used to. All the low-frequency config comes in as props.
interface TabContentProps {
  tab: Tab
  isDark: boolean
  seconds: number
  coreLayout: CoreLayout
  powerLayout: PowerLayout
  tyresLayout: TyresLayout
  inputLayout: InputLayout
  miscLayout: MiscLayout
  graphView: GraphViewState
  compact: CompactState
  chartYAxis: ChartYAxisState
  tyreView: 'cards' | 'graphs'
  tyreWearMode: 'wear' | 'life'
  selectedIdx: number | null
  onSelectDriver: (idx: number) => void
  reduceAnimations: boolean
  sectorColors: boolean
  driversMode: 'dots' | 'both' | 'labels'
  mapTimeout: number
  mapDimmed: boolean
  currentPlaybackLapNum: number | null
  playbackFilename: string | null
  analyzeCompareLapNum: number | null
  onAnalyzeCompareLapChange: (lapNum: number | null) => void
  analyzeFixedLapMode: AnalyzeFixedLapMode
  onAnalyzeFixedLapModeChange: (mode: AnalyzeFixedLapMode) => void
  onAnalyzeDataMaskChange: (mask: number) => void
}

const SubscribedTabContent = memo(function SubscribedTabContent({
  tab, isDark, seconds, coreLayout, powerLayout, tyresLayout, inputLayout, miscLayout,
  graphView, compact, chartYAxis, tyreView, tyreWearMode,
  selectedIdx, onSelectDriver, reduceAnimations, sectorColors, driversMode, mapTimeout,
  mapDimmed, currentPlaybackLapNum,
}: TabContentProps) {
  const coordinates = useChartCoordinates()
  // Hot + cold slices this subtree needs. Only components that render these
  // re-render per frame; App does not.
  const telemetry        = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapTelemetry : s.telemetry)
  const statusHistory    = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapStatusHistory : s.statusHistory)
  const damage           = useTelemetryStore(s => s.damage)
  const damageHistory    = useTelemetryStore(s => coordinates.distanceMode ? s.analyzeLapDamageHistory : s.damageHistory)
  const lap              = useTelemetryStore(s => s.lap)
  const timing           = useTelemetryStore(s => s.timing)
  const latest           = useTelemetryStore(s => s.latest)
  const allStatus        = useTelemetryStore(s => s.allStatus)
  const status           = useTelemetryStore(s => s.status)
  const participants     = useTelemetryStore(s => s.participants)
  const session          = useTelemetryStore(s => s.session)
  const tyreSets         = useTelemetryStore(s => s.tyreSets)
  const raceEvents       = useTelemetryStore(s => s.raceEvents)
  const fastestLapCarIdx = useTelemetryStore(s => s.fastestLapCarIdx)
  const lapTimesByNum    = useTelemetryStore(s => s.lapTimesByNum)
  const isConnected      = useTelemetryStore(s => s.isConnected)
  const protocolStatus   = useTelemetryStore(s => s.protocolStatus)
  const fuelUpperLimit   = useTelemetryStore(s => s.fuelUpperLimit)
  const strategy         = useTelemetryStore(s => s.strategy)

  const selectedCar        = timing?.cars.find(c => c.idx === selectedIdx) ?? null
  const playerDriver       = participants?.drivers.find(d => d.idx === (timing?.player_idx ?? -1)) ?? null
  const selectedDriver     = participants?.drivers.find(d => d.idx === selectedIdx) ?? playerDriver
  const selectedCarStatus  = allStatus?.cars.find(c => c.idx === selectedIdx) ?? null

  return (
    <>
      {tab === 'core' && (() => {
        const visibleDamageCount = Object.values(coreLayout.damageItems).filter(Boolean).length
        const damageTwoRow = visibleDamageCount > 8
        const showStatsPanel = coreLayout.showStats && Object.values(coreLayout.statsCards).some(Boolean)
        const showThermalPanel = coreLayout.showThermal && (
          tyreView === 'graphs'
            ? Object.values(coreLayout.thermalGraphs).some(Boolean)
            : Object.values(coreLayout.thermalCards).some(Boolean)
        )
        const showSpeedChartPanel = coreLayout.showSpeedChart

        let speedChartFlex = 'flex-1'
        let thermalFlex = 'flex-1'

        if (showSpeedChartPanel && showThermalPanel) {
          speedChartFlex = damageTwoRow ? 'flex-[8]' : 'flex-[13]'
          thermalFlex = damageTwoRow ? 'flex-[4]' : 'flex-[7]'
        }

        const thermalCompactCards = tyreView === 'cards'

        return (
        <div className="h-full flex flex-col overflow-hidden">
          <div className="flex-1 min-h-0 flex flex-col bg-[var(--bg-panel)] border-t border-[var(--border)] overflow-hidden divide-y divide-[var(--border)]">
            {showStatsPanel && (
              <div className="shrink-0">
                <LiveStats latest={latest} status={status} lap={lap} damage={damage} isConnected={isConnected} visibleCards={coreLayout.statsCards} isDark={isDark} compact={compact.overviewStats} />
              </div>
            )}
            {showSpeedChartPanel && (
              <div className={`${speedChartFlex} min-h-0`}>
                <SpeedRpmChart data={telemetry} statusHistory={statusHistory} isDark={isDark} view={graphView.overviewTelemetry} windowSeconds={seconds} />
              </div>
            )}
            {showThermalPanel && (
              <div className={thermalCompactCards ? 'shrink-0' : `${thermalFlex} min-h-0`}>
                <ThermalPanel latest={latest} damage={damage} telemetry={telemetry} damageHistory={damageHistory} view={tyreView} tyreWearMode={tyreWearMode} thermalGraphs={coreLayout.thermalGraphs} thermalCards={coreLayout.thermalCards} isDark={isDark} tyresLevel={compact.overviewTyres} graphViews={{ surfaceTemp: graphView.overviewTyreSurface, innerTemp: graphView.overviewTyreInner, brakeTemp: graphView.overviewTyreBrake, tyreLife: graphView.overviewTyreWear }} cardViews={{ fl: graphView.overviewTyreCardFL, fr: graphView.overviewTyreCardFR, rl: graphView.overviewTyreCardRL, rr: graphView.overviewTyreCardRR }} windowSeconds={seconds} yAxis={chartYAxis.overview} />
              </div>
            )}
            {visibleDamageCount > 0 && (
              <div className="shrink-0">
                <DamagePanel connected={!!latest} damage={damage} visibleItems={coreLayout.damageItems} twoRow={damageTwoRow} isDark={isDark} compact={compact.overviewDamage} />
              </div>
            )}
          </div>
        </div>
        )
      })()}
      {tab === 'timing_tower' && (
        <div className="h-full flex flex-col overflow-hidden">
          <div className="flex-1 min-h-0 flex bg-[var(--bg-panel)] border-t border-[var(--border)] overflow-hidden divide-x divide-[var(--border)]">
            <div className="flex-1 min-w-0 overflow-auto">
              <TimingTower
                timing={timing}
                participants={participants}
                allStatus={allStatus}
                fastestLapCarIdx={fastestLapCarIdx}
                selectedIdx={selectedIdx}
                onSelectDriver={onSelectDriver}
                isDark={isDark}
                animationsEnabled={!reduceAnimations}
              />
            </div>
            <div className="w-80 shrink-0 overflow-y-auto">
              <RacePanel
                lap={lap}
                status={status}
                selectedCar={selectedCar}
                selectedDriver={selectedDriver}
                selectedCarStatus={selectedCarStatus}
                playerIdx={timing?.player_idx ?? null}
                isDark={isDark}
              />
            </div>
          </div>
        </div>
      )}
      {tab === 'session' && (
        <div className="h-full overflow-hidden bg-[var(--bg-panel)] border-t border-[var(--border)]">
          <SessionPanel session={session} raceEvents={raceEvents} timing={timing} participants={participants} isDark={isDark} sectorColors={sectorColors} driversMode={driversMode} mapTimeout={mapTimeout} reduceAnimations={reduceAnimations} mapDimmed={mapDimmed} aeroMode={protocolStatus?.aero_mode ?? 'drs'} compactHeader={compact.sessionHeader} compactCards={compact.sessionCards} compactWeather={compact.sessionWeather} />
        </div>
      )}
      {tab === 'input' && (
        <div className="h-full flex flex-col overflow-hidden">
          <div className="flex-1 min-h-0 flex flex-col bg-[var(--bg-panel)] border-t border-[var(--border)] overflow-hidden divide-y divide-[var(--border)]">
            {(inputLayout.showGear || inputLayout.showInputs) && (
              <div className="flex-1 min-h-0 flex divide-x divide-[var(--border)]">
                {inputLayout.showGear && (
                  <div className="flex-1 min-w-0 min-h-0">
                    <GearChart isDark={isDark} view={graphView.inputGear} windowSeconds={seconds} />
                  </div>
                )}
                {inputLayout.showInputs && (
                  <div className="flex-1 min-w-0 min-h-0">
                    <InputsChart isDark={isDark} view={graphView.inputThrottleBrake} windowSeconds={seconds} />
                  </div>
                )}
              </div>
            )}
            {inputLayout.showSteering && (
              <div className="flex-1 min-h-0">
                <SteeringChart isDark={isDark} view={graphView.inputSteering} windowSeconds={seconds} />
              </div>
            )}
          </div>
        </div>
      )}

      {tab === 'power' && (
        <div className="h-full flex flex-col overflow-hidden border-t border-[var(--border)] divide-y divide-[var(--border)]">
          <div className="shrink-0 bg-[var(--bg-panel)]">
            <PowerStatsBar status={status} visibleCards={powerLayout.statsCards} isDark={isDark} compact={compact.powerCards} />
          </div>
          <div className="flex-1 min-h-0">
            <PowerBreakdownChart data={statusHistory} isDark={isDark} visibleCharts={powerLayout.charts} views={{ powerSplit: graphView.powerSplit, ersHarvest: graphView.powerHarvest, ersStore: graphView.powerStore, fuelHistory: graphView.powerFuel }} windowSeconds={seconds} fuelUpperLimit={fuelUpperLimit} hasMguh={protocolStatus?.capabilities.hasMguh ?? false} ersHarvestYAxis={chartYAxis.power.ersHarvest} />
          </div>
        </div>
      )}
      {tab === 'tyres' && (
        <div className="h-full overflow-hidden border-t border-[var(--border)]">
          <TyresPanel
            tyreSets={tyreSets}
            latest={latest}
            damage={damage}
            damageHistory={damageHistory}
            telemetry={telemetry}
            tyreWearMode={tyreWearMode}
            isDark={isDark}
            visibleGraphs={tyresLayout.charts}
            graphViews={{ surfaceTemp: graphView.tyreSurface, innerTemp: graphView.tyreInner, brakeTemp: graphView.tyreBrake, tyreLife: graphView.tyreWear }}
            cardViews={{ fl: graphView.tyreCardFL, fr: graphView.tyreCardFR, rl: graphView.tyreCardRL, rr: graphView.tyreCardRR }}
            sessionType={session?.session_type ?? null}
            windowSeconds={seconds}
            yAxis={chartYAxis.tyres}
          />
        </div>
      )}
      {tab === 'strategy' && (
        <div className="h-full overflow-hidden bg-[var(--bg-panel)] border-t border-[var(--border)]">
          <StrategyPanel
            strategy={strategy}
            isDark={isDark}
            compact={compact.strategySummary}
          />
        </div>
      )}
    </>
  )
})

// Misc owns no broad telemetry subscription. Its two chart leaves subscribe
// directly to motion and motionEx, so unrelated store publications cannot
// re-render this tab container.
const MiscTabContent = memo(function MiscTabContent({
  isDark, seconds, miscLayout, graphView,
}: Pick<TabContentProps, 'isDark' | 'seconds' | 'miscLayout' | 'graphView'>) {
  return (
    <div className="h-full flex flex-col overflow-hidden">
      <div className="flex-1 min-h-0 flex flex-col bg-[var(--bg-panel)] border-t border-[var(--border)] overflow-hidden divide-y divide-[var(--border)]">
        {miscLayout.showGForce && (
          <div className="flex-1 min-h-0">
            <GForceChart isDark={isDark} view={graphView.miscGForce} windowSeconds={seconds} />
          </div>
        )}
        {miscLayout.showRideHeight && (
          <div className="flex-1 min-h-0">
            <RideHeightChart isDark={isDark} view={graphView.miscRideHeight} windowSeconds={seconds} />
          </div>
        )}
      </div>
    </div>
  )
})

const TabContent = memo(function TabContent(props: TabContentProps) {
  if (props.tab === 'analyze') {
    return <AnalyzeScreen
      isDark={props.isDark}
      playbackFilename={props.playbackFilename}
      currentLapNum={props.currentPlaybackLapNum}
      compareLapNum={props.analyzeCompareLapNum}
      onCompareLapChange={props.onAnalyzeCompareLapChange}
      fixedLapMode={props.analyzeFixedLapMode}
      onFixedLapModeChange={props.onAnalyzeFixedLapModeChange}
      mapDimmed={props.mapDimmed}
      reduceAnimations={props.reduceAnimations}
      sectorColors={props.sectorColors}
      onDataMaskChange={props.onAnalyzeDataMaskChange}
    />
  }
  if (props.tab === 'misc') {
    return <MiscTabContent isDark={props.isDark} seconds={props.seconds} miscLayout={props.miscLayout} graphView={props.graphView} />
  }
  return <SubscribedTabContent {...props} />
})

export default TabContent
