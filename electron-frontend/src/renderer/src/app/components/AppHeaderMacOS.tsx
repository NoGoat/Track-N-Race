import { memo, type Dispatch, type SetStateAction } from 'react'
import Select from '../../lib/AnimatedSelect'
import { Maximize, Pencil, PictureInPicture2, Settings2, Shrink, Upload, X } from 'lucide-react'
import { useTelemetryStore } from '../../stores/telemetryStore'
import { buildSelectStyles } from '../../lib/selectStyles'
import { selectComponents } from '../../lib/selectComponents'
import { SESSION_TYPES, sessionAccent } from '../../components/SessionPanel'
import iconTransparent from '../../assets/icon_transparent.png'
import iconTransparentLight from '../../assets/icon_transparent_light.png'
import { getChartWindowOptionGroups, TAB_OPTIONS, type ChartWindow, type Tab, type TitlebarUpdateInterval } from '../appConfig'
import type { BannerItem } from '../bannerHelpers'
import SessionTimer from './SessionTimer'
import AnimatedAutoWidth from './AnimatedAutoWidth'
import { formatTabOptionLabel } from './TabOptionLabel'

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
  onClosePlayback: () => void
  onSelectPlaybackFile: () => void
  chartWindow: ChartWindow
  clAvailable: boolean
  referenceLapNum: number | null
  referenceLapOptions: Array<{ value: number; label: string }>
  setEditOpen: Dispatch<SetStateAction<boolean>>
  setHeaderVisible: (visible: boolean) => void
  setChartWindow: (window: ChartWindow) => void
  setReferenceLapNum: (lapNum: number | null) => void
  setSettingsOpen: (open: boolean) => void
  setTab: (tab: Tab) => void
  settingsOpen: boolean
  tab: Tab
  theme: 'dark' | 'light'
  titlebarUpdateInterval: TitlebarUpdateInterval
}

export default memo(function AppHeader({
  actualNativeTitlebar, activeBanner, editOpen, filename, headerVisible, isFullscreen,
  isMaximized, onClosePlayback, onSelectPlaybackFile, chartWindow, clAvailable, setEditOpen,
  referenceLapNum, referenceLapOptions, setHeaderVisible, setChartWindow, setReferenceLapNum, setSettingsOpen, setTab, settingsOpen, tab, theme,
  titlebarUpdateInterval,
}: AppHeaderProps) {
  const sessionType = useTelemetryStore(state => state.session?.session_type)
  const editable = tab === 'core' || tab === 'input' || tab === 'misc' || tab === 'power' || tab === 'tyres' || tab === 'session' || tab === 'timing_tower'
  const accent = sessionType !== undefined ? sessionAccent(sessionType, theme === 'dark') : null
  const windowOptionGroups = getChartWindowOptionGroups(clAvailable, Boolean(filename))
  const windowOptions = windowOptionGroups.flatMap(group => group.options)
  const displayedWindow = typeof chartWindow !== 'number' && chartWindow !== 'AL' && chartWindow !== 'SL' && (!clAvailable || (chartWindow === 'RL' && !filename)) ? 30 : chartWindow
  const selectedLapVisible = Boolean(filename) && chartWindow === 'RL'

  return (
    <div
      className={isFullscreen ? 'h-10 shrink-0' : ''}
    >
      <header
        className={`relative flex items-center gap-3 ${actualNativeTitlebar ? 'pl-2 pr-4' : isFullscreen ? 'px-4' : 'pl-20 pr-4'} h-10 select-none ${
          isFullscreen
            ? 'opacity-100 translate-y-0'
            : 'sticky top-0 z-10 transition-colors duration-500'
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

        {filename ? (
          <div className="flex items-center gap-1.5 shrink-0">
            <span className="titlebar-filename text-[11px] font-mono text-[var(--text-secondary)] truncate">{filename}</span>
            <button onClick={onClosePlayback} title="Close session data" style={{ WebkitAppRegion: 'no-drag' }} className="p-1 text-[var(--text-secondary)] hover:text-[#e10600] transition-colors"><X size={14} /></button>
          </div>
        ) : (
          <button onClick={onSelectPlaybackFile} title="Load Session Data File (.tnrd)" style={{ WebkitAppRegion: 'no-drag' }} className="p-1.5 rounded text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors shrink-0"><Upload size={14} /></button>
        )}

        {accent ? (
          <span className="text-[10px] font-medium uppercase tracking-wide rounded px-2 py-0.5 select-none shrink-0" style={{ backgroundColor: accent + '22', color: accent }}>{SESSION_TYPES[sessionType!] ?? 'Unknown'}</span>
        ) : (
          <span className="text-[10px] font-medium uppercase tracking-wide rounded px-2 py-0.5 select-none shrink-0 bg-[var(--bg-panel)] text-[var(--text-secondary)]">Offline</span>
        )}

        <div className="flex-1 min-w-0 overflow-hidden px-2 pointer-events-none">
          {activeBanner && (
            <div className="mx-auto flex max-w-full min-w-0 items-center justify-center gap-2 overflow-hidden">
              <span className="max-w-full shrink-0 truncate text-xs font-black uppercase tracking-[0.2em]" style={{ color: `color-mix(in srgb, ${activeBanner.color} 72%, var(--text-primary))` }}>{activeBanner.label}</span>
              {activeBanner.sub && <><span className="shrink-0 text-xs text-[var(--text-secondary)]">·</span><span className="min-w-0 truncate text-xs text-[var(--text-secondary)]">{activeBanner.sub}</span></>}
            </div>
          )}
        </div>
        <SessionTimer comparisonMode={clAvailable && (chartWindow === 'PL' || chartWindow === 'FL' || (chartWindow === 'RL' && !!filename)) ? chartWindow : null} referenceLapNum={referenceLapNum} updateInterval={titlebarUpdateInterval} />

        <div className={`titlebar-lap-slot ${selectedLapVisible ? 'titlebar-lap-slot--visible' : ''}`}>
          <div className="titlebar-lap-slot__inner" style={{ WebkitAppRegion: 'no-drag' }}>
            <Select options={referenceLapOptions} value={referenceLapOptions.find(option => option.value === referenceLapNum) ?? null} onChange={option => setReferenceLapNum(option?.value ?? null)} placeholder="1" styles={selectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} />
          </div>
        </div>

        <div style={{ WebkitAppRegion: 'no-drag' }}>
          <AnimatedAutoWidth measureKey={String(displayedWindow)}>
            <Select options={windowOptionGroups} value={windowOptions.find(option => option.value === displayedWindow) ?? null} onChange={option => option && setChartWindow(option.value)} styles={windowSelectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} />
          </AnimatedAutoWidth>
        </div>

        <button onClick={() => setSettingsOpen(true)} title="Settings" style={{ WebkitAppRegion: 'no-drag' }} className={`p-1.5 rounded transition-colors ${settingsOpen ? 'bg-[var(--border-focus)] text-white' : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'}`}><Settings2 size={13} /></button>
        <button onClick={() => editable && setEditOpen(value => !value)} title="Edit layout" style={{ WebkitAppRegion: 'no-drag' }} className={`p-1.5 rounded transition-colors ${!editable ? 'text-[var(--text-inactive)] cursor-not-allowed' : editOpen ? 'bg-[var(--border-focus)] text-white' : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'}`}><Pencil size={13} /></button>
        <button onClick={() => window.windowControls.minimizeToTray()} title="Background Mode" style={{ WebkitAppRegion: 'no-drag' }} className="p-1.5 rounded transition-colors text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)] shrink-0"><PictureInPicture2 size={13} /></button>
      </header>
    </div>
  )
})
