import React, { useState, useEffect, useRef, useMemo, memo, useCallback } from 'react'
import Select, { type SingleValue } from 'react-select'
import { buildSelectStyles } from './lib/selectStyles'
import { selectComponents } from './lib/selectComponents'
import { Settings2, Pencil, Shrink, X, Upload, Play, Pause, ChevronLeft, ChevronRight, AlertTriangle, Radio, Activity, Globe, Users, Sliders, Zap, Disc, BarChart2 } from 'lucide-react'
import { useTelemetry } from './hooks/useTelemetry'
import { useAppConfig } from './hooks/useAppConfig'
import LiveStats from './components/LiveStats'
import SpeedRpmChart from './components/SpeedRpmChart'
import GearChart from './components/GearChart'
import InputsChart from './components/InputsChart'
import GForceChart from './components/GForceChart'
import RideHeightChart from './components/RideHeightChart'
import SteeringChart from './components/SteeringChart'
import RacePanel from './components/RacePanel'
import TimingTower from './components/TimingTower'
import ThermalPanel from './components/ThermalPanel'
import DamagePanel from './components/DamagePanel'
import Settings from './components/Settings'
import PowerBreakdownChart from './components/PowerBreakdownChart'
import PowerStatsBar from './components/PowerStatsBar'
import TyresPanel from './components/TyresPanel'
import SessionPanel, { SESSION_TYPES, sessionAccent } from './components/SessionPanel'
import type { RaceEventMsg, ParticipantsMsg } from './types'
import iconTransparent from './assets/icon_transparent.png'
import iconTransparentLight from './assets/icon_transparent_light.png'

const WINDOWS: { label: string; value: number }[] = [
  { label: '15s', value: 15 },
  { label: '30s', value: 30 },
  { label: '1m', value: 60 },
  { label: '2m', value: 120 },
  { label: '5m', value: 300 },
  { label: '10m', value: 600 },
]

type Tab = 'core' | 'timing_tower' | 'input' | 'misc' | 'power' | 'tyres' | 'session'

interface CoreLayout {
  showStats:      boolean
  showSpeedChart: boolean
  showThermal:    boolean
  statsCards: {
    speed: boolean; rpm: boolean; gear: boolean; throttle: boolean; brake: boolean
    drs: boolean; engine: boolean; ers: boolean; fuel: boolean; pos: boolean; tyre: boolean
  }
  thermalGraphs: {
    surfaceTemp: boolean; innerTemp: boolean; brakeTemp: boolean; tyreLife: boolean
  }
  thermalCards: {
    fl: boolean; fr: boolean; rl: boolean; rr: boolean
  }
  damageItems: {
    wingFl: boolean; wingFr: boolean; wingRear: boolean; floor: boolean
    diffuser: boolean; sidepod: boolean; gearbox: boolean; engine: boolean
    tyreDmgFl: boolean; tyreDmgFr: boolean; tyreDmgRl: boolean; tyreDmgRr: boolean
    brakeDmgFl: boolean; brakeDmgFr: boolean; brakeDmgRl: boolean; brakeDmgRr: boolean
  }
}

const DEFAULT_CORE_LAYOUT: CoreLayout = {
  showStats: true, showSpeedChart: true, showThermal: true,
  statsCards:    { speed: true, rpm: true, gear: true, throttle: true, brake: true, drs: true, engine: true, ers: true, fuel: true, pos: true, tyre: true },
  thermalGraphs: { surfaceTemp: true, innerTemp: true, brakeTemp: true, tyreLife: true },
  thermalCards:  { fl: true, fr: true, rl: true, rr: true },
  damageItems:   { wingFl: true, wingFr: true, wingRear: true, floor: true, diffuser: true, sidepod: false, gearbox: true, engine: true, tyreDmgFl: false, tyreDmgFr: false, tyreDmgRl: false, tyreDmgRr: false, brakeDmgFl: false, brakeDmgFr: false, brakeDmgRl: false, brakeDmgRr: false },
}

interface InputLayout {
  showGear:     boolean
  showInputs:   boolean
  showSteering: boolean
}

const DEFAULT_INPUT_LAYOUT: InputLayout = {
  showGear: true, showInputs: true, showSteering: true,
}

interface MiscLayout {
  showGForce: boolean
  showRideHeight: boolean
}

const DEFAULT_MISC_LAYOUT: MiscLayout = { showGForce: true, showRideHeight: true }

interface PowerLayout {
  statsCards: { totalPower: boolean; ice: boolean; mguk: boolean; split: boolean; ersStore: boolean; ersPct: boolean; fuel: boolean }
  charts:     { powerSplit: boolean; ersHarvest: boolean; ersStore: boolean; fuelHistory: boolean }
}

const DEFAULT_POWER_LAYOUT: PowerLayout = {
  statsCards: { totalPower: true, ice: true, mguk: true, split: true, ersStore: true, ersPct: true, fuel: true },
  charts:     { powerSplit: true, ersHarvest: true, ersStore: true, fuelHistory: true },
}

interface TyresLayout {
  charts: { surfaceTemp: boolean; innerTemp: boolean; brakeTemp: boolean; tyreLife: boolean }
}

const DEFAULT_TYRES_LAYOUT: TyresLayout = {
  charts: { surfaceTemp: true, innerTemp: true, brakeTemp: true, tyreLife: true },
}

const TAB_LABELS: Record<Tab, string> = {
  core: 'Overview', timing_tower: 'Standings', input: 'Input', power: 'Power', tyres: 'Tyres', session: 'Session', misc: 'Misc'
}

const TAB_OPTIONS = (['core', 'session', 'timing_tower', 'input', 'power', 'tyres', 'misc'] as Tab[]).map(t => ({ value: t, label: TAB_LABELS[t] }))
const TAB_ORDER: Tab[] = ['core', 'session', 'timing_tower', 'input', 'power', 'tyres', 'misc']
const TAB_ICONS: Record<Tab, React.ElementType> = {
  core:         Activity,
  session:      Globe,
  timing_tower: Users,
  input:        Sliders,
  power:        Zap,
  tyres:        Disc,
  misc:         BarChart2,
}
const WINDOW_OPTIONS = WINDOWS.map(w => ({ value: w.value, label: w.label }))
const selectStyles = buildSelectStyles(true)


interface BannerItem {
  label: string
  sub?: string
  color: string
}

function lastName(participants: ParticipantsMsg | null, idx: number): string {
  const d = participants?.drivers.find(d => d.idx === idx)
  if (!d) return `Car ${idx}`
  const parts = d.name.trim().split(/\s+/)
  return parts[parts.length - 1]
}

function fmtLap(s: number): string {
  const m   = Math.floor(s / 60)
  const rem = s % 60
  return `${m}:${rem.toFixed(3).padStart(6, '0')}`
}

const INFRINGEMENT_LABELS: Record<number, string> = {
  0:  'Blocking by slowing',        1:  'Blocking wrong way',
  2:  'Reversing off start line',   3:  'Big collision',
  4:  'Small collision',            5:  'Collision — failed to hand back',
  6:  'Collision — attack from rear',
  7:  'SC delta exceeded',          8:  'SC illegal overtake',
  9:  'SC exceeding allowed pace',  10: 'Cornering under SC',
  11: 'SC must pit this lap',       12: 'SC pit lane curfew',
  13: 'Pit lane too fast',          14: 'Unsafe release',
  15: 'Pit re-entry too slow',      16: 'In pit too fast',
  17: 'Unsafe release',             18: 'Escape from pit',
  19: 'Ignoring blue flags',        20: 'Ignoring yellow flags',
  21: 'Ignoring drive through',     22: 'Too many drive throughs',
  23: 'DT — serve this lap',        24: 'DT — serve next lap',
  25: 'Pit stop failed to serve',   26: 'Hanging around',
  27: 'Hang around for SC',         28: 'Return to pits',
  29: 'Tyre regulations',           30: 'Lap invalidated',
  31: 'This + next lap invalid',    32: 'Lap invalid (no reason)',
  33: 'This + next invalid (no reason)', 34: 'This + prev lap invalid',
  35: 'This + prev invalid (no reason)', 36: 'Retired',
  37: 'Black flag timer',           38: 'Unserved stop-go',
  39: 'Unserved drive through',     40: 'Engine change',
  41: 'Gearbox change',             42: 'Parc fermé change',
  43: 'League grid penalty',        44: 'Retry penalty',
  45: 'Illegal time gain',          46: 'Mandatory pit stop',
  47: 'Attribute assigned',         48: 'Corner cutting',
}

function buildBanner(event: RaceEventMsg, participants: ParticipantsMsg | null): BannerItem | null {
  switch (event.code) {
    case 'SCAR': return null  // driven by session packet
    case 'FTLP':
      return { label: 'Fastest Lap', sub: `${lastName(participants, event.car_idx ?? 0)}  ·  ${fmtLap(event.lap_time_s ?? 0)}`, color: '#BF5FFF' }
    case 'DRSE': return { label: 'DRS Enabled',  color: '#37872D' }
    case 'DRSD': return { label: 'DRS Disabled', color: '#8e8e8e' }
    case 'RDFL': return { label: 'Red Flag',     color: '#e10600' }
    case 'PENA': {
      const pt = event.penalty_type ?? 0
      const LABELS: Record<number, string> = {
        0: 'Drive Through', 1: 'Stop Go',      2: 'Grid Penalty',
        4: 'Time Penalty',  5: 'Warning',       6: 'Disqualified',
      }
      const COLORS: Record<number, string> = {
        0: '#e10600', 1: '#e10600', 2: '#c47d0e',
        4: '#c47d0e', 5: '#ffd700', 6: '#e10600',
      }
      if (!(pt in LABELS)) return null
      const timeStr = (pt === 1 || pt === 4) && event.penalty_time_s ? ` ${event.penalty_time_s}s` : ''
      const driver = lastName(participants, event.car_idx ?? 0)
      const infringement = event.infringement_type != null ? INFRINGEMENT_LABELS[event.infringement_type] : undefined
      const sub = pt === 5 && infringement ? `${driver}  ·  ${infringement}` : driver
      return { label: LABELS[pt] + timeStr, sub, color: COLORS[pt] }
    }
    case 'DTSV': return { label: 'DT Served',     sub: lastName(participants, event.car_idx ?? 0), color: '#a0a8b8' }
    case 'SGSV': return { label: 'SG Served',     sub: lastName(participants, event.car_idx ?? 0), color: '#a0a8b8' }
    case 'RTMT': return { label: 'Retired',        sub: lastName(participants, event.car_idx ?? 0), color: '#a0a8b8' }
    case 'RCWN': return { label: 'Race Winner',    sub: lastName(participants, event.car_idx ?? 0), color: '#FFD700' }
    case 'CHQF': return { label: 'Chequered Flag', color: '#7a7a7a' }
    case 'LGOT': return { label: 'Lights Out',     color: '#37872D' }
    case 'SSTA': return { label: 'Session Start',  color: 'var(--accent)' }
    case 'SEND': return { label: 'Session End',    color: 'var(--accent)' }
    case 'OVTK': return null
    case 'SPTP': return null
    default:     return null
  }
}

const LogoAndTitle = memo(({ theme }: { theme: 'dark' | 'light' }) => {
  return (
    <div className="flex items-center gap-2 shrink-0">
      <img
        src={theme === 'dark' ? iconTransparent : iconTransparentLight}
        alt="F1 Logo"
        className="h-5 w-auto select-none pointer-events-none"
        draggable="false"
      />
      <span className="font-semibold text-sm">Track N Race</span>
    </div>
  )
})
LogoAndTitle.displayName = 'LogoAndTitle'

const SidebarNavItem = memo(({ icon: Icon, label, active, onClick }: {
  icon: React.ElementType
  label: string
  active: boolean
  onClick: () => void
}) => (
  <div className="relative group w-full">
    <button
      onClick={onClick}
      className={`w-full h-9 flex items-center justify-center rounded-md transition-all relative ${
        active
          ? 'text-[var(--text-secondary)]'
          : 'text-[var(--text-muted)] hover:text-[var(--text-secondary)] hover:bg-[var(--bg-hover)]'
      }`}
    >
      {active && (
        <span
          className="absolute left-0 top-1/2 -translate-y-1/2 w-0.5 h-5 rounded-r-full"
          style={{ background: 'var(--accent)' }}
        />
      )}
      <Icon size={15} />
    </button>
    <div className="absolute left-full ml-2.5 top-1/2 -translate-y-1/2 px-2.5 py-1 rounded-md text-[11px] font-mono whitespace-nowrap bg-[var(--bg-menu)] border border-[var(--border)] text-[var(--text-primary)] opacity-0 group-hover:opacity-100 pointer-events-none z-[60] transition-opacity duration-150 select-none shadow-xl">
      {label}
    </div>
  </div>
))
SidebarNavItem.displayName = 'SidebarNavItem'

const SidebarActionItem = memo(({ icon: Icon, label, active, disabled, onClick }: {
  icon: React.ElementType
  label: string
  active?: boolean
  disabled?: boolean
  onClick: () => void
}) => (
  <div className="relative group w-full">
    <button
      onClick={onClick}
      disabled={disabled}
      className={`w-full h-9 flex items-center justify-center rounded-md transition-all ${
        disabled
          ? 'text-[var(--text-inactive)] cursor-not-allowed'
          : 'text-[var(--text-muted)] hover:text-[var(--text-secondary)] hover:bg-[var(--bg-hover)]'
      }`}
      style={!disabled && active ? { background: 'color-mix(in srgb, var(--accent) 12%, transparent)', color: 'var(--accent)' } : undefined}
    >
      <Icon size={14} />
    </button>
    <div className="absolute left-full ml-2.5 top-1/2 -translate-y-1/2 px-2.5 py-1 rounded-md text-[11px] font-mono whitespace-nowrap bg-[var(--bg-menu)] border border-[var(--border)] text-[var(--text-primary)] opacity-0 group-hover:opacity-100 pointer-events-none z-[60] transition-opacity duration-150 select-none shadow-xl">
      {label}
    </div>
  </div>
))
SidebarActionItem.displayName = 'SidebarActionItem'

interface SidebarProps {
  tab: Tab
  setTab: (t: Tab) => void
  settingsOpen: boolean
  setSettingsOpen: (open: boolean) => void
  editOpen: boolean
  setEditOpen: React.Dispatch<React.SetStateAction<boolean>>
}

const Sidebar = memo(({ tab, setTab, settingsOpen, setSettingsOpen, editOpen, setEditOpen }: SidebarProps) => {
  const editable = tab === 'core' || tab === 'input' || tab === 'misc' || tab === 'power' || tab === 'tyres'
  return (
    <aside
      className="w-12 shrink-0 flex flex-col border-r border-[var(--border)] bg-[var(--bg-panel)]"
      style={{ WebkitAppRegion: 'no-drag' } as React.CSSProperties}
    >
      <nav className="flex-1 flex flex-col items-center gap-0.5 py-2 w-full px-1.5">
        {TAB_ORDER.map(tabKey => (
          <SidebarNavItem
            key={tabKey}
            icon={TAB_ICONS[tabKey]}
            label={TAB_LABELS[tabKey]}
            active={tab === tabKey}
            onClick={() => setTab(tabKey)}
          />
        ))}
      </nav>
      <div className="flex flex-col items-center gap-0.5 pb-2 pt-2 border-t border-[var(--border)] w-full px-1.5">
        <SidebarActionItem
          icon={Settings2}
          label="Settings"
          active={settingsOpen}
          onClick={() => setSettingsOpen(!settingsOpen)}
        />
        <SidebarActionItem
          icon={Pencil}
          label="Edit Layout"
          active={editOpen}
          disabled={!editable}
          onClick={() => editable && setEditOpen(v => !v)}
        />
        <SidebarActionItem
          icon={Radio}
          label="Switch to Recorder"
          onClick={() => window.windowControls.switchToRecorder()}
        />
      </div>
    </aside>
  )
})
Sidebar.displayName = 'Sidebar'

interface PlaybackControlProps {
  filename: string | undefined
  onClose: () => void
  onSelectFile: () => void
}

const PlaybackControl = memo(({ filename, onClose, onSelectFile }: PlaybackControlProps) => {
  if (filename) {
    return (
      <div
        className="flex items-center gap-1.5 shrink-0"
        style={{ WebkitAppRegion: 'no-drag' } as React.CSSProperties}
      >
        <span className="text-[11px] font-mono text-[var(--text-secondary)] max-w-[200px] truncate">
          {filename}
        </span>
        <button
          onClick={onClose}
          className="p-1 text-[var(--text-secondary)] hover:text-[#e10600] transition-colors"
        >
          <X size={14} />
        </button>
      </div>
    )
  }

  return (
    <button
      onClick={onSelectFile}
      title="Load Session Data File (.tnrd)"
      style={{ WebkitAppRegion: 'no-drag' } as React.CSSProperties}
      className="p-1.5 rounded text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors shrink-0"
    >
      <Upload size={14} />
    </button>
  )
})
PlaybackControl.displayName = 'PlaybackControl'

const SessionBadge = memo(
  ({
    sessionType,
    theme,
  }: {
    sessionType: number | undefined
    theme: 'dark' | 'light'
  }) => {
    if (sessionType !== undefined) {
      const accent = sessionAccent(sessionType, theme === 'dark')
      return (
        <span
          className="text-[10px] font-medium uppercase tracking-wide rounded px-2 py-0.5 select-none shrink-0"
          style={{ backgroundColor: accent + '22', color: accent }}
        >
          {SESSION_TYPES[sessionType] ?? 'Unknown'}
        </span>
      )
    }

    return (
      <span className="text-[10px] font-medium uppercase tracking-wide rounded px-2 py-0.5 select-none shrink-0 bg-[var(--bg-panel)] text-[var(--text-secondary)]">
        Offline
      </span>
    )
  }
)
SessionBadge.displayName = 'SessionBadge'

const SessionTimer = memo(({ sessionTime }: { sessionTime: number | undefined }) => {
  if (sessionTime === undefined) return null
  const s = sessionTime
  const formatted = `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`
  return (
    <div className="text-sm font-black tabular-nums text-[var(--text-primary)] shrink-0">
      {formatted}
    </div>
  )
})
SessionTimer.displayName = 'SessionTimer'

const CentreBanner = memo(({ activeBanner }: { activeBanner: BannerItem | null }) => {
  return (
    <div className="absolute inset-0 flex items-center justify-center pointer-events-none">
      {activeBanner && (
        <div className="flex items-center gap-2">
          <span
            className="text-xs font-black uppercase tracking-[0.2em]"
            style={{ color: activeBanner.color }}
          >
            {activeBanner.label}
          </span>
          {activeBanner.sub && (
            <>
              <span className="text-xs text-[var(--text-secondary)]">·</span>
              <span className="text-xs text-[var(--text-secondary)]">{activeBanner.sub}</span>
            </>
          )}
        </div>
      )}
    </div>
  )
})
CentreBanner.displayName = 'CentreBanner'

const TimeWindowSelector = memo(
  ({ seconds, setSeconds }: { seconds: number; setSeconds: (s: number) => void }) => {
    return (
      <div style={{ WebkitAppRegion: 'no-drag' }} className="w-20">
        <Select
          options={WINDOW_OPTIONS}
          value={WINDOW_OPTIONS.find((o) => o.value === seconds) ?? null}
          onChange={(opt) => opt && setSeconds(opt.value as number)}
          styles={selectStyles}
          components={selectComponents}
          isSearchable={false}
          menuPortalTarget={document.body}
        />
      </div>
    )
  }
)
TimeWindowSelector.displayName = 'TimeWindowSelector'

interface HeaderButtonsProps {
  settingsOpen: boolean
  setSettingsOpen: (open: boolean) => void
  editOpen: boolean
  setEditOpen: React.Dispatch<React.SetStateAction<boolean>>
  tab: Tab
}

const HeaderButtons = memo(
  ({ settingsOpen, setSettingsOpen, editOpen, setEditOpen, tab }: HeaderButtonsProps) => {
    const editable = tab === 'core' || tab === 'input' || tab === 'misc' || tab === 'power' || tab === 'tyres'
    return (
      <>
        {/* Settings button */}
        <button
          onClick={() => setSettingsOpen(true)}
          title="Settings"
          style={{ WebkitAppRegion: 'no-drag' }}
          className={`p-1.5 rounded transition-colors ${
            settingsOpen
              ? 'bg-[var(--border-focus)] text-white'
              : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'
          }`}
        >
          <Settings2 size={13} />
        </button>

        {/* Edit button — always visible, disabled on non-editable tabs */}
        <button
          onClick={() => editable && setEditOpen((v) => !v)}
          title="Edit layout"
          style={{ WebkitAppRegion: 'no-drag' }}
          className={`p-1.5 rounded transition-colors ${
            !editable
              ? 'text-[var(--text-inactive)] cursor-not-allowed'
              : editOpen
                ? 'bg-[var(--border-focus)] text-white'
                : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)]'
          }`}
        >
          <Pencil size={13} />
        </button>

        {/* Switch to Recorder button */}
        <button
          onClick={() => window.windowControls.switchToRecorder()}
          title="Switch to Background Recorder"
          style={{ WebkitAppRegion: 'no-drag' }}
          className="p-1.5 rounded transition-colors text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--border)] shrink-0"
        >
          <Radio size={13} />
        </button>
      </>
    )
  }
)
HeaderButtons.displayName = 'HeaderButtons'

interface WindowControlsProps {
  isFullscreen: boolean
  isMaximized: boolean
  showOnlyFullscreen?: boolean
}

const WindowControls = memo(({ isFullscreen, isMaximized, showOnlyFullscreen }: WindowControlsProps) => {
  return (
    <div
      className="flex self-stretch ml-2 -mr-4 shrink-0"
      style={{ WebkitAppRegion: 'no-drag' }}
    >
      <button
        onClick={() => window.windowControls.fullscreen()}
        title={isFullscreen ? 'Exit Fullscreen' : 'Fullscreen'}
        className="h-full px-4 text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors flex items-center justify-center"
      >
        {isFullscreen ? (
          <Shrink size={13} />
        ) : (
          <svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1.2">
            <polyline points="0,3 0,0 3,0"/>
            <polyline points="7,0 10,0 10,3"/>
            <polyline points="10,7 10,10 7,10"/>
            <polyline points="3,10 0,10 0,7"/>
          </svg>
        )}
      </button>

      {!showOnlyFullscreen && (
        <>
          <button
            onClick={() => window.windowControls.minimize()}
            title="Minimize"
            className="h-full px-4 text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors flex items-center justify-center"
          >
            <svg width="10" height="10" viewBox="0 0 10 10" fill="currentColor"><rect y="4.5" width="10" height="1"/></svg>
          </button>

          <button
            onClick={() => window.windowControls.maximize()}
            title={isMaximized ? 'Restore' : 'Maximize'}
            className="h-full px-4 text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors flex items-center justify-center"
          >
            {isMaximized ? (
              <svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1">
                <polyline points="3,0.5 9.5,0.5 9.5,7"/>
                <rect x="0.5" y="3" width="6.5" height="6.5"/>
              </svg>
            ) : (
              <svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1">
                <rect x="0.5" y="0.5" width="9" height="9"/>
              </svg>
            )}
          </button>

          <button
            onClick={() => window.windowControls.close()}
            title="Close"
            className="h-full px-4 text-[var(--text-secondary)] hover:text-white hover:bg-[#e10600] transition-colors flex items-center justify-center"
          >
            <svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1">
              <line x1="0" y1="0" x2="10" y2="10"/><line x1="10" y1="0" x2="0" y2="10"/>
            </svg>
          </button>
        </>
      )}
    </div>
  )
})
WindowControls.displayName = 'WindowControls'

interface PlaybackControlsBarProps {
  isPlaying: boolean
  speed: number
  onSeekBackward: () => void
  onTogglePlay: () => void
  onSeekForward: () => void
  selectStyles: any
}

const PlaybackControlsBar = memo(function PlaybackControlsBar({
  isPlaying,
  speed,
  onSeekBackward,
  onTogglePlay,
  onSeekForward,
  selectStyles
}: PlaybackControlsBarProps) {
  return (
    <div className="flex items-center gap-3">
      <button
        onClick={onSeekBackward}
        className="w-8 h-8 flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors"
      >
        <ChevronLeft size={18} />
      </button>
      <button
        onClick={onTogglePlay}
        className="w-10 h-10 flex items-center justify-center rounded-full bg-[var(--border-focus)] text-white hover:bg-[var(--border-focus-hover)] transition-colors shadow-lg shadow-[var(--border-focus)]/20"
      >
        {isPlaying ? <Pause size={18} fill="currentColor" /> : <Play size={18} fill="currentColor" className="ml-0.5" />}
      </button>
      <button
        onClick={onSeekForward}
        className="w-8 h-8 flex items-center justify-center rounded-full text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors"
      >
        <ChevronRight size={18} />
      </button>
      <div className="w-[4.5rem]">
        <Select
          value={{ value: speed, label: `${speed}x` }}
          onChange={(option) => {
            if (option) window.playerBridge.setSpeed(option.value)
          }}
          options={[
            { value: 0.25, label: '0.25x' },
            { value: 0.5, label: '0.5x' },
            { value: 1, label: '1x' },
            { value: 2, label: '2x' },
            { value: 4, label: '4x' },
          ]}
          styles={selectStyles}
          components={selectComponents}
          menuPlacement="top"
          isSearchable={false}
        />
      </div>
    </div>
  )
})
PlaybackControlsBar.displayName = 'PlaybackControlsBar'

interface PlaybackProgressTrackerProps {
  currentTime: number
  progressPct: number
  totalTime: number
}

const PlaybackProgressTracker = memo(function PlaybackProgressTracker({
  currentTime,
  progressPct,
  totalTime
}: PlaybackProgressTrackerProps) {
  const [isDragging, setIsDragging] = useState(false)
  const [dragProgress, setDragProgress] = useState(0)

  const startSessionTime = currentTime - (progressPct * totalTime)
  const progressToUse = isDragging ? dragProgress : progressPct
  const displayTime = isDragging
    ? (startSessionTime + dragProgress * totalTime)
    : currentTime

  const handleMouseDown = useCallback(() => {
    setIsDragging(true)
    setDragProgress(progressPct)
  }, [progressPct])

  const handleMouseUp = useCallback(() => {
    setIsDragging(false)
  }, [])

  const handleTouchStart = useCallback(() => {
    setIsDragging(true)
    setDragProgress(progressPct)
  }, [progressPct])

  const handleTouchEnd = useCallback(() => {
    setIsDragging(false)
  }, [])

  const handleChange = useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    const val = parseFloat(e.target.value)
    setDragProgress(val)
    window.playerBridge.seek(val)
  }, [])

  return (
    <div className="flex-1 flex items-center gap-4">
      <span className="text-xs font-mono text-[var(--text-secondary)] tabular-nums">{fmtLap(displayTime)}</span>
      <input
        type="range"
        min="0"
        max="1"
        step="0.001"
        value={progressToUse}
        onMouseDown={handleMouseDown}
        onMouseUp={handleMouseUp}
        onTouchStart={handleTouchStart}
        onTouchEnd={handleTouchEnd}
        onChange={handleChange}
        className="flex-1 h-1.5 bg-[var(--border)] rounded-full appearance-none outline-none cursor-pointer [&::-webkit-slider-thumb]:appearance-none [&::-webkit-slider-thumb]:w-3 [&::-webkit-slider-thumb]:h-3 [&::-webkit-slider-thumb]:rounded-full [&::-webkit-slider-thumb]:bg-[var(--accent)] [&::-webkit-slider-thumb]:cursor-pointer"
        style={{
          background: `linear-gradient(to right, var(--accent) ${progressToUse * 100}%, var(--border) ${progressToUse * 100}%)`
        }}
      />
      <span className="text-xs font-mono text-[var(--text-secondary)] tabular-nums">{fmtLap(totalTime)}</span>
    </div>
  )
})
PlaybackProgressTracker.displayName = 'PlaybackProgressTracker'

interface PlaybackLapSelectorProps {
  speedRpmBlocks: any[]
  totalTime: number
  sessionFileStart: number
  currentLapNum: number | null
  selectStyles: any
}

const PlaybackLapSelector = memo(function PlaybackLapSelector({
  speedRpmBlocks, totalTime, sessionFileStart, currentLapNum, selectStyles
}: PlaybackLapSelectorProps) {
  const options = useMemo(
    () => speedRpmBlocks.map(b => ({ value: b.lapNum, label: String(b.lapNum) })),
    [speedRpmBlocks]
  )

  const value = useMemo(
    () => currentLapNum !== null ? { value: currentLapNum, label: String(currentLapNum) } : null,
    [currentLapNum]
  )

  const handleChange = useCallback((opt: SingleValue<{ value: number; label: string }>) => {
    if (!opt || totalTime <= 0) return
    const block = speedRpmBlocks.find(b => b.lapNum === opt.value)
    if (!block) return
    const ratio = (block.startSessionTime - sessionFileStart) / totalTime
    window.playerBridge.seek(Math.max(0, Math.min(1, ratio)))
  }, [speedRpmBlocks, sessionFileStart, totalTime])

  return (
    <div className="w-[4.5rem] shrink-0">
      <Select
        value={value}
        options={options}
        onChange={handleChange}
        isSearchable={false}
        maxMenuHeight={150}
        menuPlacement="top"
        styles={selectStyles}
        components={selectComponents}
        placeholder="—"
      />
    </div>
  )
})
PlaybackLapSelector.displayName = 'PlaybackLapSelector'

export default function App() {
  const [actualNativeTitlebar] = useState(() => window.electronStore.get('nativeTitlebar', false) as boolean)
  const [theme, setTheme] = useAppConfig<'dark' | 'light'>('theme', 'dark')
  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme)
  }, [theme])



  const [seconds, setSeconds] = useAppConfig<number>('timeWindow', 30)
  const [mapTimeout, setMapTimeout] = useAppConfig<number>('mapTimeout', 10)
  const [tab, setTab] = useState<Tab>('core')
  const [settingsOpen, setSettingsOpen] = useState(false)
  const [selectedIdx, setSelectedIdx] = useState<number | null>(null)
  const [tyreView, setTyreView] = useAppConfig<'cards' | 'graphs'>('tyreView', 'cards')
  const [tyreWearMode, setTyreWearMode] = useAppConfig<'wear' | 'life'>('tyreWearMode', 'life')
  const [scBanner, setScBanner] = useState<BannerItem | null>(null)
  const [transientBanner, setTransientBanner] = useState<BannerItem | null>(null)
  const [speedRpmMode, setSpeedRpmMode] = useState<'default' | 'CL' | 'PL' | 'FL' | 'compare'>('default')
  const [rawCoreLayout, setCoreLayout] = useAppConfig<CoreLayout>('coreLayout', DEFAULT_CORE_LAYOUT)
  const coreLayout = useMemo((): CoreLayout => ({
    ...DEFAULT_CORE_LAYOUT,
    ...rawCoreLayout,
    statsCards:    { ...DEFAULT_CORE_LAYOUT.statsCards,    ...(rawCoreLayout?.statsCards    ?? {}) },
    thermalGraphs: { ...DEFAULT_CORE_LAYOUT.thermalGraphs, ...(rawCoreLayout?.thermalGraphs ?? {}) },
    thermalCards:  { ...DEFAULT_CORE_LAYOUT.thermalCards,  ...(rawCoreLayout?.thermalCards  ?? {}) },
    damageItems:   { ...DEFAULT_CORE_LAYOUT.damageItems,   ...(rawCoreLayout?.damageItems   ?? {}) },
  }), [rawCoreLayout])
  const [inputLayout, setInputLayout] = useAppConfig<InputLayout>('inputLayout', DEFAULT_INPUT_LAYOUT)
  const [miscLayout, setMiscLayout] = useAppConfig<MiscLayout>('miscLayout', DEFAULT_MISC_LAYOUT)
  const [rawPowerLayout, setPowerLayout] = useAppConfig<PowerLayout>('powerLayout', DEFAULT_POWER_LAYOUT)
  const powerLayout = useMemo((): PowerLayout => ({
    statsCards: { ...DEFAULT_POWER_LAYOUT.statsCards, ...(rawPowerLayout?.statsCards ?? {}) },
    charts:     { ...DEFAULT_POWER_LAYOUT.charts,     ...(rawPowerLayout?.charts     ?? {}) },
  }), [rawPowerLayout])
  const [rawTyresLayout, setTyresLayout] = useAppConfig<TyresLayout>('tyresLayout', DEFAULT_TYRES_LAYOUT)
  const tyresLayout = useMemo((): TyresLayout => ({
    charts: { ...DEFAULT_TYRES_LAYOUT.charts, ...(rawTyresLayout?.charts ?? {}) },
  }), [rawTyresLayout])
  const [bannerDuration, setBannerDuration] = useAppConfig<number>('bannerDuration', 3)
  const [sectorColors, setSectorColors] = useAppConfig<boolean>('sectorColors', false)
  const [nativeTitlebar, setNativeTitlebar] = useAppConfig<boolean>('nativeTitlebar', false)
  const [driversMode, setDriversMode]   = useAppConfig<'dots' | 'both' | 'labels'>('driversMode', (() => {
    const legacy = window.electronStore.get('showLabels', null) as boolean | null
    if (legacy === true) return 'both'
    if (legacy === false) return 'dots'
    return 'both'
  })())
  const [editOpen, setEditOpen] = useState(false)
  const [isMaximized, setIsMaximized] = useState(false)
  const [isFullscreen, setIsFullscreen] = useState(false)
  const [headerVisible, setHeaderVisible] = useState(false)
  const [playbackState, setPlaybackState] = useState<any>(null)
  const playbackStateRef = useRef<any>(null)
  const sessionFileStartRef = useRef(0)
  const capturedForBlocksRef = useRef<any[] | null>(null)
  const [currentPlaybackLapNum, setCurrentPlaybackLapNum] = useState<number | null>(null)
  const currentPlaybackLapNumRef = useRef<number | null>(null)
  const speedRpmBlocksRef = useRef<any[] | null>(null)
  const [confirmOpenFilePath, setConfirmOpenFilePath] = useState<string | null>(null)

  useEffect(() => {
    return window.playerBridge.onRequestOpenConfirm((filePath) => {
      setConfirmOpenFilePath(filePath)
    })
  }, [])

  useEffect(() => window.windowControls.onMaximizeChange(setIsMaximized), [])
  useEffect(() => window.windowControls.onFullscreenChange(setIsFullscreen), [])
  useEffect(() => { if (!isFullscreen) setHeaderVisible(false) }, [isFullscreen])
  useEffect(() => {
    return window.playerBridge.onStateChange((st) => {
      playbackStateRef.current = st
      setPlaybackState(st)
      const blocks = speedRpmBlocksRef.current
      if (blocks) {
        const lapNum = blocks.find(b => st.currentTime >= b.startSessionTime && st.currentTime <= b.endSessionTime)?.lapNum ?? null
        if (lapNum !== currentPlaybackLapNumRef.current) {
          currentPlaybackLapNumRef.current = lapNum
          setCurrentPlaybackLapNum(lapNum)
        }
      }
    })
  }, [])

  const handleSeekBackward = useCallback(() => {
    const st = playbackStateRef.current
    if (st) {
      window.playerBridge.seek(Math.max(0, st.currentTime - 5) / st.totalTime)
    }
  }, [])

  const handleSeekForward = useCallback(() => {
    const st = playbackStateRef.current
    if (st) {
      window.playerBridge.seek(Math.min(st.totalTime, st.currentTime + 5) / st.totalTime)
    }
  }, [])

  const handleTogglePlay = useCallback(() => {
    const st = playbackStateRef.current
    if (st) {
      if (st.isPlaying) {
        window.playerBridge.pause()
      } else {
        window.playerBridge.play()
      }
    }
  }, [])

  const handleCloseSettings = useCallback(() => {
    setSettingsOpen(false)
  }, [])

  const { telemetry, motion, motionEx, status, statusHistory, damage, damageHistory, lap, timing, participants, allStatus, fastestLapCarIdx, raceEvent, raceEvents, session, tyreSets, latest, lapTelemetry, lapStatusHistory, lapHistory, fastestLap, speedRpmBlocks, isConnected, error, protocolStatus, protocolWarning } = useTelemetry(seconds)

  speedRpmBlocksRef.current = speedRpmBlocks

  // Capture session file start once when blocks first arrive — stable for the session
  if (speedRpmBlocks !== capturedForBlocksRef.current) {
    capturedForBlocksRef.current = speedRpmBlocks
    if (speedRpmBlocks && playbackStateRef.current) {
      const st = playbackStateRef.current
      sessionFileStartRef.current = st.currentTime - st.progressPct * st.totalTime
    }
  }

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

  // Transient event queue
  const tQueueRef    = useRef<BannerItem[]>([])
  const tShowingRef  = useRef(false)
  const tTimerRef    = useRef<ReturnType<typeof setTimeout> | null>(null)
  const participantsRef = useRef(participants)
  participantsRef.current = participants
  const bannerDurationRef = useRef(bannerDuration)
  bannerDurationRef.current = bannerDuration

  // Stable dequeue function via ref (avoids stale closures)
  const dequeueRef = useRef<() => void>(() => {})
  dequeueRef.current = () => {
    if (tQueueRef.current.length === 0) {
      setTransientBanner(null)
      tShowingRef.current = false
      return
    }
    const next = tQueueRef.current.shift()!
    setTransientBanner(next)
    tShowingRef.current = true
    tTimerRef.current = setTimeout(() => dequeueRef.current(), bannerDurationRef.current * 1000)
  }

  // Race leader tracking
  const p1IdxRef = useRef<number | null>(null)
  useEffect(() => {
    if (!timing) return
    const leader = timing.cars.find(c => c.position === 1 && c.result_status === 2)
    if (!leader) return
    if (p1IdxRef.current === null) {
      p1IdxRef.current = leader.idx
      return
    }
    if (leader.idx !== p1IdxRef.current) {
      p1IdxRef.current = leader.idx
      const item: BannerItem = {
        label: 'New Race Leader',
        sub: lastName(participantsRef.current, leader.idx),
        color: 'var(--accent)',
      }
      tQueueRef.current.push(item)
      if (!tShowingRef.current) dequeueRef.current()
    }
  }, [timing])

  useEffect(() => {
    if (!raceEvent) return
    const item = buildBanner(raceEvent, participantsRef.current)
    if (!item) return
    tQueueRef.current.push(item)
    if (!tShowingRef.current) dequeueRef.current()
  }, [raceEvent])

  useEffect(() => () => { if (tTimerRef.current) clearTimeout(tTimerRef.current) }, [])

  // Safety Car / VSC / Formation Lap — driven by session packet
  useEffect(() => {
    if (!session) return
    const sc = session.safety_car_status
    if (sc === 0) { setScBanner(null); return }
    const labels: Record<number, string> = {
      1: 'SAFETY CAR', 2: 'VIRTUAL SAFETY CAR', 3: 'FORMATION LAP',
    }
    const colors: Record<number, string> = { 1: '#ffd700', 2: '#ffb347', 3: '#ffd700' }
    setScBanner({ label: labels[sc] ?? 'SAFETY CAR', color: colors[sc] ?? '#ffd700' })
  }, [session])

  const protocolWarningBanner = useMemo(() => {
    if (!protocolWarning) return null
    return {
      label: 'PROTOCOL MISMATCH DETECTED',
      sub: `Receiving ${protocolWarning.detected_format} packets - override is set to ${protocolWarning.forced_format}`,
      color: '#ff4646'
    }
  }, [protocolWarning])

  // Protocol warning takes highest priority, followed by transient events, followed by SC/VSC/FL
  const activeBanner = useMemo(() => {
    return protocolWarningBanner ?? transientBanner ?? scBanner
  }, [protocolWarningBanner, transientBanner, scBanner])
  const handleClosePlayback = useCallback(() => {
    setSelectedIdx(null)
    window.playerBridge.close()
  }, [])

  const handleSelectPlaybackFile = useCallback(async () => {
    const file = await window.fsBridge.selectTNRDFile()
    if (file) window.playerBridge.load(file)
  }, [])

  const handleSelectDriver = useCallback((idx: number) => {
    setSelectedIdx(prev => prev === idx ? null : idx)
  }, [])

  const selectedCar        = timing?.cars.find(c => c.idx === selectedIdx) ?? null
  const playerDriver       = participants?.drivers.find(d => d.idx === (timing?.player_idx ?? -1)) ?? null
  const selectedDriver     = participants?.drivers.find(d => d.idx === selectedIdx) ?? playerDriver
  const selectedCarStatus  = allStatus?.cars.find(c => c.idx === selectedIdx) ?? null

  return (
    <div className="h-dvh bg-[var(--bg-base)] text-[var(--text-primary)] flex flex-col relative">

      {/* Full-width header — floats above everything in fullscreen */}
      <div
        className={isFullscreen ? 'absolute top-0 left-0 right-0 z-50 h-10' : ''}
        onMouseEnter={() => { if (isFullscreen) setHeaderVisible(true) }}
        onMouseLeave={() => { if (isFullscreen) setHeaderVisible(false) }}
      >
        <header
          className={`relative flex items-center gap-3 px-4 h-10 ${
            isFullscreen
              ? `transition-all duration-150 ${headerVisible ? 'opacity-100 translate-y-0' : 'opacity-0 -translate-y-full'}`
              : 'sticky top-0 z-10 transition-colors duration-500'
          }`}
          style={activeBanner
            ? { background: `${activeBanner.color}18`, borderColor: `${activeBanner.color}50`, WebkitAppRegion: actualNativeTitlebar ? 'no-drag' : 'drag' }
            : { background: 'var(--bg-panel)', borderColor: 'var(--border)', WebkitAppRegion: actualNativeTitlebar ? 'no-drag' : 'drag' }
          }
        >
          <img
            src={theme === 'dark' ? iconTransparent : iconTransparentLight}
            alt=""
            className="h-5 w-auto select-none pointer-events-none shrink-0"
            draggable="false"
          />

          <PlaybackControl
            filename={playbackState?.filename}
            onClose={handleClosePlayback}
            onSelectFile={handleSelectPlaybackFile}
          />

          <SessionBadge sessionType={session?.session_type} theme={theme} />

          <div className="flex-1" />

          <SessionTimer sessionTime={latest?.session_time} />

          <CentreBanner activeBanner={activeBanner} />

          <TimeWindowSelector seconds={seconds} setSeconds={setSeconds} />

          <WindowControls isFullscreen={isFullscreen} isMaximized={isMaximized} showOnlyFullscreen={actualNativeTitlebar} />
        </header>
      </div>

      {/* Body: sidebar + content */}
      <div className="flex-1 flex min-h-0 relative">

        {/* Dynamic Floating Event Banner for Fullscreen Mode */}
        {isFullscreen && !headerVisible && activeBanner && (
          <div className="absolute top-4 left-1/2 -translate-x-1/2 z-40 pointer-events-none">
            <div
              className="flex items-center gap-3 px-4 py-1.5 rounded-full border shadow-lg backdrop-blur-md animate-banner-in text-xs"
              style={{
                background: `rgba(7, 8, 16, 0.88)`,
                backgroundImage: `linear-gradient(rgba(255,255,255,0.02), rgba(255,255,255,0))`,
                borderColor: `${activeBanner.color}50`,
                boxShadow: `0 4px 20px -2px ${activeBanner.color}15, 0 2px 8px -1px rgba(0,0,0,0.5)`,
              }}
            >
              <div
                className="w-1.5 h-1.5 rounded-full shrink-0 animate-pulse"
                style={{ backgroundColor: activeBanner.color }}
              />
              <span
                className="font-black uppercase tracking-[0.2em]"
                style={{ color: activeBanner.color }}
              >
                {activeBanner.label}
              </span>
              {activeBanner.sub && (
                <>
                  <span className="text-[var(--text-secondary)]">·</span>
                  <span className="text-[var(--text-secondary)] font-semibold">{activeBanner.sub}</span>
                </>
              )}
            </div>
          </div>
        )}

        {/* Slim icon-only sidebar */}
        <Sidebar
          tab={tab}
          setTab={setTab}
          settingsOpen={settingsOpen}
          setSettingsOpen={setSettingsOpen}
          editOpen={editOpen}
          setEditOpen={setEditOpen}
        />

        {/* Content column */}
        <div className="flex-1 flex flex-col min-w-0">

      {/* Analyzing Session Modal */}
      {playbackState?.isScanning && (
        <div className="fixed inset-0 z-[100] flex flex-col items-center justify-center bg-[var(--bg-modal)] backdrop-blur-sm">
          <div className="text-xl font-bold text-[var(--text-primary)] mb-4 tracking-widest uppercase text-sm">Analyzing Session Data</div>
          <div className="w-64 h-1.5 bg-[var(--border)] rounded-full overflow-hidden relative">
            <div className="absolute inset-y-0 left-0 w-1/2 rounded-full animate-bounce" style={{ animation: 'scan 1.5s infinite linear', background: 'var(--accent)' }} />
          </div>
          <style>{`
            @keyframes scan {
              0% { left: -50%; }
              100% { left: 100%; }
            }
          `}</style>
        </div>
      )}

      {/* Edit modal — centered overlay */}
      {editOpen && (tab === 'core' || tab === 'input' || tab === 'misc' || tab === 'power' || tab === 'tyres') && (
        <div
          className="fixed inset-0 z-50 flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
          onClick={() => setEditOpen(false)}
        >
          <div
            className="bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[920px] max-h-[90vh] flex flex-col overflow-hidden"
            onClick={e => e.stopPropagation()}
          >
            {/* Header */}
            <div className="flex items-center justify-between px-6 py-4 border-b border-[var(--border)] shrink-0">
              <div>
                <div className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest">
                  {tab === 'input' ? 'Edit Input Layout' : tab === 'misc' ? 'Edit Misc Layout' : tab === 'power' ? 'Edit Power Layout' : tab === 'tyres' ? 'Edit Tyres Layout' : 'Edit Overview Layout'}
                </div>
                <div className="text-[10px] font-mono text-[var(--text-secondary)] mt-1 uppercase tracking-wider">Toggle sections to show or hide</div>
              </div>
              <button
                onClick={() => setEditOpen(false)}
                className="w-8 h-8 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#e10600] transition-colors"
              >
                <X size={14} />
              </button>
            </div>

            {/* Body */}
            <div className="overflow-y-auto p-6 flex flex-col gap-6">
              {tab === 'tyres' ? (<>

                <div className="flex flex-col gap-2">
                  <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Charts (Graph View)</div>
                  <div className="w-full grid grid-cols-2 rounded-none overflow-hidden border border-[var(--border)] bg-[var(--border)] gap-[1px]">
                    {([
                      { key: 'surfaceTemp', label: 'Surface Temp' },
                      { key: 'innerTemp',   label: 'Inner Temp'   },
                      { key: 'brakeTemp',   label: 'Brake Temp'   },
                      { key: 'tyreLife',    label: tyreWearMode === 'life' ? 'Tyre Life' : 'Tyre Wear' },
                    ] as { key: keyof TyresLayout['charts']; label: string }[]).map(({ key, label }) => {
                      const on = tyresLayout.charts[key]
                      return (
                        <button
                          key={key}
                          onClick={() => setTyresLayout({ ...tyresLayout, charts: { ...tyresLayout.charts, [key]: !on } })}
                          className={`h-32 flex flex-col items-center justify-center rounded-none font-mono text-xs font-semibold transition-all relative ${
                            on
                              ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                              : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                          }`}
                        >
                          <span className="font-bold">{label}</span>
                          <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${on ? 'text-[var(--accent)]' : 'text-[var(--text-muted)]'}`}>
                            {on ? 'ACTIVE' : 'HIDDEN'}
                          </span>
                        </button>
                      )
                    })}
                  </div>
                </div>

              </>) : tab === 'power' ? (<>

                <div className="flex flex-col gap-6">
                  {/* Stats Bar Preview */}
                  <div className="flex flex-col gap-2">
                    <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Stats Bar</div>
                    <div className="w-full flex rounded-none overflow-hidden border border-[var(--border)] divide-x divide-[var(--border)] bg-[var(--bg-input)]">
                      {([
                        { key: 'totalPower', label: 'Total Power' },
                        { key: 'ice',        label: 'ICE'         },
                        { key: 'mguk',       label: 'MGU-K'       },
                        { key: 'split',      label: 'Split'       },
                        { key: 'ersStore',   label: 'ERS Store'   },
                        { key: 'ersPct',     label: 'ERS %'       },
                        { key: 'fuel',       label: 'Fuel'        },
                      ] as { key: keyof PowerLayout['statsCards']; label: string }[]).map(({ key, label }) => {
                        const on = powerLayout.statsCards[key]
                        return (
                          <button
                            key={key}
                            onClick={() => setPowerLayout({ ...powerLayout, statsCards: { ...powerLayout.statsCards, [key]: !on } })}
                            className={`flex-1 py-6 flex flex-col items-center justify-center rounded-none font-mono text-[11px] font-semibold transition-all relative ${
                              on
                                ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                                : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                            }`}
                          >
                            <span className="font-bold">{label}</span>
                            <span className={`text-[8px] mt-1 tracking-wider uppercase font-bold opacity-60 ${on ? 'text-[var(--accent)]' : 'text-[var(--text-muted)]'}`}>
                              {on ? 'ON' : 'OFF'}
                            </span>
                          </button>
                        )
                      })}
                    </div>
                  </div>

                  {/* Charts Breakdown Preview */}
                  <div className="flex flex-col gap-2">
                    <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Charts</div>
                    <div className="w-full flex rounded-none overflow-hidden border border-[var(--border)] divide-x divide-[var(--border)] bg-[var(--bg-input)]">
                      {([
                        { key: 'powerSplit',  label: 'Power Split'  },
                        { key: 'ersHarvest',  label: 'ERS Harvest'  },
                        { key: 'ersStore',    label: 'ERS Store'    },
                        { key: 'fuelHistory', label: 'Fuel History' },
                      ] as { key: keyof PowerLayout['charts']; label: string }[]).map(({ key, label }) => {
                        const on = powerLayout.charts[key]
                        return (
                          <button
                            key={key}
                            onClick={() => setPowerLayout({ ...powerLayout, charts: { ...powerLayout.charts, [key]: !on } })}
                            className={`flex-1 h-44 flex flex-col items-center justify-center rounded-none font-mono text-xs font-semibold transition-all relative ${
                              on
                                ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                                : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                            }`}
                          >
                            <span className="font-bold">{label}</span>
                            <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${on ? 'text-[var(--accent)]' : 'text-[var(--text-muted)]'}`}>
                              {on ? 'ACTIVE' : 'HIDDEN'}
                            </span>
                          </button>
                        )
                      })}
                    </div>
                  </div>
                </div>

              </>) : tab === 'misc' ? (<>

                <div className="flex flex-col gap-2">
                  <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Panels</div>
                  <div className="w-full flex flex-col rounded-none overflow-hidden border border-[var(--border)] divide-y divide-[var(--border)] bg-[var(--bg-input)]">
                    <button
                      onClick={() => setMiscLayout({ ...miscLayout, showGForce: !miscLayout.showGForce })}
                      className={`h-44 w-full flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                        miscLayout.showGForce
                          ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                          : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                      }`}
                    >
                      <span className="text-sm font-bold">G-Force Chart</span>
                      <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${miscLayout.showGForce ? 'text-[var(--accent)]' : 'text-[var(--text-muted)]'}`}>
                        {miscLayout.showGForce ? 'ACTIVE' : 'HIDDEN'}
                      </span>
                    </button>
                    <button
                      onClick={() => setMiscLayout({ ...miscLayout, showRideHeight: !miscLayout.showRideHeight })}
                      className={`h-44 w-full flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                        miscLayout.showRideHeight
                          ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                          : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                      }`}
                    >
                      <span className="text-sm font-bold">Ride Height Chart</span>
                      <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${miscLayout.showRideHeight ? 'text-[var(--accent)]' : 'text-[var(--text-muted)]'}`}>
                        {miscLayout.showRideHeight ? 'ACTIVE' : 'HIDDEN'}
                      </span>
                    </button>
                  </div>
                </div>

              </>) : tab === 'input' ? (<>

                <div className="flex flex-col gap-2">
                  <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Panels</div>
                  <div className="w-full flex flex-col rounded-none overflow-hidden border border-[var(--border)] divide-y divide-[var(--border)] bg-[var(--bg-input)]">
                    {/* Top Row: Gear and Throttle/Brake Chart */}
                    <div className="w-full flex divide-x divide-[var(--border)]">
                      <button
                        onClick={() => setInputLayout({ ...inputLayout, showGear: !inputLayout.showGear })}
                        className={`flex-1 h-48 flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                          inputLayout.showGear
                            ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                            : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                        }`}
                      >
                        <span className="text-sm font-bold">Gear Indicator</span>
                        <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${inputLayout.showGear ? 'text-[var(--accent)]' : 'text-[var(--text-muted)]'}`}>
                          {inputLayout.showGear ? 'ACTIVE' : 'HIDDEN'}
                        </span>
                      </button>
                      <button
                        onClick={() => setInputLayout({ ...inputLayout, showInputs: !inputLayout.showInputs })}
                        className={`flex-1 h-48 flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                          inputLayout.showInputs
                            ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                            : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                        }`}
                      >
                        <span className="text-sm font-bold">Throttle / Brake Chart</span>
                        <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${inputLayout.showInputs ? 'text-[var(--accent)]' : 'text-[var(--text-muted)]'}`}>
                          {inputLayout.showInputs ? 'ACTIVE' : 'HIDDEN'}
                        </span>
                      </button>
                    </div>
                    {/* Bottom Row: Steering full width */}
                    <button
                      onClick={() => setInputLayout({ ...inputLayout, showSteering: !inputLayout.showSteering })}
                      className={`h-28 w-full flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                        inputLayout.showSteering
                          ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                          : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                      }`}
                    >
                      <span className="text-sm font-bold">Steering Telemetry</span>
                      <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${inputLayout.showSteering ? 'text-[var(--accent)]' : 'text-[var(--text-muted)]'}`}>
                        {inputLayout.showSteering ? 'ACTIVE' : 'HIDDEN'}
                      </span>
                    </button>
                  </div>
                </div>

              </>) : (<>

                <div className="flex flex-col gap-6">
                  {/* Cohesive Layout Preview Box */}
                  <div className="flex flex-col gap-2">
                    <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Screen Layout Schematic</div>
                    <div className="w-full flex flex-col rounded-none overflow-hidden border border-[var(--border)] bg-[var(--bg-input)] divide-y divide-[var(--border)]">
                      
                      {/* 1. Stats Bar Row */}
                      <div className="w-full flex divide-x divide-[var(--border)]">
                        {([
                          { key: 'speed',    label: 'Speed'    },
                          { key: 'rpm',      label: 'RPM'      },
                          { key: 'gear',     label: 'Gear'     },
                          { key: 'throttle', label: 'Throt'    },
                          { key: 'brake',    label: 'Brake'    },
                          { key: 'drs',      label: 'DRS'      },
                          { key: 'engine',   label: 'Eng'      },
                          { key: 'ers',      label: 'ERS'      },
                          { key: 'fuel',     label: 'Fuel'     },
                          { key: 'pos',      label: 'Pos'      },
                          { key: 'tyre',     label: 'Tyre'     },
                        ] as { key: keyof CoreLayout['statsCards']; label: string }[]).map(({ key, label }) => {
                          const on = coreLayout.statsCards[key]
                          return (
                            <button
                              key={key}
                              onClick={() => setCoreLayout({ ...coreLayout, statsCards: { ...coreLayout.statsCards, [key]: !on } })}
                              className={`flex-1 py-5 flex flex-col items-center justify-center rounded-none font-mono text-[10px] font-bold transition-all relative ${
                                on
                                  ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                                  : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                              }`}
                            >
                              <span>{label}</span>
                            </button>
                          )
                        })}
                      </div>

                      {/* 2. Main Chart Row */}
                      <button
                        onClick={() => setCoreLayout({ ...coreLayout, showSpeedChart: !coreLayout.showSpeedChart })}
                        className={`h-44 w-full flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                          coreLayout.showSpeedChart
                            ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                            : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                        }`}
                      >
                        <span className="text-sm font-bold uppercase tracking-wider">Speed + RPM + ERS Chart</span>
                        <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${coreLayout.showSpeedChart ? 'text-[var(--accent)]' : 'text-[var(--text-muted)]'}`}>
                          {coreLayout.showSpeedChart ? 'VISIBLE' : 'HIDDEN'}
                        </span>
                      </button>

                      {/* 3. Tyre/Thermal Section Row */}
                      {tyreView === 'graphs' ? (
                        <div className="w-full flex divide-x divide-[var(--border)]">
                          {([
                            { key: 'surfaceTemp', label: 'Surf Temp' },
                            { key: 'innerTemp',   label: 'Inner Temp'   },
                            { key: 'brakeTemp',   label: 'Brake Temp'   },
                            { key: 'tyreLife',    label: tyreWearMode === 'life' ? 'Tyre Life' : 'Tyre Wear' },
                          ] as { key: keyof CoreLayout['thermalGraphs']; label: string }[]).map(({ key, label }) => {
                            const on = coreLayout.thermalGraphs[key]
                            return (
                              <button
                                key={key}
                                onClick={() => setCoreLayout({ ...coreLayout, thermalGraphs: { ...coreLayout.thermalGraphs, [key]: !on } })}
                                className={`flex-1 py-7 flex flex-col items-center justify-center rounded-none font-mono text-xs font-semibold transition-all relative ${
                                  on
                                    ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                                    : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                                }`}
                              >
                                <span className="font-bold">{label}</span>
                              </button>
                            )
                          })}
                        </div>
                      ) : (
                        <div className="w-full flex divide-x divide-[var(--border)]">
                          {([
                            { key: 'fl', label: 'Front Left'  },
                            { key: 'fr', label: 'Front Right' },
                            { key: 'rl', label: 'Rear Left'   },
                            { key: 'rr', label: 'Rear Right'  },
                          ] as { key: keyof CoreLayout['thermalCards']; label: string }[]).map(({ key, label }) => {
                            const on = coreLayout.thermalCards[key]
                            return (
                              <button
                                key={key}
                                onClick={() => setCoreLayout({ ...coreLayout, thermalCards: { ...coreLayout.thermalCards, [key]: !on } })}
                                className={`flex-1 py-7 flex flex-col items-center justify-center rounded-none font-mono text-xs font-semibold transition-all relative ${
                                  on
                                    ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                                    : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                                }`}
                              >
                                <span className="font-bold">{label}</span>
                              </button>
                            )
                          })}
                        </div>
                      )}

                      {/* 4. Damage Row 1 (Tyres & Brakes) */}
                      <div className="w-full flex divide-x divide-[var(--border)]">
                        {([
                          { key: 'tyreDmgFl',  label: 'Tyre FL'   },
                          { key: 'brakeDmgFl', label: 'Brake FL'  },
                          { key: 'tyreDmgFr',  label: 'Tyre FR'   },
                          { key: 'brakeDmgFr', label: 'Brake FR'  },
                          { key: 'tyreDmgRl',  label: 'Tyre RL'   },
                          { key: 'brakeDmgRl', label: 'Brake RL'  },
                          { key: 'tyreDmgRr',  label: 'Tyre RR'   },
                          { key: 'brakeDmgRr', label: 'Brake RR'  },
                        ] as { key: keyof CoreLayout['damageItems']; label: string }[]).map(({ key, label }) => {
                          const on = coreLayout.damageItems[key]
                          return (
                            <button
                              key={key}
                              onClick={() => setCoreLayout({ ...coreLayout, damageItems: { ...coreLayout.damageItems, [key]: !on } })}
                              className={`flex-1 py-4 flex flex-col items-center justify-center rounded-none font-mono text-[10px] font-semibold transition-all relative ${
                                on
                                  ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                                  : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                              }`}
                            >
                              <span>{label}</span>
                            </button>
                          )
                        })}
                      </div>

                      {/* 5. Damage Row 2 (Aero & Body) */}
                      <div className="w-full flex divide-x divide-[var(--border)]">
                        {([
                          { key: 'wingFl',     label: 'Wing FL'   },
                          { key: 'wingFr',     label: 'Wing FR'   },
                          { key: 'wingRear',   label: 'Rear Wing' },
                          { key: 'floor',      label: 'Floor'     },
                          { key: 'diffuser',   label: 'Diffuser'  },
                          { key: 'sidepod',    label: 'Sidepod'   },
                          { key: 'gearbox',    label: 'Gearbox'   },
                          { key: 'engine',     label: 'Engine'    },
                        ] as { key: keyof CoreLayout['damageItems']; label: string }[]).map(({ key, label }) => {
                          const on = coreLayout.damageItems[key]
                          return (
                            <button
                              key={key}
                              onClick={() => setCoreLayout({ ...coreLayout, damageItems: { ...coreLayout.damageItems, [key]: !on } })}
                              className={`flex-1 py-4 flex flex-col items-center justify-center rounded-none font-mono text-[10px] font-semibold transition-all relative ${
                                on
                                  ? 'bg-[var(--bg-widget-active)] text-[var(--accent)]'
                                  : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                              }`}
                            >
                              <span>{label}</span>
                            </button>
                          )
                        })}
                      </div>

                    </div>
                  </div>
                </div>

              </>)}
            </div>
          </div>
        </div>
      )}

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
        />
      )}

      {/* Content */}
      <main className="flex-1 min-h-0">
        {tab === 'core' && (() => {
          const visibleDamageCount = Object.values(coreLayout.damageItems).filter(Boolean).length
          const damageTwoRow = visibleDamageCount > 8
          return (
          <div className="h-full flex flex-col overflow-hidden">
            <div className="flex-1 min-h-0 flex flex-col bg-[var(--bg-panel)] border-t border-[var(--border)] overflow-hidden divide-y divide-[var(--border)]">
              {coreLayout.showStats && (
                <div className="shrink-0">
                  <LiveStats latest={latest} status={status} lap={lap} damage={damage} isConnected={isConnected} visibleCards={coreLayout.statsCards} isDark={theme === 'dark'} />
                </div>
              )}
              {coreLayout.showSpeedChart && (
                <div className={`${damageTwoRow ? 'flex-[8]' : 'flex-[13]'} min-h-0`}>
                  <SpeedRpmChart data={telemetry} statusHistory={statusHistory} lapData={lapTelemetry} lapStatusHistory={lapStatusHistory} lapHistory={lapHistory} fastestLap={fastestLap} speedRpmBlocks={speedRpmBlocks} mode={speedRpmMode} onModeChange={setSpeedRpmMode} isDark={theme === 'dark'} />
                </div>
              )}
              {coreLayout.showThermal && (
                <div className={`${damageTwoRow ? 'flex-[4]' : 'flex-[7]'} min-h-0`}>
                  <ThermalPanel latest={latest} damage={damage} telemetry={telemetry} damageHistory={damageHistory} view={tyreView} tyreWearMode={tyreWearMode} thermalGraphs={coreLayout.thermalGraphs} thermalCards={coreLayout.thermalCards} isDark={theme === 'dark'} />
                </div>
              )}
              {visibleDamageCount > 0 && (
                <div className="shrink-0">
                  <DamagePanel connected={!!latest} damage={damage} visibleItems={coreLayout.damageItems} twoRow={damageTwoRow} isDark={theme === 'dark'} />
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
                  onSelectDriver={handleSelectDriver}
                  isDark={theme === 'dark'}
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
                  isDark={theme === 'dark'}
                />
              </div>
            </div>
          </div>
        )}
        {tab === 'session' && (
          <div className="h-full overflow-hidden bg-[var(--bg-panel)] border-t border-[var(--border)]">
            <SessionPanel session={session} raceEvents={raceEvents} timing={timing} participants={participants} isDark={theme === 'dark'} sectorColors={sectorColors} driversMode={driversMode} mapTimeout={mapTimeout} />
          </div>
        )}
        {tab === 'input' && (
          <div className="h-full flex flex-col overflow-hidden">
            <div className="flex-1 min-h-0 flex flex-col bg-[var(--bg-panel)] border-t border-[var(--border)] overflow-hidden divide-y divide-[var(--border)]">
              {(inputLayout.showGear || inputLayout.showInputs) && (
                <div className="flex-1 min-h-0 flex divide-x divide-[var(--border)]">
                  {inputLayout.showGear && (
                    <div className="flex-1 min-w-0 min-h-0">
                      <GearChart data={telemetry} isDark={theme === 'dark'} />
                    </div>
                  )}
                  {inputLayout.showInputs && (
                    <div className="flex-1 min-w-0 min-h-0">
                      <InputsChart data={telemetry} isDark={theme === 'dark'} />
                    </div>
                  )}
                </div>
              )}
              {inputLayout.showSteering && (
                <div className="flex-1 min-h-0">
                  <SteeringChart data={telemetry} isDark={theme === 'dark'} />
                </div>
              )}
            </div>
          </div>
        )}

        {tab === 'misc' && (
          <div className="h-full flex flex-col overflow-hidden">
            <div className="flex-1 min-h-0 flex flex-col bg-[var(--bg-panel)] border-t border-[var(--border)] overflow-hidden divide-y divide-[var(--border)]">
              {miscLayout.showGForce && (
                <div className="flex-1 min-h-0">
                  <GForceChart data={motion} isDark={theme === 'dark'} />
                </div>
              )}
              {miscLayout.showRideHeight && (
                <div className="flex-1 min-h-0">
                  <RideHeightChart data={motionEx} isDark={theme === 'dark'} />
                </div>
              )}
            </div>
          </div>
        )}
        {tab === 'power' && (
          <div className="h-full flex flex-col overflow-hidden border-t border-[var(--border)] divide-y divide-[var(--border)]">
            <div className="shrink-0 bg-[var(--bg-panel)]">
              <PowerStatsBar status={status} visibleCards={powerLayout.statsCards} isDark={theme === 'dark'} />
            </div>
            <div className="flex-1 min-h-0">
              <PowerBreakdownChart data={statusHistory} isDark={theme === 'dark'} visibleCharts={powerLayout.charts} />
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
              isDark={theme === 'dark'}
              visibleGraphs={tyresLayout.charts}
              sessionType={session?.session_type ?? null}
            />
          </div>
        )}
      </main>

      {/* Playback Controls Bar */}
      {playbackState && playbackState.filename && (
        <div className="h-14 border-t border-[var(--border)] bg-[var(--bg-panel)] shrink-0 flex items-center px-6 gap-6 z-40 select-none">
          <PlaybackControlsBar
            isPlaying={playbackState.isPlaying}
            speed={playbackState.speed}
            onSeekBackward={handleSeekBackward}
            onTogglePlay={handleTogglePlay}
            onSeekForward={handleSeekForward}
            selectStyles={selectStyles}
          />
          {speedRpmBlocks && speedRpmBlocks.length > 0 && (
            <PlaybackLapSelector
              speedRpmBlocks={speedRpmBlocks}
              totalTime={playbackState.totalTime}
              sessionFileStart={sessionFileStartRef.current}
              currentLapNum={currentPlaybackLapNum}
              selectStyles={selectStyles}
            />
          )}
          <PlaybackProgressTracker
            currentTime={playbackState.currentTime}
            progressPct={playbackState.progressPct}
            totalTime={playbackState.totalTime}
          />
        </div>
      )}

      {confirmOpenFilePath && (
        <div className="fixed inset-0 z-[100] flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]">
          <div className="bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[480px] flex flex-col overflow-hidden animate-[eventFadeIn_0.2s_ease-out]">
            {/* Header */}
            <div className="flex items-center justify-between px-6 py-4 border-b border-[var(--border)] shrink-0 select-none">
              <div>
                <div className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest flex items-center gap-2">
                  <AlertTriangle size={14} className="text-amber-500" />
                  <span>Open Session File</span>
                </div>
              </div>
              <button
                onClick={() => setConfirmOpenFilePath(null)}
                className="w-8 h-8 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#e10600] transition-colors"
              >
                <X size={14} />
              </button>
            </div>

            {/* Body */}
            <div className="p-6 flex flex-col gap-4">
              <div className="flex flex-col gap-2">
                <p className="text-sm font-semibold text-[var(--text-primary)]">
                  Are you sure you want to open this file?
                </p>
                <p className="text-xs text-[var(--text-secondary)] leading-relaxed">
                  Opening it will stop the event bridge and if you have an session right now with the game, it will be closed.
                </p>
              </div>
              
              {confirmOpenFilePath && (
                <div className="p-3 rounded-lg bg-[var(--bg-card)]/30 border border-[var(--border)] flex items-center gap-3 select-none">
                  <span className="text-[10px] font-mono text-[var(--text-muted)] uppercase tracking-wider shrink-0">
                    Selected file:
                  </span>
                  <span className="text-xs font-mono text-[var(--text-secondary)] truncate flex-1 font-semibold">
                    {confirmOpenFilePath.split(/[\\/]/).pop()}
                  </span>
                </div>
              )}
            </div>

            {/* Footer / Actions */}
            <div className="flex items-center justify-end gap-3 px-6 py-4 border-t border-[var(--border)] bg-[var(--bg-card)]/10 shrink-0">
              <button
                onClick={() => setConfirmOpenFilePath(null)}
                className="px-4 h-8 rounded-lg text-xs font-semibold font-mono border border-[var(--border)] text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-all active:scale-95 cursor-pointer outline-none"
              >
                No
              </button>
              <button
                onClick={() => {
                  window.playerBridge.load(confirmOpenFilePath)
                  setConfirmOpenFilePath(null)
                }}
                className="px-4 h-8 rounded-lg text-xs font-semibold font-mono bg-[var(--border-focus)] text-white hover:bg-[var(--border-focus-hover)] shadow-sm transition-all active:scale-95 cursor-pointer outline-none"
              >
                Yes
              </button>
            </div>
          </div>
        </div>
      )}

        </div> {/* content column */}
      </div> {/* body row */}
    </div>
  )
}
