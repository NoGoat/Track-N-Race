import { useState, memo } from 'react'
import { Clock, Network, Sun, Map, AlertTriangle, Radio, X, Info, HardDrive, ScrollText, ChevronDown, ExternalLink, LineChart, Shrink } from 'lucide-react'
import type { ProtocolStatusMsg, ProtocolWarningMsg } from '../types'
import {
  GRAPH_GROUPS, ALL_GRAPH_SECTIONS, COMPACT_GROUPS, ALL_COMPACT_BOOL_KEYS, TYRE_LEVEL_OPTIONS,
  type GraphViewState, type GraphView, type CompactState,
} from '../lib/graphSections'
import iconTransparent from '../assets/icon_transparent.png'
import iconTransparentLight from '../assets/icon_transparent_light.png'
import { ATTRIBUTIONS, ATTRIBUTION_SECTIONS } from '../data/attributions'

interface Props {
  isOpen: boolean
  onClose: () => void
  tyreView: 'cards' | 'graphs'
  onTyreViewChange: (v: 'cards' | 'graphs') => void
  tyreWearMode: 'wear' | 'life'
  onTyreWearModeChange: (v: 'wear' | 'life') => void
  bannerDuration: number
  onBannerDurationChange: (v: number) => void
  theme: 'dark' | 'light'
  onThemeChange: (v: 'dark' | 'light') => void
  sectorColors: boolean
  onSectorColorsChange: (v: boolean) => void
  driversMode: 'dots' | 'both' | 'labels'
  onDriversModeChange: (v: 'dots' | 'both' | 'labels') => void
  mapTimeout: number
  onMapTimeoutChange: (v: number) => void
  detectedGameLabel: string
  detectedWarningFormat: number | null
  forcedWarningFormat: number | null
  nativeTitlebar: boolean
  onNativeTitlebarChange: (v: boolean) => void
  reduceAnimations: boolean
  onReduceAnimationsChange: (v: boolean) => void
  mapDimmed: boolean
  onMapDimmedChange: (v: boolean) => void
  graphView: GraphViewState
  onGraphViewChange: (v: GraphViewState) => void
  compact: CompactState
  onCompactChange: (v: CompactState) => void
}

type Option<T> = { value: T; label: string }

const SegmentedControl = memo(function SegmentedControl<T extends string | number | boolean>({
  options, value, onChange,
}: {
  options: Option<T>[]
  value: T
  onChange: (v: T) => void
}) {
  return (
    <div className="flex gap-1">
      {options.map((opt) => (
        <button
          key={String(opt.value)}
          onClick={() => onChange(opt.value)}
          className={`px-3 py-1 text-xs font-medium whitespace-nowrap transition-colors border-b-2 ${
            opt.value === value
              ? 'border-[var(--border-focus)] text-[var(--text-primary)]'
              : 'border-transparent text-[var(--text-secondary)] hover:text-[var(--text-primary)]'
          }`}
        >
          {opt.label}
        </button>
      ))}
    </div>
  )
}) as <T extends string | number | boolean>(props: { options: Option<T>[]; value: T; onChange: (v: T) => void }) => React.ReactElement

const Toggle = memo(function Toggle({ value, onChange }: { value: boolean; onChange: (v: boolean) => void }) {
  return (
    <button
      role="switch"
      aria-checked={value}
      onClick={() => onChange(!value)}
      className={`relative inline-flex w-10 h-[22px] rounded-full transition-colors duration-200 focus:outline-none active:scale-95 ${
        value ? 'bg-[var(--border-focus)]' : 'bg-[var(--border-muted)]'
      }`}
    >
      <span className={`absolute top-[3px] w-4 h-4 rounded-full bg-white shadow-sm transition-transform duration-200 ${
        value ? 'translate-x-[22px]' : 'translate-x-[3px]'
      }`} />
    </button>
  )
})

const Row = memo(function Row({ label, description, warning, children }: {
  label: string
  description: React.ReactNode
  warning?: string
  children: React.ReactNode
}) {
  return (
    <div className="flex items-center justify-between gap-8 px-4 py-3.5 hover:bg-[var(--bg-hover)]/30 rounded-xl transition-all duration-150">
      <div className="flex-1 min-w-0">
        <p className="text-sm font-semibold text-[var(--text-primary)] leading-none">{label}</p>
        <p className="text-xs text-[var(--text-muted)] mt-1.5 leading-relaxed">{description}</p>
        {warning && (
          <div className="flex items-center gap-1.5 mt-2">
            <AlertTriangle size={10} color="#f59e0b" className="shrink-0" />
            <span className="text-[10px] text-amber-500/80">{warning}</span>
          </div>
        )}
      </div>
      <div className="shrink-0">{children}</div>
    </div>
  )
})

type RestartStatus = 'idle' | 'applying' | 'ok' | 'error'

const Settings = memo(function Settings({
  isOpen, onClose,
  tyreView, onTyreViewChange,
  tyreWearMode, onTyreWearModeChange,
  bannerDuration, onBannerDurationChange,
  theme, onThemeChange,
  sectorColors, onSectorColorsChange,
  driversMode, onDriversModeChange,
  mapTimeout, onMapTimeoutChange,
  detectedGameLabel,
  detectedWarningFormat,
  forcedWarningFormat,
  nativeTitlebar,
  onNativeTitlebarChange,
  reduceAnimations,
  onReduceAnimationsChange,
  mapDimmed,
  onMapDimmedChange,
  graphView,
  onGraphViewChange,
  compact,
  onCompactChange,
}: Props) {
  const [activeCategory, setActiveCategory] = useState<'appearance' | 'graphs' | 'compact' | 'notifications' | 'map' | 'network' | 'protocol' | 'storage'>('appearance')
  const [view, setView] = useState<'category' | 'about' | 'attributions'>('category')
  const [expandedLicense, setExpandedLicense] = useState<string | null>(null)
  
  const [loggingEnabled, setLoggingEnabled] = useState<boolean>(() => window.electronStore.get('logging.enabled', false) as boolean)
  const [loggingDirectory, setLoggingDirectory] = useState<string>(() => window.electronStore.get('logging.directory', '') as string)
  
  const [port, setPort]         = useState<number>(() => window.electronStore.get('udp.port', 20777) as number)
  const [addr, setAddr]         = useState<string>(() => window.electronStore.get('udp.bindAddress', '0.0.0.0') as string)
  const [udpStatus, setUdpStatus] = useState<RestartStatus>('idle')
  const [errorMsg, setErrorMsg]   = useState('')
  const [protocolOverride, setProtocolOverride] = useState<'auto' | 'f1_24' | 'f1_25' | 'f1_26'>(
    () => (window.electronStore.get('udp.protocol', 'auto') as 'auto' | 'f1_24' | 'f1_25' | 'f1_26')
  )

  if (!isOpen) return null

  const portValid = Number.isInteger(port) && port >= 1 && port <= 65535
  const dirty     = port !== (window.electronStore.get('udp.port', 20777) as number)
                 || addr !== (window.electronStore.get('udp.bindAddress', '0.0.0.0') as string)

  async function applyUdp() {
    if (!portValid || udpStatus === 'applying') return
    setUdpStatus('applying')
    window.electronStore.set('udp.port', port)
    window.electronStore.set('udp.bindAddress', addr)
    const result = await window.udpBridge.restart()
    if (result.ok) {
      setUdpStatus('ok')
    } else {
      setErrorMsg(result.error ?? 'Unknown error')
      setUdpStatus('error')
    }
    setTimeout(() => setUdpStatus('idle'), 2500)
  }

  function handleProtocolOverrideChange(v: 'auto' | 'f1_24' | 'f1_25' | 'f1_26') {
    setProtocolOverride(v)
    window.protocolBridge.setOverride(v)
  }

  async function handleSelectDirectory() {
    const dir = await window.fsBridge.selectDirectory()
    if (dir) {
      setLoggingDirectory(dir)
      window.electronStore.set('logging.directory', dir)
    }
  }

  function handleLoggingToggle(val: boolean) {
    setLoggingEnabled(val)
    window.electronStore.set('logging.enabled', val)
  }



  const inputCls = 'bg-[var(--bg-input)] border border-[var(--border-muted)] rounded-lg text-xs text-[var(--text-primary)] px-3 h-8 outline-none focus:border-[var(--border-focus)] transition-colors w-full tabular-nums'

  const CATEGORIES = [
    { id: 'appearance' as const, label: 'Appearance', icon: <Sun size={14} />, color: '#f59e0b' },
    { id: 'graphs' as const, label: 'Graphs', icon: <LineChart size={14} />, color: '#0ea5e9' },
    { id: 'compact' as const, label: 'Compact', icon: <Shrink size={14} />, color: '#14b8a6' },
    { id: 'notifications' as const, label: 'Notifications', icon: <Clock size={14} />, color: '#8b5cf6' },
    { id: 'map' as const, label: 'Map', icon: <Map size={14} />, color: '#10b981' },
    { id: 'network' as const, label: 'Network', icon: <Network size={14} />, color: '#0ea5e9' },
    { id: 'protocol' as const, label: 'Protocol', icon: <Radio size={14} />, color: '#e879f9' },
    { id: 'storage' as const, label: 'Data Storage', icon: <HardDrive size={14} />, color: '#10b981' },
  ]

  const renderAppearance = () => (
    <div className="flex flex-col gap-1 animate-[eventFadeIn_0.2s_ease-out]">
      <Row label="Theme" description="Switch between dark and light interface">
        <SegmentedControl
          options={[{ value: 'dark' as const, label: 'Dark' }, { value: 'light' as const, label: 'Light' }]}
          value={theme} onChange={onThemeChange}
        />
      </Row>
      <Row label="Tyre View Mode" description="How tyre data is displayed in the Overview tab">
        <SegmentedControl
          options={[{ value: 'cards' as const, label: 'Cards' }, { value: 'graphs' as const, label: 'Graphs' }]}
          value={tyreView} onChange={onTyreViewChange}
        />
      </Row>
      <Row label="Tyre Wear Graph Mode" description="Whether graphs show remaining tyre life or accumulated wear">
        <SegmentedControl
          options={[{ value: 'life' as const, label: 'Tyre Life' }, { value: 'wear' as const, label: 'Tyre Wear' }]}
          value={tyreWearMode} onChange={onTyreWearModeChange}
        />
      </Row>
      <Row
        label="Native Titlebar"
        description="Enable the operating system's native titlebar and window frame borders instead of the custom frameless titlebar."
        warning="Requires restarting the application to apply."
      >
        <Toggle value={nativeTitlebar} onChange={onNativeTitlebarChange} />
      </Row>
      <Row
        label="Reduce Animations"
        description="Disable motion effects across the app, including position-swap transitions and flashes in the Standings table."
      >
        <Toggle value={reduceAnimations} onChange={onReduceAnimationsChange} />
      </Row>
    </div>
  )

  const GroupLabel = ({ children }: { children: React.ReactNode }) => (
    <div className="px-4 pt-4 pb-1 text-[10px] font-semibold uppercase tracking-widest text-[var(--text-muted)]">{children}</div>
  )

  const BulkButton = ({ label, onClick }: { label: string; onClick: () => void }) => (
    <button
      onClick={onClick}
      className="text-xs px-3 h-8 rounded-lg font-medium transition-all active:scale-95 bg-[var(--border-muted)] text-[var(--text-primary)] hover:bg-[var(--border-focus)] hover:text-white"
    >
      {label}
    </button>
  )

  const renderGraphs = () => {
    const anyChart = ALL_GRAPH_SECTIONS.some(k => graphView[k] === 'chart')
    const setAll = (v: GraphView) =>
      onGraphViewChange(Object.fromEntries(ALL_GRAPH_SECTIONS.map(k => [k, v])) as GraphViewState)
    return (
      <div className="flex flex-col gap-1 animate-[eventFadeIn_0.2s_ease-out]">
        <div className="flex items-center justify-between px-4 py-3">
          <p className="text-xs text-[var(--text-muted)]">Show each telemetry graph as its line chart or a raw-values table.</p>
          <BulkButton label={anyChart ? 'Set All Table' : 'Set All Chart'} onClick={() => setAll(anyChart ? 'table' : 'chart')} />
        </div>
        {GRAPH_GROUPS.map(group => (
          <div key={group.group}>
            <GroupLabel>{group.group}</GroupLabel>
            {group.sections.map(s => (
              <Row key={s.key} label={s.label} description="">
                <SegmentedControl
                  options={[{ value: 'chart' as const, label: 'Chart' }, { value: 'table' as const, label: 'Table' }]}
                  value={graphView[s.key]}
                  onChange={(v) => onGraphViewChange({ ...graphView, [s.key]: v })}
                />
              </Row>
            ))}
          </div>
        ))}
      </div>
    )
  }

  const renderCompact = () => {
    const anyCompact = ALL_COMPACT_BOOL_KEYS.some(k => compact[k]) || compact.overviewTyres > 0
    const setAll = (on: boolean) => {
      const next = { ...compact, overviewTyres: on ? 5 : 0 }
      for (const k of ALL_COMPACT_BOOL_KEYS) next[k] = on
      onCompactChange(next)
    }
    return (
      <div className="flex flex-col gap-1 animate-[eventFadeIn_0.2s_ease-out]">
        <div className="flex items-center justify-between px-4 py-3">
          <p className="text-xs text-[var(--text-muted)]">Collapse a section's cards to a denser single-line layout.</p>
          <BulkButton label={anyCompact ? 'Set All Normal' : 'Set All Compact'} onClick={() => setAll(!anyCompact)} />
        </div>
        {COMPACT_GROUPS.map(group => (
          <div key={group.group}>
            <GroupLabel>{group.group}</GroupLabel>
            {group.sections.map(s => (
              <Row key={s.key} label={s.label} description="">
                <SegmentedControl
                  options={[{ value: false, label: 'Normal' }, { value: true, label: 'Compact' }]}
                  value={compact[s.key]}
                  onChange={(v) => onCompactChange({ ...compact, [s.key]: v })}
                />
              </Row>
            ))}
            {group.group === 'Overview' && (
              <Row label="Tyre Cards" description="">
                <SegmentedControl
                  options={TYRE_LEVEL_OPTIONS}
                  value={compact.overviewTyres}
                  onChange={(v) => onCompactChange({ ...compact, overviewTyres: v })}
                />
              </Row>
            )}
          </div>
        ))}
      </div>
    )
  }

  const renderNotifications = () => (
    <div className="flex flex-col gap-1 animate-[eventFadeIn_0.2s_ease-out]">
      <Row label="Event Banner Duration" description="How long each race event notification is shown before the next one">
        <SegmentedControl
          options={[2, 3, 5, 8, 10].map(v => ({ value: v, label: `${v}s` }))}
          value={bannerDuration} onChange={onBannerDurationChange}
        />
      </Row>
    </div>
  )

  const renderMap = () => (
    <div className="flex flex-col gap-1 animate-[eventFadeIn_0.2s_ease-out]">
      <Row label="Map Opacity" description="Dims the track outline to 40% opacity so driver dots and labels stand out.">
        <Toggle value={mapDimmed} onChange={onMapDimmedChange} />
      </Row>
      <Row
        label="Sector Colors"
        description="Color each sector of the track individually on the map. Lines are drawn in white (dark mode) or black (light mode) when disabled."
        warning="Sector colors can make the map harder to read at a glance."
      >
        <Toggle value={sectorColors} onChange={onSectorColorsChange} />
      </Row>
      <Row label="Drivers" description="Configure how cars and driver codes are represented on the track map.">
        <SegmentedControl
          options={[
            { value: 'dots' as const, label: 'Dots' },
            { value: 'both' as const, label: 'Dots + Labels' },
            { value: 'labels' as const, label: 'Labels' }
          ]}
          value={driversMode} onChange={onDriversModeChange}
        />
      </Row>
      <Row label="Hide Static Drivers" description="Hide driver dots/labels if their position hasn't changed for more than the selected duration.">
        <SegmentedControl
          options={[
            { value: 0, label: 'Off' },
            { value: 3, label: '3s' },
            { value: 5, label: '5s' },
            { value: 10, label: '10s' },
            { value: 15, label: '15s' },
            { value: 30, label: '30s' }
          ]}
          value={mapTimeout} onChange={onMapTimeoutChange}
        />
      </Row>
    </div>
  )

  const renderNetwork = () => (
    <div className="flex flex-col gap-1 animate-[eventFadeIn_0.2s_ease-out]">
      <Row label="UDP Port" description="Port the game broadcasts telemetry to (2025 default: 20777)">
        <div className="w-28">
          <input
            type="number" min={1} max={65535} value={port}
            onChange={e => setPort(parseInt(e.target.value, 10))}
            className={`${inputCls} ${!portValid ? 'border-red-600/60' : ''}`}
          />
        </div>
      </Row>
      <Row
        label="Bind Address"
        description={<>Network interface to listen on — <span className="text-[var(--text-secondary)]">0.0.0.0</span> for all interfaces, <span className="text-[var(--text-secondary)]">127.0.0.1</span> for localhost only</>}
      >
        <div className="w-36">
          <input
            type="text" value={addr} placeholder="0.0.0.0"
            onChange={e => setAddr(e.target.value)}
            className={inputCls}
          />
        </div>
      </Row>
      
      {/* Seamless Action Bar */}
      <div className="flex items-center justify-between px-4 py-3.5 mt-2">
        <p className="text-xs">
          {udpStatus === 'ok'       && <span className="text-green-400 font-medium">Listener restarted successfully</span>}
          {udpStatus === 'error'    && <span className="text-red-400 font-medium">{errorMsg}</span>}
          {udpStatus === 'applying' && <span className="text-[var(--text-muted)] font-medium">Restarting…</span>}
          {udpStatus === 'idle'     && (dirty
            ? <span className="text-amber-400/80 font-medium">Unsaved changes</span>
            : <span className="text-[var(--text-muted)]">Restart the listener to apply changes</span>
          )}
        </p>
        <button
          onClick={applyUdp}
          disabled={!portValid || udpStatus === 'applying'}
          className={`text-xs px-4 h-7 rounded-lg font-medium transition-all active:scale-95 disabled:opacity-40 disabled:cursor-not-allowed disabled:active:scale-100 ${
            udpStatus === 'ok'    ? 'bg-green-700/30 text-green-400' :
            udpStatus === 'error' ? 'bg-red-900/30 text-red-400' :
                                   'bg-[var(--border-focus)] text-white hover:bg-[var(--border-focus-hover)]'
          }`}
        >
          {udpStatus === 'applying' ? 'Restarting…' : udpStatus === 'ok' ? 'Applied' : 'Apply & Restart'}
        </button>
      </div>
    </div>
  )

  const renderProtocol = () => (
    <div className="flex flex-col gap-1 animate-[eventFadeIn_0.2s_ease-out]">
      <Row
        label="Detected Protocol"
        description="The protocol version currently detected from incoming UDP packets"
      >
        <span className="text-xs font-semibold text-[var(--text-secondary)] tabular-nums">
          {detectedGameLabel}
        </span>
      </Row>
      <Row
        label="Protocol Version Override"
        description="Force a specific protocol version. Auto uses the packet format field to detect the correct parser automatically."
      >
        <SegmentedControl
          options={[
            { value: 'auto'  as const, label: 'Auto'  },
            { value: 'f1_24' as const, label: '2024' },
            { value: 'f1_25' as const, label: '2025' },
            { value: 'f1_26' as const, label: '2026' },
          ]}
          value={protocolOverride}
          onChange={handleProtocolOverrideChange}
        />
      </Row>
      
      {/* Seamless Protocol Warning Banner */}
      {detectedWarningFormat && forcedWarningFormat && (
        <div className="flex items-center gap-3 px-4 py-3.5 mt-4 rounded-xl bg-red-950/20 border border-red-500/20">
          <div className="relative shrink-0">
            <span className="absolute inset-0 rounded-full bg-red-500/30 animate-ping" />
            <AlertTriangle size={16} className="relative text-red-500 animate-pulse" />
          </div>
          <div>
            <p className="text-xs font-semibold text-red-400">
              Protocol mismatch detected
            </p>
            <p className="text-[11px] text-[var(--text-muted)] mt-0.5">
              Receiving {detectedWarningFormat} packets — override is set to {forcedWarningFormat}
            </p>
          </div>
        </div>
      )}
    </div>
  )

  const renderStorage = () => (
    <div className="flex flex-col gap-1 animate-[eventFadeIn_0.2s_ease-out]">
      <Row
        label="Record Session Data"
        description="Write live telemetry to disk in .tnrd format for later playback or analysis. This may consume disk space."
      >
        <Toggle value={loggingEnabled} onChange={handleLoggingToggle} />
      </Row>
      <Row
        label="Output Directory"
        description="Folder where .tnrd session files will be saved"
      >
        <div className="flex items-center gap-2">
          <input
            type="text"
            readOnly
            value={loggingDirectory}
            placeholder="Select a folder..."
            className="bg-[var(--bg-input)] border border-[var(--border-muted)] rounded-lg text-xs text-[var(--text-secondary)] px-3 h-8 outline-none w-48 tabular-nums truncate select-none opacity-80"
          />
          <button
            onClick={handleSelectDirectory}
            className="text-xs px-3 h-8 rounded-lg font-medium transition-all active:scale-95 bg-[var(--border-muted)] text-[var(--text-primary)] hover:bg-[var(--border-focus)] hover:text-white"
          >
            Browse
          </button>
        </div>
      </Row>
    </div>
  )

  const renderAbout = () => (
    <div className="flex flex-col items-center justify-center text-center py-12 animate-[eventFadeIn_0.2s_ease-out] w-full max-w-[640px] select-none mx-auto my-auto">
      {/* Logo */}
      <img
        src={theme === 'dark' ? iconTransparent : iconTransparentLight}
        alt="Track N Race Logo"
        className="h-24 w-auto mb-6 select-none pointer-events-none drop-shadow-[0_4px_12px_rgba(0,0,0,0.25)]"
        draggable="false"
      />
      
      {/* App Name */}
      <h1 className="text-xl font-bold font-mono tracking-wider text-[var(--text-primary)] uppercase">
        Track N Race
      </h1>
      
      {/* Version Pill */}
      <p className="text-[10px] font-mono text-[var(--text-secondary)] mt-2 font-bold uppercase tracking-wider bg-[var(--bg-input)] border border-[var(--border-muted)] px-3 py-1 rounded-full">
        {/* @ts-ignore */}
        {import.meta.env.DEV ? 'Version Next' : (typeof __APP_VERSION__ !== 'undefined' ? __APP_VERSION__ : '1.0.0')}
      </p>

      {/* Description Tagline */}
      <p className="text-xs text-[var(--text-secondary)] mt-4 font-mono leading-relaxed max-w-[340px]">
        A telemetry app for Formula One games.
      </p>

      {/* Creator Credits (Whitespace separated, no harsh lines) */}
      <div className="mt-16 flex flex-col items-center">
        <span className="text-[9px] font-mono text-[var(--text-muted)] uppercase tracking-[0.25em]">
          CREATED BY
        </span>
        <span className="text-xs font-semibold font-mono text-[var(--text-primary)] mt-1.5 tracking-wide">
          NoGoat
        </span>
      </div>
    </div>
  )

  const renderAttributions = () => (
    <div className="w-full animate-[eventFadeIn_0.2s_ease-out] select-none">
      <h2 className="text-xs font-bold font-mono uppercase tracking-widest text-[var(--text-primary)]">
        Attribution
      </h2>
      <p className="text-[10px] font-mono text-[var(--text-secondary)] mt-1 leading-relaxed">
        Track N Race is built with these open-source components. Thank you to their authors.
      </p>

      {ATTRIBUTION_SECTIONS.map(section => {
        const items = ATTRIBUTIONS.filter(a => a.category === section.category)
        if (items.length === 0) return null
        return (
          <div key={section.category} className="mt-6">
            <div className="text-[9px] font-mono text-[var(--text-muted)] uppercase tracking-[0.25em] mb-2">
              {section.label}
            </div>
            <div className="flex flex-col gap-2">
              {items.map(item => {
                const expanded = expandedLicense === item.name
                return (
                  <div
                    key={item.name}
                    className="border border-[var(--border)] rounded-lg bg-[var(--bg-card)]/40 overflow-hidden"
                  >
                    <button
                      onClick={() => setExpandedLicense(expanded ? null : item.name)}
                      className="w-full flex items-center gap-2 px-3 py-2.5 text-left hover:bg-[var(--bg-hover)] transition-colors"
                    >
                      <ChevronDown
                        size={13}
                        className={`shrink-0 text-[var(--text-muted)] transition-transform ${expanded ? 'rotate-180' : ''}`}
                      />
                      <span className="text-xs font-semibold font-mono text-[var(--text-primary)]">
                        {item.name}
                      </span>
                      {item.version && (
                        <span className="text-[9px] font-mono text-[var(--text-primary)] font-bold bg-[var(--border-muted)] px-1.5 py-0.5 rounded-full tabular-nums">
                          {item.version}
                        </span>
                      )}
                      <span className="text-[9px] font-mono text-[var(--text-primary)] font-bold uppercase tracking-wider bg-[var(--border-muted)] px-1.5 py-0.5 rounded">
                        {item.license}
                      </span>
                      <span className="flex-1" />
                      <a
                        href={item.homepage}
                        target="_blank"
                        rel="noopener noreferrer"
                        onClick={e => e.stopPropagation()}
                        className="shrink-0 text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors"
                        title={item.homepage}
                      >
                        <ExternalLink size={13} />
                      </a>
                    </button>
                    {expanded && (
                      <pre className="max-h-[240px] overflow-y-auto whitespace-pre-wrap break-words bg-[var(--bg-input)] border-t border-[var(--border)] p-3 text-[10px] leading-relaxed font-mono text-[var(--text-secondary)]">
                        {item.licenseText.trim()}
                      </pre>
                    )}
                  </div>
                )
              })}
            </div>
          </div>
        )
      })}
    </div>
  )

  function renderCategoryContent() {
    switch (activeCategory) {
      case 'appearance':
        return renderAppearance()
      case 'graphs':
        return renderGraphs()
      case 'compact':
        return renderCompact()
      case 'notifications':
        return renderNotifications()
      case 'map':
        return renderMap()
      case 'network':
        return renderNetwork()
      case 'protocol':
        return renderProtocol()
      case 'storage':
        return renderStorage()
    }
  }

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
      onClick={onClose}
    >
      <div
        className="bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[1080px] h-[650px] max-h-[85vh] flex flex-col overflow-hidden"
        onClick={e => e.stopPropagation()}
      >
        {/* Header */}
        <div className="flex items-center justify-between px-6 py-4 border-b border-[var(--border)] shrink-0">
          <div>
            <div className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest">
              Application Settings
            </div>
            <div className="text-[10px] font-mono text-[var(--text-secondary)] mt-1 uppercase tracking-wider">
              Configure telemetry and visual preferences
            </div>
          </div>
          <button
            onClick={onClose}
            className="w-8 h-8 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#e10600] transition-colors"
          >
            <X size={14} />
          </button>
        </div>

        {/* Body Container (Sidebar + Content) */}
        <div className="flex flex-1 min-h-0">
          {/* Sidebar */}
          <div className="w-[220px] shrink-0 border-r border-[var(--border)] bg-[var(--bg-card)]/30 p-4 flex flex-col gap-1.5 overflow-y-auto select-none h-full">
            {CATEGORIES.map(cat => {
              const active = activeCategory === cat.id
              return (
                <button
                  key={cat.id}
                  onClick={() => {
                    setActiveCategory(cat.id)
                    setView('category')
                  }}
                  className={`w-full flex items-center gap-3 px-3 py-2.5 rounded-lg text-xs font-semibold font-mono transition-all text-left ${
                    active && view === 'category'
                      ? 'bg-[var(--border-focus)] text-white shadow-sm'
                      : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]'
                  }`}
                >
                  <span style={{ color: cat.color }}>{cat.icon}</span>
                  <span>{cat.label}</span>
                </button>
              )
            })}

            {/* Flex spacer to push About/Attribution buttons to bottom */}
            <div className="flex-1" />

            {/* Attribution Button */}
            <button
              onClick={() => setView('attributions')}
              className={`w-full flex items-center gap-3 px-3 py-2.5 rounded-lg text-xs font-semibold font-mono transition-all text-left ${
                view === 'attributions'
                  ? 'bg-[var(--border-focus)] text-white shadow-sm'
                  : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]'
              }`}
            >
              <ScrollText size={14} style={{ color: '#a0a8b8' }} />
              <span>Attribution</span>
            </button>

            {/* About Button */}
            <button
              onClick={() => setView('about')}
              className={`w-full flex items-center gap-3 px-3 py-2.5 rounded-lg text-xs font-semibold font-mono transition-all text-left ${
                view === 'about'
                  ? 'bg-[var(--border-focus)] text-white shadow-sm'
                  : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]'
              }`}
            >
              <Info size={14} style={{ color: '#a0a8b8' }} />
              <span>About</span>
            </button>
          </div>

          {/* Content Area */}
          <div className={`flex-1 overflow-y-auto p-8 bg-[var(--bg-panel)] flex flex-col ${
            view === 'about'
              ? 'items-center justify-center'
              : 'items-start justify-start'
          }`}>
            <div className="w-full max-w-[858px]">
              {view === 'about'
                ? renderAbout()
                : view === 'attributions'
                  ? renderAttributions()
                  : renderCategoryContent()}
            </div>
          </div>
        </div>
      </div>
    </div>
  )
})

export default Settings
