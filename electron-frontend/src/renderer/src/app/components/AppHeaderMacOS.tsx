import { memo, useMemo, useRef, type Dispatch, type SetStateAction } from 'react'
import Select from '../../lib/AnimatedSelect'
import { Columns2, Columns3, Pencil, PictureInPicture2, Settings2 } from 'lucide-react'
import { useTelemetryStore } from '../../stores/telemetryStore'
import { buildSelectStyles } from '../../lib/selectStyles'
import { selectComponents } from '../../lib/selectComponents'
import { sessionAccent } from '../../components/SessionPanel'
import { getChartWindowOptionGroups, TAB_OPTIONS, type ChartWindow, type Tab, type Theme, type TitlebarUpdateInterval } from '../appConfig'
import type { BannerItem } from '../bannerHelpers'
import SessionTimer from './SessionTimer'
import AnimatedAutoWidth from './AnimatedAutoWidth'
import DriverSelect from '../../components/DriverSelect'
import type { DriverOption } from '../../lib/driverOptions'
import { formatTabOptionLabel } from './TabOptionLabel'
import SyncedTooltipIcon from './SyncedTooltipIcon'
import TitlebarFiles from './TitlebarFiles'
import TitlebarDrivers from './TitlebarDrivers'
import { TITLEBAR_SPACER_STYLE, useTitlebarFit } from '../hooks/useTitlebarFit'
import { fileDateLabels } from '../../lib/secondaryFile'
import { formatReferenceLapGroup, formatReferenceLapOption, groupReferenceLapOptions, type ReferenceLapOption } from '../../lib/secondaryComparison'
import type { SecondaryFileData } from '../../components/AnalyzeScreen'

const selectStyles = buildSelectStyles(true)
const tabSelectStyles = buildSelectStyles(true, { menuWidth: '120px' })
const windowSelectStyles = buildSelectStyles(true, { labelStyleGroupHeadings: true, menuWidth: '7rem', scrollableMenu: false })

interface AppHeaderProps {
  actualNativeTitlebar: boolean
  activeBanner: BannerItem | null
  editOpen: boolean
  filename?: string
  headerVisible: boolean
  isFullscreen: boolean
  isMaximized: boolean
  inputCursorSyncEnabled: boolean
  sectorBoundariesEnabled: boolean
  /** Side-by-side A | B charts; off by default, B's laps then sit in the lap selectors. */
  fileSplitEnabled: boolean
  setFileSplitEnabled: (enabled: boolean) => void
  onClosePlayback: () => void
  onSelectPlaybackFile: () => void
  secondaryFile: SecondaryFileData | null
  secondaryLoading: boolean
  secondaryError: string | null
  onSelectSecondaryFile: () => void
  onCloseSecondaryFile: () => void
  chartWindow: ChartWindow
  driverOptions: DriverOption[]
  driverSelectorVisible: boolean
  driverSelectorDisabled: boolean
  selectedDriverIdx: number | null
  clAvailable: boolean
  referenceLapNum: number | null
  referenceLapOptions: ReferenceLapOption[]
  setEditOpen: Dispatch<SetStateAction<boolean>>
  setHeaderVisible: (visible: boolean) => void
  setInputCursorSyncEnabled: (enabled: boolean) => void
  setSectorBoundariesEnabled: (enabled: boolean) => void
  setChartWindow: (window: ChartWindow) => void
  setSelectedDriverIdx: (driverIndex: number) => void
  /** The car followed in the second file; -1 is its recorded player. */
  secondaryDriverIdx: number
  setSecondaryDriverIdx: (driverIndex: number) => void
  setReferenceLapNum: (lapNum: number | null) => void
  setSettingsOpen: (open: boolean) => void
  onOpenUdpSettings: () => void
  setTab: (tab: Tab) => void
  settingsOpen: boolean
  tab: Tab
  theme: Theme
  titlebarUpdateInterval: TitlebarUpdateInterval
  udpListenerError: string | null
}

export default memo(function AppHeader({
  actualNativeTitlebar, activeBanner, editOpen, filename, isFullscreen, inputCursorSyncEnabled, sectorBoundariesEnabled, fileSplitEnabled, setFileSplitEnabled,
  onClosePlayback, onSelectPlaybackFile, secondaryFile, secondaryLoading, secondaryError, onSelectSecondaryFile, onCloseSecondaryFile, chartWindow, clAvailable, driverOptions, driverSelectorVisible, driverSelectorDisabled, selectedDriverIdx, secondaryDriverIdx, setSecondaryDriverIdx, setEditOpen,
  referenceLapNum, referenceLapOptions, setInputCursorSyncEnabled, setSectorBoundariesEnabled, setChartWindow, setSelectedDriverIdx, setReferenceLapNum, setSettingsOpen, setTab, settingsOpen, tab, theme,
  titlebarUpdateInterval, udpListenerError, onOpenUdpSettings,
}: AppHeaderProps) {
  const sessionType = useTelemetryStore(state => state.session?.session_type)
  const editable = tab === 'core' || tab === 'input' || tab === 'misc' || tab === 'power' || tab === 'tyres' || tab === 'session' || tab === 'timing_tower' || tab === 'stint' || tab === 'damage'
  const sectorBoundariesAvailable = tab === 'core' || tab === 'input' || tab === 'misc' || tab === 'power' || tab === 'tyres'
  // Needs a second file and a page whose charts split (Trends included).
  const fileSplitAvailable = !!secondaryFile && (sectorBoundariesAvailable || tab === 'stint')
  // Trends plots per-lap graphs: no sectors, but its graphs share a tooltip.
  const cursorSyncAvailable = sectorBoundariesAvailable || tab === 'stint'
  const accent = sessionType !== undefined ? sessionAccent(sessionType, theme !== 'light') : null
  const isSafetyCarEnding = activeBanner?.sub === 'Ending'
    && (activeBanner.label === 'Safety Car' || activeBanner.label === 'Virtual Safety Car')
  const recordingOpen = Boolean(filename)
  const windowOptionGroups = useMemo(
    () => getChartWindowOptionGroups(clAvailable, recordingOpen),
    [clAvailable, recordingOpen],
  )
  const windowOptions = useMemo(() => windowOptionGroups.flatMap(group => group.options), [windowOptionGroups])
  const referenceLapMenu = useMemo(() => groupReferenceLapOptions(referenceLapOptions), [referenceLapOptions])
  const displayedWindow = typeof chartWindow !== 'number' && chartWindow !== 'AL' && chartWindow !== 'SL' && (!clAvailable || (chartWindow === 'RL' && !filename)) ? 30 : chartWindow
  const selectedLapVisible = recordingOpen && chartWindow === 'RL'
  // Two files open: one picker for both files' drivers (Drivers 2). When the
  // title bar runs out of room it hides the app title, then shortens the
  // drivers to three-letter codes.
  const twoDrivers = driverSelectorVisible && secondaryFile !== null
  const [dateA, dateB] = twoDrivers ? fileDateLabels(filename, secondaryFile.filename) : [null, null]
  const headerRef = useRef<HTMLElement>(null)
  const fit = useTitlebarFit(headerRef, twoDrivers ? 2 : 0)

  return (
    <div
      className={isFullscreen ? 'h-10 shrink-0' : ''}
    >
      <header
        ref={headerRef}
        className={`relative flex items-center gap-3 ${actualNativeTitlebar ? 'pl-2 pr-4' : isFullscreen ? 'px-4' : 'pl-20 pr-4'} h-10 select-none ${
          isFullscreen
            ? 'opacity-100 translate-y-0'
            : 'sticky top-0 z-10'
        }`}
        style={activeBanner
          ? { background: `${activeBanner.color}18`, borderColor: `${activeBanner.color}50`, WebkitAppRegion: actualNativeTitlebar ? 'no-drag' : 'drag' }
          : { background: 'var(--bg-panel)', borderColor: 'var(--border)', WebkitAppRegion: actualNativeTitlebar ? 'no-drag' : 'drag' }}
      >
        <div style={{ WebkitAppRegion: 'no-drag' }}>
          <AnimatedAutoWidth measureKey={tab}>
            <Select options={TAB_OPTIONS} value={TAB_OPTIONS.find(option => option.value === tab) ?? null} onChange={option => option && setTab(option.value as Tab)} formatOptionLabel={formatTabOptionLabel} styles={tabSelectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} />
          </AnimatedAutoWidth>
        </div>

        <TitlebarFiles
          filename={filename}
          sessionType={sessionType}
          accent={accent}
          closeHoverClass="hover:text-[#e10600]"
          onClosePlayback={onClosePlayback}
          onSelectPlaybackFile={onSelectPlaybackFile}
          secondaryFile={secondaryFile}
          secondaryLoading={secondaryLoading}
          secondaryError={secondaryError}
          onSelectSecondaryFile={onSelectSecondaryFile}
          onCloseSecondaryFile={onCloseSecondaryFile}
        />

        {udpListenerError && (
          <button
            type="button"
            title={udpListenerError}
            onClick={onOpenUdpSettings}
            style={{ WebkitAppRegion: 'no-drag' }}
            className="shrink-0 rounded border border-red-500/60 bg-red-500/15 px-2 py-0.5 text-[10px] font-bold uppercase tracking-wide text-red-400 hover:bg-red-500/25"
          >
            UDP error
          </button>
        )}

        <div data-titlebar-spacer className="min-w-0 overflow-hidden px-2 pointer-events-none" style={TITLEBAR_SPACER_STYLE}>
          {activeBanner && (
            <div className="mx-auto flex max-w-full min-w-0 items-center justify-center gap-2 overflow-hidden">
              <span className="max-w-full shrink-0 truncate text-xs font-black uppercase tracking-[0.2em]" style={{ color: `color-mix(in srgb, ${activeBanner.color} 72%, var(--text-primary))` }}>
                {activeBanner.label}{isSafetyCarEnding && ` ${activeBanner.sub}`}
              </span>
              {activeBanner.sub && !isSafetyCarEnding && <><span className="shrink-0 text-xs text-[var(--text-secondary)]">·</span><span className="min-w-0 truncate text-xs text-[var(--text-secondary)]">{activeBanner.sub}</span></>}
            </div>
          )}
        </div>
        <SessionTimer comparisonMode={clAvailable && (chartWindow === 'PL' || chartWindow === 'FL' || (chartWindow === 'RL' && !!filename)) ? chartWindow : null} referenceLapNum={referenceLapNum} updateInterval={titlebarUpdateInterval} />

        <div className={`titlebar-lap-slot ${selectedLapVisible ? 'titlebar-lap-slot--visible' : ''}`}>
          <div className="titlebar-lap-slot__inner" style={{ WebkitAppRegion: 'no-drag' }}>
            <Select options={referenceLapMenu} formatOptionLabel={formatReferenceLapOption} formatGroupLabel={formatReferenceLapGroup} value={referenceLapOptions.find(option => option.value === referenceLapNum) ?? null} onChange={option => setReferenceLapNum(option?.value ?? null)} placeholder="1" styles={selectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} />
          </div>
        </div>

        {twoDrivers && (
          <TitlebarDrivers
            aOptions={driverOptions}
            aSelected={selectedDriverIdx}
            aDisabled={driverSelectorDisabled}
            onChangeA={setSelectedDriverIdx}
            secondaryFile={secondaryFile}
            bSelected={secondaryDriverIdx}
            onChangeB={setSecondaryDriverIdx}
            dateA={dateA ?? 'File A'}
            dateB={dateB ?? 'File B'}
            compact={fit >= 2}
          />
        )}

        {driverSelectorVisible && !twoDrivers && (
          <div title={driverSelectorDisabled
            ? 'This recording saved only your driver.'
            : 'Drivers marked Public have private status and damage values hidden by the game.'} style={{ WebkitAppRegion: 'no-drag' }}>
            <AnimatedAutoWidth measureKey={String(selectedDriverIdx)}>
              <DriverSelect options={driverOptions} selectedDriverIdx={selectedDriverIdx} disabled={driverSelectorDisabled} onChange={idx => { if (idx !== null) setSelectedDriverIdx(idx) }} portal />
            </AnimatedAutoWidth>
          </div>
        )}

        <div style={{ WebkitAppRegion: 'no-drag' }}>
          <AnimatedAutoWidth measureKey={String(displayedWindow)}>
            <Select options={windowOptionGroups} value={windowOptions.find(option => option.value === displayedWindow) ?? null} onChange={option => option && setChartWindow(option.value)} styles={windowSelectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} />
          </AnimatedAutoWidth>
        </div>

        <button onClick={() => setSettingsOpen(true)} title="Settings" style={{ WebkitAppRegion: 'no-drag' }} className={`p-1.5 rounded transition-colors ${settingsOpen ? 'bg-[var(--border-focus)] text-white' : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'}`}><Settings2 size={13} /></button>
        <button
          onClick={() => fileSplitAvailable && setFileSplitEnabled(!fileSplitEnabled)}
          disabled={!fileSplitAvailable}
          aria-label="Side-by-side Charts"
          aria-pressed={fileSplitAvailable && fileSplitEnabled}
          title={fileSplitAvailable ? 'Side-by-side Charts' : undefined}
          style={{ WebkitAppRegion: 'no-drag' }}
          className={`p-1.5 rounded transition-colors ${!fileSplitAvailable ? 'text-[var(--text-inactive)] cursor-not-allowed' : fileSplitEnabled ? 'bg-[var(--border-focus)] text-white' : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'}`}
        ><Columns2 size={13} /></button>
        <button
          onClick={() => sectorBoundariesAvailable && setSectorBoundariesEnabled(!sectorBoundariesEnabled)}
          disabled={!sectorBoundariesAvailable}
          aria-label="Sector Boundaries"
          aria-pressed={sectorBoundariesAvailable && sectorBoundariesEnabled}
          title={sectorBoundariesAvailable ? 'Sector Boundaries' : undefined}
          style={{ WebkitAppRegion: 'no-drag' }}
          className={`p-1.5 rounded transition-colors ${!sectorBoundariesAvailable ? 'text-[var(--text-inactive)] cursor-not-allowed' : sectorBoundariesEnabled ? 'bg-[var(--border-focus)] text-white' : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'}`}
        ><Columns3 size={13} /></button>
        <button
          onClick={() => cursorSyncAvailable && setInputCursorSyncEnabled(!inputCursorSyncEnabled)}
          disabled={!cursorSyncAvailable}
          aria-label="Synchronize Tooltip"
          aria-pressed={cursorSyncAvailable && inputCursorSyncEnabled}
          title={cursorSyncAvailable ? 'Synchronize Tooltip' : undefined}
          style={{ WebkitAppRegion: 'no-drag' }}
          className={`p-1.5 rounded transition-colors ${!cursorSyncAvailable ? 'text-[var(--text-inactive)] cursor-not-allowed' : inputCursorSyncEnabled ? 'bg-[var(--border-focus)] text-white' : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'}`}
        ><SyncedTooltipIcon size={13} /></button>
        <button onClick={() => editable && setEditOpen(value => !value)} title="Edit layout" style={{ WebkitAppRegion: 'no-drag' }} className={`p-1.5 rounded transition-colors ${!editable ? 'text-[var(--text-inactive)] cursor-not-allowed' : editOpen ? 'bg-[var(--border-focus)] text-white' : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'}`}><Pencil size={13} /></button>
        <button onClick={() => window.windowControls.minimizeToTray()} title="Background Mode" style={{ WebkitAppRegion: 'no-drag' }} className="hidden p-1.5 rounded transition-colors text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)] shrink-0"><PictureInPicture2 size={13} /></button>
      </header>
    </div>
  )
})
