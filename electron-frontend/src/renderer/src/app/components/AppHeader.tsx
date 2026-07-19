import { memo, type Dispatch, type SetStateAction } from 'react'
import Select from 'react-select'
import { Maximize, Pencil, PictureInPicture2, Settings2, Shrink, Upload, X } from 'lucide-react'
import { useTelemetryStore } from '../../stores/telemetryStore'
import { buildSelectStyles } from '../../lib/selectStyles'
import { selectComponents } from '../../lib/selectComponents'
import { SESSION_TYPES, sessionAccent } from '../../components/SessionPanel'
import iconTransparent from '../../assets/icon_transparent.png'
import iconTransparentLight from '../../assets/icon_transparent_light.png'
import { TAB_OPTIONS, WINDOW_OPTIONS, type Tab } from '../appConfig'
import type { BannerItem } from '../bannerHelpers'

const selectStyles = buildSelectStyles(true)

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
  seconds: number
  setEditOpen: Dispatch<SetStateAction<boolean>>
  setHeaderVisible: (visible: boolean) => void
  setSeconds: (seconds: number) => void
  setSettingsOpen: (open: boolean) => void
  setTab: (tab: Tab) => void
  settingsOpen: boolean
  tab: Tab
  theme: 'dark' | 'light'
}

const SessionTimer = memo(function SessionTimer() {
  const sessionTime = useTelemetryStore(state => state.latest?.session_time)
  if (sessionTime === undefined) return null
  const formatted = `${Math.floor(sessionTime / 60)}:${String(Math.floor(sessionTime % 60)).padStart(2, '0')}`
  return <div className="text-sm font-black tabular-nums text-[var(--text-primary)] shrink-0">{formatted}</div>
})

export default memo(function AppHeader({
  actualNativeTitlebar, activeBanner, editOpen, filename, headerVisible, isFullscreen,
  isMaximized, onClosePlayback, onSelectPlaybackFile, seconds, setEditOpen,
  setHeaderVisible, setSeconds, setSettingsOpen, setTab, settingsOpen, tab, theme,
}: AppHeaderProps) {
  const sessionType = useTelemetryStore(state => state.session?.session_type)
  const editable = tab === 'core' || tab === 'input' || tab === 'misc' || tab === 'power' || tab === 'tyres'
  const accent = sessionType !== undefined ? sessionAccent(sessionType, theme === 'dark') : null

  return (
    <div
      className={isFullscreen ? `absolute top-0 left-0 right-0 z-50 ${headerVisible ? 'h-10' : 'h-px'}` : ''}
      onMouseEnter={() => { if (isFullscreen) setHeaderVisible(true) }}
      onMouseLeave={() => { if (isFullscreen) setHeaderVisible(false) }}
    >
      <header
        className={`relative flex items-center gap-3 ${actualNativeTitlebar ? 'pl-2 pr-4' : 'px-4'} h-10 select-none ${
          isFullscreen
            ? `transition-all duration-150 ${headerVisible ? 'opacity-100 translate-y-0' : 'opacity-0 -translate-y-full'}`
            : 'sticky top-0 z-10 transition-colors duration-500'
        }`}
        style={activeBanner
          ? { background: `${activeBanner.color}18`, borderColor: `${activeBanner.color}50`, WebkitAppRegion: actualNativeTitlebar ? 'no-drag' : 'drag' }
          : { background: 'var(--bg-panel)', borderColor: 'var(--border)', WebkitAppRegion: actualNativeTitlebar ? 'no-drag' : 'drag' }}
      >
        {!actualNativeTitlebar && (
          <div className="flex items-center gap-2 shrink-0">
            <img src={theme === 'dark' ? iconTransparent : iconTransparentLight} alt="F1 Logo" className="h-5 w-auto select-none pointer-events-none" draggable="false" />
            <span className="font-semibold text-sm max-[1200px]:hidden">Track N Race</span>
          </div>
        )}

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
        <SessionTimer />
        <div className="absolute inset-0 flex items-center justify-center pointer-events-none">
          {activeBanner && (
            <div className="flex items-center gap-2">
              <span className="text-xs font-black uppercase tracking-[0.2em]" style={{ color: activeBanner.color }}>{activeBanner.label}</span>
              {activeBanner.sub && <><span className="text-xs text-[var(--text-secondary)]">·</span><span className="text-xs text-[var(--text-secondary)]">{activeBanner.sub}</span></>}
            </div>
          )}
        </div>

        <div style={{ WebkitAppRegion: 'no-drag' }} className="w-[3.8rem]">
          <Select options={WINDOW_OPTIONS} value={WINDOW_OPTIONS.find(option => option.value === seconds) ?? null} onChange={option => option && setSeconds(option.value as number)} styles={selectStyles} components={selectComponents} isSearchable={false} menuPortalTarget={document.body} />
        </div>

        <button onClick={() => setSettingsOpen(true)} title="Settings" style={{ WebkitAppRegion: 'no-drag' }} className={`p-1.5 rounded transition-colors ${settingsOpen ? 'bg-[var(--border-focus)] text-white' : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'}`}><Settings2 size={13} /></button>
        <button onClick={() => editable && setEditOpen(value => !value)} title="Edit layout" style={{ WebkitAppRegion: 'no-drag' }} className={`p-1.5 rounded transition-colors ${!editable ? 'text-[var(--text-inactive)] cursor-not-allowed' : editOpen ? 'bg-[var(--border-focus)] text-white' : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'}`}><Pencil size={13} /></button>
        <button onClick={() => window.windowControls.minimizeToTray()} title="Background Mode" style={{ WebkitAppRegion: 'no-drag' }} className="p-1.5 rounded transition-colors text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)] shrink-0"><PictureInPicture2 size={13} /></button>
        <button onClick={() => window.windowControls.fullscreen()} title={isFullscreen ? 'Exit Fullscreen' : 'Fullscreen'} style={{ WebkitAppRegion: 'no-drag' }} className="p-1.5 rounded transition-colors text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)] shrink-0">{isFullscreen ? <Shrink size={13} /> : <Maximize size={13} />}</button>

        {!actualNativeTitlebar && !isFullscreen && (
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
