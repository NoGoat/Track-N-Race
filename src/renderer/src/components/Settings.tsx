import { useState } from 'react'
import { Clock, Network, Sun, Map, AlertTriangle, Radio } from 'lucide-react'
import type { ProtocolStatusMsg, ProtocolWarningMsg } from '../types'

interface Props {
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

function SectionLabel({ icon, label }: { icon: React.ReactNode; label: string }) {
  return (
    <div className="flex items-center gap-2 mb-2 px-1">
      {icon}
      <span className="text-[10px] font-semibold uppercase tracking-[0.12em] text-[var(--text-secondary)]">{label}</span>
    </div>
  )
}

function Row({ label, description, warning, children }: {
  label: string
  description: React.ReactNode
  warning?: string
  children: React.ReactNode
}) {
  return (
    <div className="flex items-center gap-8 px-5 py-4">
      <div className="flex-1 min-w-0">
        <p className="text-sm font-medium text-[var(--text-primary)] leading-none">{label}</p>
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
  tyreView, onTyreViewChange,
  tyreWearMode, onTyreWearModeChange,
  bannerDuration, onBannerDurationChange,
  theme, onThemeChange,
  sectorColors, onSectorColorsChange,
  driversMode, onDriversModeChange,
  mapTimeout, onMapTimeoutChange,
  protocolStatus, protocolWarning,
}: Props) {
  const [port, setPort]         = useState<number>(() => window.electronStore.get('udp.port', 20777) as number)
  const [addr, setAddr]         = useState<string>(() => window.electronStore.get('udp.bindAddress', '0.0.0.0') as string)
  const [udpStatus, setUdpStatus] = useState<RestartStatus>('idle')
  const [errorMsg, setErrorMsg]   = useState('')
  const [protocolOverride, setProtocolOverride] = useState<'auto' | 'f1_24' | 'f1_25'>(
    () => (window.electronStore.get('udp.protocol', 'auto') as 'auto' | 'f1_24' | 'f1_25')
  )

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
    // auto + no live detection yet — check last persisted
    if (active_format) {
      return `${active_format} (last session)`
    }
    return 'No data yet'
  }

  const inputCls = 'bg-[var(--bg-input)] border border-[var(--border-muted)] rounded-lg text-xs text-[var(--text-primary)] px-3 h-8 outline-none focus:border-[var(--border-focus)] transition-colors w-full tabular-nums'
  const card = 'bg-[var(--bg-card)] border border-[var(--border-subtle)] rounded-xl overflow-hidden divide-y divide-[var(--border-subtle)]'

  return (
    <div className="h-full overflow-y-auto">
      <div className="p-8 max-w-[1300px] mx-auto">

        <h1 className="text-sm font-semibold text-[var(--text-primary)] mb-8">Settings</h1>

        <div className="space-y-7">

          <div>
            <SectionLabel icon={<Sun size={12} color="#f59e0b" />} label="Appearance" />
            <div className={card}>
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
          </div>

          <div>
            <SectionLabel icon={<Clock size={12} color="#8b5cf6" />} label="Notifications" />
            <div className={card}>
              <Row label="Event Banner Duration" description="How long each race event notification is shown before the next one">
                <SegmentedControl
                  options={[2, 3, 5, 8, 10].map(v => ({ value: v, label: `${v}s` }))}
                  value={bannerDuration} onChange={onBannerDurationChange}
                />
              </Row>
            </div>
          </div>

          <div>
            <SectionLabel icon={<Map size={12} color="#10b981" />} label="Map" />
            <div className={card}>
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
          </div>

          <div>
            <SectionLabel icon={<Network size={12} color="#0ea5e9" />} label="Network" />
            <div className={card}>
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
              <div className="flex items-center justify-between px-5 py-3 bg-[var(--bg-input)]/30">
                <p className="text-xs">
                  {udpStatus === 'ok'       && <span className="text-green-400">Listener restarted successfully</span>}
                  {udpStatus === 'error'    && <span className="text-red-400">{errorMsg}</span>}
                  {udpStatus === 'applying' && <span className="text-[var(--text-muted)]">Restarting…</span>}
                  {udpStatus === 'idle'     && (dirty
                    ? <span className="text-amber-400/80">Unsaved changes</span>
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
          </div>

          <div>
            <SectionLabel icon={<Radio size={12} color="#e879f9" />} label="Protocol" />
            <div className={card}>
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
              {protocolWarning && (
                <div className="flex items-center gap-3 px-5 py-4 border-t border-[var(--border-subtle)]">
                  <div className="relative shrink-0">
                    {/* Pulsing glow ring */}
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
          </div>

        </div>
      </div>
    </div>
  )
}
