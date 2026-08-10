import { memo, type Dispatch, type SetStateAction } from 'react'
import Select from 'react-select'
import { Maximize, Pencil, PictureInPicture2, Settings2, Shrink, Upload, X } from 'lucide-react'
import { useTelemetryStore } from '../../stores/telemetryStore'
import { buildSelectStyles } from '../../lib/selectStyles'
import { selectComponents, selectComponentsWithSeparator } from '../../lib/selectComponents'
import { SESSION_TYPES, sessionAccent } from '../../components/SessionPanel'
import iconTransparent from '../../assets/icon_transparent.png'
import iconTransparentLight from '../../assets/icon_transparent_light.png'
import { TAB_OPTIONS, WINDOW_OPTIONS, type ChartWindow, type Tab, type TitlebarUpdateInterval } from '../appConfig'
import type { BannerItem } from '../bannerHelpers'
import SessionTimer from './SessionTimer'

const selectStyles = buildSelectStyles(true)
const windowSeparator = { value: 'separator' as const, label: '', isDisabled: true, isSeparator: true as const }

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
  const editable = tab === 'core' || tab === 'input' || tab === 'misc' || tab === 'power' || tab === 'tyres'
  const accent = sessionType !== undefined ? sessionAccent(sessionType, theme === 'dark') : null
  const windowOptions = clAvailable
    ? [{ value: 'CL' as const, label: 'CL' }, { value: 'PL' as const, label: 'PL' }, { value: 'FL' as const, label: 'FL' }, ...(filename ? [{ value: 'RL' as const, label: 'RL' }] : []), windowSeparator, ...WINDOW_OPTIONS]
    : WINDOW_OPTIONS
  const displayedWindow = typeof chartWindow !== 'number' && (!clAvailable || (chartWindow === 'RL' && !filename)) ? 30 : chartWindow

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
        <div style={{ WebkitAppRegion: 'no-drag' }} className="w-full max-w-[100px]">
          <Select options={TAB_OPTIONS} value={TAB_OPTIONS.find(option => option.value === tab) ?? null} onChange={option => option && setTab(option.value as Tab)} styles={selectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} />
        </div>

        {filename ? (
          <div className="flex items-center gap-1.5 shrink-0">
            <span className="text-[11px] font-mono text-[var(--text-secondary)] max-w-[200px] truncate">{filename}</span>
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

        <div className="flex-1" />
        <SessionTimer comparisonMode={clAvailable && (chartWindow === 'PL' || chartWindow === 'FL' || (chartWindow === 'RL' && !!filename)) ? chartWindow : null} referenceLapNum={referenceLapNum} updateInterval={titlebarUpdateInterval} />
        <div className="absolute inset-0 flex items-center justify-center pointer-events-none">
          {activeBanner && (
            <div className="flex items-center gap-2">
              <span className="text-xs font-black uppercase tracking-[0.2em]" style={{ color: activeBanner.color }}>{activeBanner.label}</span>
              {activeBanner.sub && <><span className="text-xs text-[var(--text-secondary)]">·</span><span className="text-xs text-[var(--text-secondary)]">{activeBanner.sub}</span></>}
            </div>
          )}
        </div>

        {filename && chartWindow === 'RL' && (
          <div style={{ WebkitAppRegion: 'no-drag' }} className="w-[3rem]">
            <Select options={referenceLapOptions} value={referenceLapOptions.find(option => option.value === referenceLapNum) ?? null} onChange={option => setReferenceLapNum(option?.value ?? null)} placeholder="—" styles={selectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} />
          </div>
        )}

        <div style={{ WebkitAppRegion: 'no-drag' }} className="w-[3.8rem]">
          <Select options={windowOptions} value={windowOptions.find(option => option.value === displayedWindow) ?? null} onChange={option => option && option.value !== 'separator' && setChartWindow(option.value)} styles={selectStyles} components={selectComponentsWithSeparator} isSearchable={false} menuPortalTarget={document.body} />
        </div>

        <button onClick={() => setSettingsOpen(true)} title="Settings" style={{ WebkitAppRegion: 'no-drag' }} className={`p-1.5 rounded transition-colors ${settingsOpen ? 'bg-[var(--border-focus)] text-white' : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'}`}><Settings2 size={13} /></button>
        <button onClick={() => editable && setEditOpen(value => !value)} title="Edit layout" style={{ WebkitAppRegion: 'no-drag' }} className={`p-1.5 rounded transition-colors ${!editable ? 'text-[var(--text-inactive)] cursor-not-allowed' : editOpen ? 'bg-[var(--border-focus)] text-white' : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'}`}><Pencil size={13} /></button>
        <button onClick={() => window.windowControls.minimizeToTray()} title="Background Mode" style={{ WebkitAppRegion: 'no-drag' }} className="p-1.5 rounded transition-colors text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)] shrink-0"><PictureInPicture2 size={13} /></button>
      </header>
    </div>
  )
})
