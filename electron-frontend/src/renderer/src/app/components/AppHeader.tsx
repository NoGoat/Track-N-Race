import { memo, useMemo, useRef, type Dispatch, type SetStateAction } from 'react'
import Select from '../../lib/AnimatedSelect'
import { Columns2, Columns3, Maximize, Pencil, PictureInPicture2, Settings2, Shrink } from 'lucide-react'
import { useTelemetryStore } from '../../stores/telemetryStore'
import { buildSelectStyles } from '../../lib/selectStyles'
import { selectComponents } from '../../lib/selectComponents'
import { sessionAccent } from '../../components/SessionPanel'
import iconTransparent from '../../assets/icon_transparent.png'
import iconTransparentLight from '../../assets/icon_transparent_light.png'
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
  filename: string | null | undefined
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
  actualNativeTitlebar, activeBanner, editOpen, filename, headerVisible, isFullscreen, inputCursorSyncEnabled, sectorBoundariesEnabled, fileSplitEnabled, setFileSplitEnabled,
  isMaximized, onClosePlayback, onSelectPlaybackFile, secondaryFile, secondaryLoading, secondaryError, onSelectSecondaryFile, onCloseSecondaryFile, chartWindow, clAvailable, driverOptions, driverSelectorVisible, driverSelectorDisabled, selectedDriverIdx, secondaryDriverIdx, setSecondaryDriverIdx, setEditOpen,
  referenceLapNum, referenceLapOptions, setHeaderVisible, setInputCursorSyncEnabled, setSectorBoundariesEnabled, setChartWindow, setSelectedDriverIdx, setReferenceLapNum, setSettingsOpen, setTab, settingsOpen, tab, theme,
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
  const usesTitleBarOverlay = (window.platform === 'win32' || window.platform === 'linux') && !actualNativeTitlebar
  const headerPadding = actualNativeTitlebar
    ? 'pl-2 pr-4'
    : 'px-4'
  const titleBarSafePadding = usesTitleBarOverlay && !isFullscreen
    ? {
        paddingLeft: 'calc(env(titlebar-area-x, 0px) + 1rem)',
        paddingRight: 'calc(100% - env(titlebar-area-x, 0px) - env(titlebar-area-width, 100%) + 1rem)',
      }
    : undefined
  const headerBackground = activeBanner
    ? isFullscreen
      ? `color-mix(in srgb, ${activeBanner.color} 9%, var(--bg-panel))`
      : `${activeBanner.color}18`
    : 'var(--bg-panel)'
  const headerBorderColor = activeBanner ? `${activeBanner.color}50` : 'var(--border)'
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
      className={isFullscreen ? `absolute top-0 left-0 right-0 z-50 ${headerVisible ? 'h-10' : 'h-px'}` : ''}
      onMouseEnter={() => { if (isFullscreen) setHeaderVisible(true) }}
      onMouseLeave={() => { if (isFullscreen) setHeaderVisible(false) }}
    >
      <header
        ref={headerRef}
        className={`relative flex items-center gap-3 ${headerPadding} h-10 select-none ${
          isFullscreen
            ? `transition-all duration-150 ${headerVisible ? 'opacity-100 translate-y-0' : 'opacity-0 -translate-y-full'}`
            : 'sticky top-0 z-10'
        }`}
        style={{
          background: headerBackground,
          borderColor: headerBorderColor,
          WebkitAppRegion: actualNativeTitlebar ? 'no-drag' : 'drag',
          ...titleBarSafePadding,
        }}
      >
        {!actualNativeTitlebar && (
          <div className="flex items-center gap-2 shrink-0">
            <img src={theme === 'light' ? iconTransparentLight : iconTransparent} alt="F1 Logo" className="h-5 w-auto select-none pointer-events-none" draggable="false" />
            {fit >= 1
              // Hidden for room, but still measured so it can come back when it fits.
              ? <span aria-hidden className="absolute w-0 h-0 overflow-hidden invisible"><span data-titlebar-title-measure className="inline-block whitespace-nowrap font-semibold text-sm max-[1400px]:hidden">Track N Race</span></span>
              : <span data-titlebar-title-measure className="font-semibold text-sm max-[1400px]:hidden">Track N Race</span>}
          </div>
        )}

        <div style={{ WebkitAppRegion: 'no-drag' }}>
          <AnimatedAutoWidth measureKey={tab}>
            <Select options={TAB_OPTIONS} value={TAB_OPTIONS.find(option => option.value === tab) ?? null} onChange={option => option && setTab(option.value as Tab)} formatOptionLabel={formatTabOptionLabel} styles={tabSelectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} />
          </AnimatedAutoWidth>
        </div>

        <TitlebarFiles
          filename={filename}
          sessionType={sessionType}
          accent={accent}
          closeHoverClass="hover:text-[#d44252]"
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
        <button onClick={() => window.windowControls.fullscreen()} title={isFullscreen ? 'Exit Fullscreen' : 'Fullscreen'} style={{ WebkitAppRegion: 'no-drag' }} className="p-1.5 rounded transition-colors text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)] shrink-0">{isFullscreen ? <Shrink size={13} /> : <Maximize size={13} />}</button>

        {!actualNativeTitlebar && !usesTitleBarOverlay && !isFullscreen && (
          <div className="flex self-stretch ml-2 -mr-4 shrink-0" style={{ WebkitAppRegion: 'no-drag' }}>
            <button onClick={() => window.windowControls.minimize()} title="Minimize" className="h-full px-4 text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors flex items-center justify-center"><svg width="10" height="10" viewBox="0 0 10 10" fill="currentColor"><rect y="4.5" width="10" height="1" /></svg></button>
            <button onClick={() => window.windowControls.maximize()} title={isMaximized ? 'Restore' : 'Maximize'} className="h-full px-4 text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors flex items-center justify-center">{isMaximized ? <svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1"><polyline points="3,0.5 9.5,0.5 9.5,7" /><rect x="0.5" y="3" width="6.5" height="6.5" /></svg> : <svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1"><rect x="0.5" y="0.5" width="9" height="9" /></svg>}</button>
            <button onClick={() => window.windowControls.close()} title="Close" className="h-full px-4 text-[var(--text-secondary)] hover:text-white hover:bg-[#e10600] transition-colors flex items-center justify-center"><svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1"><line x1="0" y1="0" x2="10" y2="10" /><line x1="10" y1="0" x2="0" y2="10" /></svg></button>
          </div>
        )}
      </header>
    </div>
  )
})
