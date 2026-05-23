import { useState } from 'react'
import { Clock, Network, Sun, Map, AlertTriangle, Radio, X, Info } from 'lucide-react'
import type { ProtocolStatusMsg, ProtocolWarningMsg } from '../types'
import iconTransparent from '../assets/icon_transparent.png'
import iconTransparentLight from '../assets/icon_transparent_light.png'

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
  protocolStatus: ProtocolStatusMsg | null
  protocolWarning: ProtocolWarningMsg | null
}

type Option<T> = { value: T; label: string }

function SegmentedControl<T extends string | number | boolean>({
  options, value, onChange,
}: {
  options: Option<T>[]
  value: T
  onChange: (v: T) => void
}) {
  return (
    <div className="flex rounded-lg bg-[var(--bg-input)] border border-[var(--border-muted)] p-0.5 gap-0.5">
      {options.map((opt) => (
        <button
          key={String(opt.value)}
          onClick={() => onChange(opt.value)}
          className={`px-3 h-7 rounded-md text-xs font-medium whitespace-nowrap transition-all duration-150 ${
            opt.value === value
              ? 'bg-[var(--border-focus)] text-white shadow-sm'
              : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-white/5 active:scale-95'
          }`}
        >
          {opt.label}
        </button>
      ))}
    </div>
  )
}

function Toggle({ value, onChange }: { value: boolean; onChange: (v: boolean) => void }) {
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
}

function Row({ label, description, warning, children }: {
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
}

type RestartStatus = 'idle' | 'applying' | 'ok' | 'error'

export default function Settings({
  isOpen, onClose,
  tyreView, onTyreViewChange,
  tyreWearMode, onTyreWearModeChange,
  bannerDuration, onBannerDurationChange,
  theme, onThemeChange,
  sectorColors, onSectorColorsChange,
  driversMode, onDriversModeChange,
  mapTimeout, onMapTimeoutChange,
  protocolStatus, protocolWarning,
}: Props) {
  const [activeCategory, setActiveCategory] = useState<'appearance' | 'notifications' | 'map' | 'network' | 'protocol'>('appearance')
  const [showAbout, setShowAbout] = useState(false)
  
  const [port, setPort]         = useState<number>(() => window.electronStore.get('udp.port', 20777) as number)
  const [addr, setAddr]         = useState<string>(() => window.electronStore.get('udp.bindAddress', '0.0.0.0') as string)
  const [udpStatus, setUdpStatus] = useState<RestartStatus>('idle')
  const [errorMsg, setErrorMsg]   = useState('')
  const [protocolOverride, setProtocolOverride] = useState<'auto' | 'f1_24' | 'f1_25'>(
    () => (window.electronStore.get('udp.protocol', 'auto') as 'auto' | 'f1_24' | 'f1_25')
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

  function handleProtocolOverrideChange(v: 'auto' | 'f1_24' | 'f1_25') {
    setProtocolOverride(v)
    window.protocolBridge.setOverride(v)
  }

  function formatDetectedGame(status: ProtocolStatusMsg | null): string {
    if (!status) return 'No data yet'
    const { detected_format, active_format, override } = status
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
  }

  const inputCls = 'bg-[var(--bg-input)] border border-[var(--border-muted)] rounded-lg text-xs text-[var(--text-primary)] px-3 h-8 outline-none focus:border-[var(--border-focus)] transition-colors w-full tabular-nums'

  const CATEGORIES = [
    { id: 'appearance' as const, label: 'Appearance', icon: <Sun size={14} />, color: '#f59e0b' },
    { id: 'notifications' as const, label: 'Notifications', icon: <Clock size={14} />, color: '#8b5cf6' },
    { id: 'map' as const, label: 'Map', icon: <Map size={14} />, color: '#10b981' },
    { id: 'network' as const, label: 'Network', icon: <Network size={14} />, color: '#0ea5e9' },
    { id: 'protocol' as const, label: 'Protocol', icon: <Radio size={14} />, color: '#e879f9' },
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
    </div>
  )

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
          {formatDetectedGame(protocolStatus)}
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
          ]}
          value={protocolOverride}
          onChange={handleProtocolOverrideChange}
        />
      </Row>
      
      {/* Seamless Protocol Warning Banner */}
      {protocolWarning && (
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
              Receiving {protocolWarning.detected_format} packets — override is set to {protocolWarning.forced_format}
            </p>
          </div>
        </div>
      )}
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

  function renderCategoryContent() {
    switch (activeCategory) {
      case 'appearance':
        return renderAppearance()
      case 'notifications':
        return renderNotifications()
      case 'map':
        return renderMap()
      case 'network':
        return renderNetwork()
      case 'protocol':
        return renderProtocol()
    }
  }

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
      onClick={onClose}
    >
      <div
        className="bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[960px] h-[650px] max-h-[85vh] flex flex-col overflow-hidden"
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
                    setShowAbout(false)
                  }}
                  className={`w-full flex items-center gap-3 px-3 py-2.5 rounded-lg text-xs font-semibold font-mono transition-all text-left ${
                    active && !showAbout
                      ? 'bg-[var(--border-focus)] text-white shadow-sm'
                      : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]'
                  }`}
                >
                  <span style={{ color: cat.color }}>{cat.icon}</span>
                  <span>{cat.label}</span>
                </button>
              )
            })}

            {/* Flex spacer to push About button to bottom */}
            <div className="flex-1" />

            {/* About Button */}
            <button
              onClick={() => setShowAbout(true)}
              className={`w-full flex items-center gap-3 px-3 py-2.5 rounded-lg text-xs font-semibold font-mono transition-all text-left ${
                showAbout
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
            showAbout
              ? 'items-center justify-center'
              : 'items-start justify-start'
          }`}>
            <div className="w-full max-w-[640px]">
              {showAbout ? renderAbout() : renderCategoryContent()}
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
