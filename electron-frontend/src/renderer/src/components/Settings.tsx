import { Fragment, useLayoutEffect, useRef, useState, memo } from 'react'
import { flushSync } from 'react-dom'
import { Clock, Network, Sun, Map, AlertTriangle, Radio, X, Info, HardDrive, ScrollText, ChevronDown, ExternalLink, LineChart, Shrink, MoveVertical, LayoutGrid } from 'lucide-react'
import type { ProtocolStatusMsg, ProtocolWarningMsg } from '../types'
import {
  GRAPH_GROUPS, ALL_GRAPH_SECTIONS, COMPACT_GROUPS, ALL_COMPACT_BOOL_KEYS, DENSITY_OPTIONS, TYRE_LEVEL_OPTIONS, WEATHER_LEVEL_OPTIONS, HEADER_LEVEL_OPTIONS,
  TYRE_Y_AXIS_SECTIONS, POWER_Y_AXIS_SECTIONS,
  type GraphViewState, type GraphView, type CompactState, type DensityMode, type ChartYAxisState, type YAxisBehavior,
} from '../lib/graphSections'
import iconTransparent from '../assets/icon_transparent.png'
import iconTransparentLight from '../assets/icon_transparent_light.png'
import { ATTRIBUTIONS, ATTRIBUTION_SECTIONS } from '../data/attributions'
import type { ChartFrameRate } from '../lib/timechart/frameRate'
import { BUTTON_CLASS } from '../lib/buttonStyles'
import type { PageLayouts, Theme, TitlebarUpdateInterval } from '../app/appConfig'
import { useModalPresence } from '../lib/useModalPresence'

interface Props {
  isOpen: boolean
  onClose: () => void
  tyreView: 'cards' | 'graphs'
  onTyreViewChange: (v: 'cards' | 'graphs') => void
  tyreWearMode: 'wear' | 'life'
  onTyreWearModeChange: (v: 'wear' | 'life') => void
  bannerDuration: number
  onBannerDurationChange: (v: number) => void
  theme: Theme
  onThemeChange: (v: Theme) => void
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
  titlebarUpdateInterval: TitlebarUpdateInterval
  onTitlebarUpdateIntervalChange: (v: TitlebarUpdateInterval) => void
  reduceAnimations: boolean
  onReduceAnimationsChange: (v: boolean) => void
  fpsInFocus: ChartFrameRate
  onFpsInFocusChange: (v: ChartFrameRate) => void
  fpsOutOfFocus: ChartFrameRate
  onFpsOutOfFocusChange: (v: ChartFrameRate) => void
  mapDimmed: boolean
  onMapDimmedChange: (v: boolean) => void
  pageLayouts: PageLayouts
  onPageLayoutsChange: (v: PageLayouts) => void
  secondaryHorizontalCrosshairEnabled: boolean
  onSecondaryHorizontalCrosshairEnabledChange: (v: boolean) => void
  secondaryVerticalCrosshairEnabled: boolean
  onSecondaryVerticalCrosshairEnabledChange: (v: boolean) => void
  graphView: GraphViewState
  onGraphViewChange: (v: GraphViewState) => void
  compact: CompactState
  onCompactChange: (v: CompactState) => void
  chartYAxis: ChartYAxisState
  onChartYAxisChange: (v: ChartYAxisState) => void
}

type Option<T> = { value: T; label: string }

const SegmentedControl = memo(function SegmentedControl<T extends string | number | boolean>({
  options, value, onChange,
}: {
  options: Option<T>[]
  value: T
  onChange: (v: T) => void
}) {
  const containerRef = useRef<HTMLDivElement>(null)
  const itemRefs = useRef(new globalThis.Map<string, HTMLButtonElement>())
  const [indicator, setIndicator] = useState<{ left: number; width: number } | null>(null)

  useLayoutEffect(() => {
    const el = itemRefs.current.get(String(value))
    if (!el) return

    const updateIndicator = () => {
      setIndicator(current => {
        const next = { left: el.offsetLeft, width: el.offsetWidth }
        return current !== null && current.left === next.left && current.width === next.width ? current : next
      })
    }

    updateIndicator()
    const container = containerRef.current
    const resizeObserver = new ResizeObserver(updateIndicator)
    if (container) resizeObserver.observe(container)
    resizeObserver.observe(el)
    return () => resizeObserver.disconnect()
  }, [value, options])

  return (
    <div ref={containerRef} className="relative flex gap-1 isolate">
      {indicator && (
        <div
          aria-hidden="true"
          className="pointer-events-none absolute bottom-0 left-0 h-[2px] rounded-full bg-[var(--border-focus)] transition-[transform,width] duration-[220ms] ease-[cubic-bezier(0.22,1,0.36,1)]"
          style={{
            width: indicator.width,
            transform: `translateX(${indicator.left}px)`,
          }}
        />
      )}
      {options.map((opt) => {
        const active = opt.value === value
        return (
          <button
            key={String(opt.value)}
            ref={(node) => {
              if (node) itemRefs.current.set(String(opt.value), node)
              else itemRefs.current.delete(String(opt.value))
            }}
            onClick={() => onChange(opt.value)}
            className={`relative z-10 px-3 py-1 text-xs font-medium whitespace-nowrap transition-colors border-b-2 border-transparent ${
              active
                ? 'text-[var(--text-primary)]'
                : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)]'
            }`}
          >
            {opt.label}
          </button>
        )
      })}
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
    <div className="flex items-center justify-between gap-8 px-4 py-3.5 rounded-xl">
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
type UdpForwardTarget = { address: string; port: number }

function loadForwardTargets(): UdpForwardTarget[] {
  const value = window.electronStore.get('udp.forwardTargets', [])
  if (!Array.isArray(value)) return []
  return value.slice(0, 15).flatMap((item): UdpForwardTarget[] => {
    if (!item || typeof item !== 'object') return []
    const candidate = item as Record<string, unknown>
    return typeof candidate.address === 'string' && typeof candidate.port === 'number'
      ? [{ address: candidate.address, port: candidate.port }]
      : []
  })
}

function isIpv4Address(value: string): boolean {
  const parts = value.trim().split('.')
  return parts.length === 4 && parts.every(part => /^\d{1,3}$/.test(part) && Number(part) <= 255)
}

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
  titlebarUpdateInterval,
  onTitlebarUpdateIntervalChange,
  reduceAnimations,
  onReduceAnimationsChange,
  fpsInFocus,
  onFpsInFocusChange,
  fpsOutOfFocus,
  onFpsOutOfFocusChange,
  mapDimmed,
  onMapDimmedChange,
  pageLayouts,
  onPageLayoutsChange,
  secondaryHorizontalCrosshairEnabled,
  onSecondaryHorizontalCrosshairEnabledChange,
  secondaryVerticalCrosshairEnabled,
  onSecondaryVerticalCrosshairEnabledChange,
  graphView,
  onGraphViewChange,
  compact,
  onCompactChange,
  chartYAxis,
  onChartYAxisChange,
}: Props) {
  const modalPresence = useModalPresence(isOpen)
  const [activeCategory, setActiveCategory] = useState<'appearance' | 'layout' | 'graphs' | 'yAxis' | 'compact' | 'notifications' | 'map' | 'network' | 'protocol' | 'storage'>('appearance')
  const [view, setView] = useState<'category' | 'about' | 'attributions'>('category')
  const [expandedLicense, setExpandedLicense] = useState<string | null>(null)
  const settingsContentRef = useRef<HTMLDivElement>(null)
  const settingsNavigationSequence = useRef(0)
  const settingsSidebarRef = useRef<HTMLDivElement>(null)
  const settingsSidebarItemRefs = useRef(new globalThis.Map<string, HTMLButtonElement>())
  const [sidebarIndicator, setSidebarIndicator] = useState<{ top: number; height: number } | null>(null)
  const selectedSidebarItem = view === 'category' ? activeCategory : view

  useLayoutEffect(() => {
    const sidebar = settingsSidebarRef.current
    const item = settingsSidebarItemRefs.current.get(selectedSidebarItem)
    if (!sidebar || !item) return

    const updateIndicator = () => {
      setSidebarIndicator(current => {
        const next = { top: item.offsetTop, height: item.offsetHeight }
        return current !== null && current.top === next.top && current.height === next.height ? current : next
      })
    }

    updateIndicator()
    const resizeObserver = new ResizeObserver(updateIndicator)
    resizeObserver.observe(sidebar)
    resizeObserver.observe(item)
    return () => resizeObserver.disconnect()
  }, [modalPresence.mounted, selectedSidebarItem])

  function navigateToSettingsView(
    nextView: 'category' | 'about' | 'attributions',
    nextCategory = activeCategory,
  ): void {
    if (nextView === view && (nextView !== 'category' || nextCategory === activeCategory)) return

    const updateView = () => {
      setActiveCategory(nextCategory)
      setView(nextView)
    }
    const content = settingsContentRef.current
    if (reduceAnimations || !content) {
      settingsNavigationSequence.current += 1
      content?.getAnimations().forEach(animation => animation.cancel())
      updateView()
      return
    }

    const sequence = ++settingsNavigationSequence.current
    content.getAnimations().forEach(animation => animation.cancel())
    const currentOpacity = getComputedStyle(content).opacity
    const fadeOut = content.animate(
      [{ opacity: currentOpacity }, { opacity: 0 }],
      { duration: 90, easing: 'cubic-bezier(0.4, 0, 1, 1)', fill: 'both' },
    )

    void fadeOut.finished.then(() => {
      if (sequence !== settingsNavigationSequence.current) return
      flushSync(updateView)
      fadeOut.cancel()

      const fadeIn = content.animate(
        [
          { opacity: 0, transform: 'translateY(10px)' },
          { opacity: 1, transform: 'translateY(0)' },
        ],
        { duration: 210, easing: 'cubic-bezier(0.16, 1, 0.3, 1)', fill: 'both' },
      )
      void fadeIn.finished.then(() => fadeIn.cancel(), () => {})
    }, () => {})
  }
  
  const [loggingEnabled, setLoggingEnabled] = useState<boolean>(() => window.electronStore.get('logging.enabled', false) as boolean)
  const [updateChecksEnabled, setUpdateChecksEnabled] = useState<boolean>(() => window.electronStore.get('updates.enabled', true) as boolean)
  const [loggingDirectory, setLoggingDirectory] = useState<string>(() => window.electronStore.get('logging.directory', '') as string)
  
  const [port, setPort]         = useState<number>(() => window.electronStore.get('udp.port', 20777) as number)
  const [addr, setAddr]         = useState<string>(() => window.electronStore.get('udp.bindAddress', '0.0.0.0') as string)
  const [forwardingEnabled, setForwardingEnabled] = useState<boolean>(
    () => window.electronStore.get('udp.forwardingEnabled', false) as boolean,
  )
  const [forwardTargets, setForwardTargets] = useState<UdpForwardTarget[]>(loadForwardTargets)
  const [udpStatus, setUdpStatus] = useState<RestartStatus>('idle')
  const [errorMsg, setErrorMsg]   = useState('')
  const [protocolOverride, setProtocolOverride] = useState<'auto' | 'f1_24' | 'f1_25' | 'f1_26'>(
    () => (window.electronStore.get('udp.protocol', 'auto') as 'auto' | 'f1_24' | 'f1_25' | 'f1_26')
  )

  if (!modalPresence.mounted) return null

  const portValid = Number.isInteger(port) && port >= 1 && port <= 65535
  const forwardTargetsValid = !forwardingEnabled || forwardTargets.every(target =>
    isIpv4Address(target.address) && Number.isInteger(target.port) && target.port >= 1 && target.port <= 65535 &&
    !(target.port === port && (target.address.trim().startsWith('127.') || target.address.trim() === addr.trim())),
  )
  const dirty     = port !== (window.electronStore.get('udp.port', 20777) as number)
                 || addr !== (window.electronStore.get('udp.bindAddress', '0.0.0.0') as string)
                 || forwardingEnabled !== (window.electronStore.get('udp.forwardingEnabled', false) as boolean)
                 || JSON.stringify(forwardTargets) !== JSON.stringify(loadForwardTargets())

  async function applyUdp() {
    if (!portValid || !forwardTargetsValid || udpStatus === 'applying') return
    setUdpStatus('applying')
    const normalizedForwardTargets = forwardTargets.map(target => ({
      address: target.address.trim(), port: target.port,
    }))
    setForwardTargets(normalizedForwardTargets)
    window.electronStore.set('udp.port', port)
    window.electronStore.set('udp.bindAddress', addr)
    window.electronStore.set('udp.forwardingEnabled', forwardingEnabled)
    window.electronStore.set('udp.forwardTargets', normalizedForwardTargets)
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

  function handleUpdateChecksToggle(value: boolean) {
    setUpdateChecksEnabled(value)
    window.electronStore.set('updates.enabled', value)
  }



  const inputCls = 'bg-[var(--bg-input)] border border-[var(--border-muted)] rounded-lg text-xs text-[var(--text-primary)] px-3 h-8 outline-none focus:border-[var(--border-focus)] transition-colors w-full tabular-nums'

  const CATEGORIES = [
    { id: 'appearance' as const, label: 'Appearance', icon: <Sun size={14} />, color: '#f59e0b' },
    { id: 'layout' as const, label: 'Layout', icon: <LayoutGrid size={14} />, color: '#f97316' },
    { id: 'graphs' as const, label: 'Graphs', icon: <LineChart size={14} />, color: '#0ea5e9' },
    { id: 'yAxis' as const, label: 'Y Axis Behavior', icon: <MoveVertical size={14} />, color: '#6366f1' },
    { id: 'compact' as const, label: 'Compact', icon: <Shrink size={14} />, color: '#14b8a6' },
    { id: 'notifications' as const, label: 'Notifications', icon: <Clock size={14} />, color: '#8b5cf6' },
    { id: 'map' as const, label: 'Map', icon: <Map size={14} />, color: '#10b981' },
    { id: 'network' as const, label: 'Network', icon: <Network size={14} />, color: '#0ea5e9' },
    { id: 'protocol' as const, label: 'Protocol', icon: <Radio size={14} />, color: '#e879f9' },
    { id: 'storage' as const, label: 'Data Storage', icon: <HardDrive size={14} />, color: '#10b981' },
  ]

  const renderAppearance = () => (
    <div className="flex flex-col gap-1">
      <Row label="Theme" description="Switch between dark, midnight, and light interface">
        <SegmentedControl<Theme>
          options={[
            { value: 'dark', label: 'Dark' },
            { value: 'midnight', label: 'Midnight' },
            { value: 'light', label: 'Light' },
          ]}
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
      <Row label="Delta Updates" description="How often the session timer and lap-comparison delta refresh.">
        <SegmentedControl<TitlebarUpdateInterval>
          options={[
            { value: 0, label: 'Realtime' },
            { value: 250, label: '250 ms' },
            { value: 500, label: '500 ms' },
            { value: 1000, label: '1 second' },
          ]}
          value={titlebarUpdateInterval}
          onChange={onTitlebarUpdateIntervalChange}
        />
      </Row>
      <Row
        label="Reduce Animations"
        description="Disable motion effects across the app, including position-swap transitions and flashes in the Standings table."
      >
        <Toggle value={reduceAnimations} onChange={onReduceAnimationsChange} />
      </Row>
      <Row label="FPS in focus" description="Maximum chart frame rate while the application window is focused.">
        <SegmentedControl<ChartFrameRate>
          options={[
            { value: 0, label: 'Pause' },
            { value: 1, label: '1' },
            { value: 10, label: '10' },
            { value: 30, label: '30' },
            { value: 60, label: '60' },
            { value: 120, label: '120' },
            { value: 'display', label: 'Match display' },
          ]}
          value={fpsInFocus}
          onChange={onFpsInFocusChange}
        />
      </Row>
      <Row label="FPS out of focus" description="Maximum chart frame rate while the application window is not focused.">
        <SegmentedControl<ChartFrameRate>
          options={[
            { value: 0, label: 'Pause' },
            { value: 1, label: '1' },
            { value: 10, label: '10' },
            { value: 30, label: '30' },
            { value: 60, label: '60' },
            { value: 120, label: '120' },
            { value: 'display', label: 'Match display' },
          ]}
          value={fpsOutOfFocus}
          onChange={onFpsOutOfFocusChange}
        />
      </Row>
    </div>
  )

  const GroupLabel = ({ children }: { children: React.ReactNode }) => (
    <div className="px-4 pt-4 pb-1 text-[10px] font-semibold uppercase tracking-widest text-[var(--text-muted)]">{children}</div>
  )

  const renderLayout = () => (
    <div className="flex flex-col gap-1">
      <GroupLabel>Inputs</GroupLabel>
      <Row
        label="Chart Layout"
        description="Arrange the Input page as a grid or a vertical stack. Every chart keeps its own selectable horizontal axis."
      >
        <SegmentedControl
          options={[
            { value: 'grid' as const, label: 'Grid' },
            { value: 'vertical' as const, label: 'Vertical' },
          ]}
          value={pageLayouts.input}
          onChange={(input) => onPageLayoutsChange({ ...pageLayouts, input })}
        />
      </Row>
      <Row
        label="Pedal Charts"
        description="Combined uses a signed centre line; Combined 2 overlays both inputs from 0–100%; Split uses two independent charts."
      >
        <SegmentedControl
          options={[
            { value: 'combined' as const, label: 'Combined' },
            { value: 'combined2' as const, label: 'Combined 2' },
            { value: 'split' as const, label: 'Split' },
          ]}
          value={pageLayouts.inputPedals}
          onChange={(inputPedals) => onPageLayoutsChange({ ...pageLayouts, inputPedals })}
        />
      </Row>
      <GroupLabel>Misc</GroupLabel>
      <Row
        label="G-Force Charts"
        description="Show lateral and longitudinal G-force together or as two aligned charts."
      >
        <SegmentedControl
          options={[
            { value: 'combined' as const, label: 'Combined' },
            { value: 'split' as const, label: 'Split' },
          ]}
          value={pageLayouts.miscGForce}
          onChange={(miscGForce) => onPageLayoutsChange({ ...pageLayouts, miscGForce })}
        />
      </Row>
      <Row
        label="Ride Height Charts"
        description="Show front and rear ride height together or as two aligned charts."
      >
        <SegmentedControl
          options={[
            { value: 'combined' as const, label: 'Combined' },
            { value: 'split' as const, label: 'Split' },
          ]}
          value={pageLayouts.miscRideHeight}
          onChange={(miscRideHeight) => onPageLayoutsChange({ ...pageLayouts, miscRideHeight })}
        />
      </Row>
      <GroupLabel>Power</GroupLabel>
      <Row
        label="Chart Layout"
        description="Arrange the Power charts as a 2×2 grid or an aligned vertical stack. Every chart keeps its own selectable horizontal axis."
      >
        <SegmentedControl
          options={[
            { value: 'grid' as const, label: 'Grid' },
            { value: 'vertical' as const, label: 'Vertical' },
          ]}
          value={pageLayouts.power}
          onChange={(power) => onPageLayoutsChange({ ...pageLayouts, power })}
        />
      </Row>
      <GroupLabel>Tyres</GroupLabel>
      <Row
        label="Chart Layout"
        description="Arrange the Tyres charts as a 2×2 grid or an aligned vertical stack. Every chart keeps its own selectable horizontal axis."
      >
        <SegmentedControl
          options={[
            { value: 'grid' as const, label: 'Grid' },
            { value: 'vertical' as const, label: 'Vertical' },
          ]}
          value={pageLayouts.tyres}
          onChange={(tyres) => onPageLayoutsChange({ ...pageLayouts, tyres })}
        />
      </Row>
      <GroupLabel>Shared Tooltip</GroupLabel>
      <Row
        label="Secondary Vertical Crosshair"
        description="Draw the vertical cursor line on synchronized secondary charts. The hovered chart always keeps its normal crosshair."
      >
        <Toggle value={secondaryVerticalCrosshairEnabled} onChange={onSecondaryVerticalCrosshairEnabledChange} />
      </Row>
      <Row
        label="Secondary Horizontal Crosshair"
        description="Draw the horizontal cursor line on synchronized secondary charts. The hovered chart always keeps its normal crosshair."
      >
        <Toggle value={secondaryHorizontalCrosshairEnabled} onChange={onSecondaryHorizontalCrosshairEnabledChange} />
      </Row>
    </div>
  )

  const BulkButton = ({ label, onClick }: { label: string; onClick: () => void }) => (
    <button
      onClick={onClick}
      className={BUTTON_CLASS}
    >
      {label}
    </button>
  )

  const renderGraphs = () => {
    const anyChart = ALL_GRAPH_SECTIONS.some(k => graphView[k] === 'chart')
    const setAll = (v: GraphView) =>
      onGraphViewChange(Object.fromEntries(ALL_GRAPH_SECTIONS.map(k => [k, v])) as GraphViewState)
    return (
      <div className="flex flex-col gap-1">
        <div className="flex items-center justify-end px-4 py-3">
          <BulkButton label={anyChart ? 'Set All Table' : 'Set All Chart'} onClick={() => setAll(anyChart ? 'table' : 'chart')} />
        </div>
        {GRAPH_GROUPS.map(group => (
          <div key={group.group}>
            <GroupLabel>{group.group}</GroupLabel>
            {group.sections.map(s => (
              <Row key={s.key} label={s.label} description="">
                <SegmentedControl
                  options={[{ value: 'chart' as const, label: s.chartLabel ?? 'Chart' }, { value: 'table' as const, label: 'Table' }]}
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
    const setAllDensity = (mode: DensityMode) => {
      const next: CompactState = {
        ...compact,
        overviewTyres: mode === 'spacious' ? 6 : mode === 'compact' ? 1 : 0,
        sessionWeather: mode === 'spacious' ? 4 : mode === 'compact' ? 1 : 0,
        sessionHeader: mode === 'spacious' ? 3 : mode === 'compact' ? 1 : 0,
      }
      for (const k of ALL_COMPACT_BOOL_KEYS) next[k] = mode
      onCompactChange(next)
    }
    return (
      <div className="flex flex-col gap-1">
        <div className="flex items-center justify-end px-4 py-3 gap-2">
          <BulkButton label="Set All Compact" onClick={() => setAllDensity('compact')} />
          <BulkButton label="Set All Normal" onClick={() => setAllDensity('normal')} />
          <BulkButton label="Set All Spacious" onClick={() => setAllDensity('spacious')} />
        </div>
        {COMPACT_GROUPS.map(group => (
          <div key={group.group}>
            <GroupLabel>{group.group}</GroupLabel>
            {group.sections.map(s => (
              <Fragment key={s.key}>
                <Row label={s.label} description="">
                  <SegmentedControl
                    options={DENSITY_OPTIONS}
                    value={compact[s.key]}
                    onChange={(v) => onCompactChange({ ...compact, [s.key]: v })}
                  />
                </Row>
                {s.key === 'sessionCards' && (
                  <>
                    <Row label="Header" description="">
                      <SegmentedControl
                        options={HEADER_LEVEL_OPTIONS}
                        value={compact.sessionHeader}
                        onChange={(v) => onCompactChange({ ...compact, sessionHeader: v })}
                      />
                    </Row>
                    <Row label="Weather Strip" description="">
                      <SegmentedControl
                        options={WEATHER_LEVEL_OPTIONS}
                        value={compact.sessionWeather}
                        onChange={(v) => onCompactChange({ ...compact, sessionWeather: v })}
                      />
                    </Row>
                  </>
                )}
              </Fragment>
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

  const renderYAxis = () => {
    const tyreGroups = [
      { key: 'overview' as const, label: 'Overview' },
      { key: 'tyres' as const, label: 'Tyres' },
    ]
    const anyDynamic = tyreGroups.some(group =>
      TYRE_Y_AXIS_SECTIONS.some(section => chartYAxis[group.key][section.key] === 'dynamic'),
    ) || POWER_Y_AXIS_SECTIONS.some(section => chartYAxis.power[section.key] === 'dynamic')
    const setAll = (value: YAxisBehavior) => {
      const tyres = Object.fromEntries(TYRE_Y_AXIS_SECTIONS.map(s => [s.key, value])) as ChartYAxisState['overview']
      const power = Object.fromEntries(POWER_Y_AXIS_SECTIONS.map(s => [s.key, value])) as ChartYAxisState['power']
      onChartYAxisChange({ overview: { ...tyres }, tyres: { ...tyres }, power })
    }
    return (
      <div className="flex flex-col gap-1">
        <div className="flex items-center justify-end px-4 py-3">
          <BulkButton
            label={anyDynamic ? 'Set All Fixed' : 'Set All Dynamic'}
            onClick={() => setAll(anyDynamic ? 'fixed' : 'dynamic')}
          />
        </div>
        {tyreGroups.map(group => (
          <div key={group.key}>
            <GroupLabel>{group.label}</GroupLabel>
            {TYRE_Y_AXIS_SECTIONS.map(section => (
              <Row key={section.key} label={section.label} description={`Fixed: ${section.fixedRange}`}>
                <SegmentedControl
                  options={[{ value: 'fixed' as const, label: 'Fixed' }, { value: 'dynamic' as const, label: 'Dynamic' }]}
                  value={chartYAxis[group.key][section.key]}
                  onChange={(value) => onChartYAxisChange({
                    ...chartYAxis,
                    [group.key]: { ...chartYAxis[group.key], [section.key]: value },
                  })}
                />
              </Row>
            ))}
          </div>
        ))}
        <div>
          <GroupLabel>Power</GroupLabel>
          {POWER_Y_AXIS_SECTIONS.map(section => (
            <Row key={section.key} label={section.label} description={`Fixed: ${section.fixedRange}`}>
              <SegmentedControl
                options={[{ value: 'fixed' as const, label: 'Fixed' }, { value: 'dynamic' as const, label: 'Dynamic' }]}
                value={chartYAxis.power[section.key]}
                onChange={(value) => onChartYAxisChange({
                  ...chartYAxis,
                  power: { ...chartYAxis.power, [section.key]: value },
                })}
              />
            </Row>
          ))}
        </div>
      </div>
    )
  }

  const renderNotifications = () => (
    <div className="flex flex-col gap-1">
      <Row label="Event Banner Duration" description="How long each race event notification is shown before the next one">
        <SegmentedControl
          options={[2, 3, 5, 8, 10].map(v => ({ value: v, label: `${v}s` }))}
          value={bannerDuration} onChange={onBannerDurationChange}
        />
      </Row>
      <Row
        label="Check for Updates"
        description="Check GitHub for a newer Track N Race release when the app starts, at most once per day."
      >
        <Toggle value={updateChecksEnabled} onChange={handleUpdateChecksToggle} />
      </Row>
    </div>
  )

  const renderMap = () => (
    <div className="flex flex-col gap-1">
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
    <div className="flex flex-col gap-1">
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

      <Row
        label="UDP Forward Mode"
        description="Forward every received packet unchanged to the configured destinations."
      >
        <Toggle value={forwardingEnabled} onChange={setForwardingEnabled} />
      </Row>

      {forwardingEnabled && (
        <div className="mx-4 my-2 rounded-xl border border-[var(--border-muted)] overflow-hidden">
          <div className="flex items-center justify-between px-3 py-2.5 border-b border-[var(--border-muted)] bg-[var(--bg-input)]/40">
            <div>
              <p className="text-xs font-semibold text-[var(--text-primary)]">Forwarding channels</p>
              <p className="text-[10px] text-[var(--text-muted)] mt-0.5">IPv4 destination and port · {forwardTargets.length}/15 configured</p>
            </div>
            <button
              type="button"
              onClick={() => setForwardTargets([...forwardTargets, { address: '', port: 20777 }])}
              disabled={forwardTargets.length >= 15}
              className={BUTTON_CLASS}
            >
              Add channel
            </button>
          </div>
          {forwardTargets.length === 0 ? (
            <p className="px-3 py-4 text-xs text-center text-[var(--text-muted)]">No forwarding channels configured.</p>
          ) : forwardTargets.map((target, index) => {
            const targetValid = isIpv4Address(target.address) && Number.isInteger(target.port) &&
              target.port >= 1 && target.port <= 65535 &&
              !(target.port === port && (target.address.trim().startsWith('127.') || target.address.trim() === addr.trim()))
            return (
              <div key={index} className="flex items-center gap-2 px-3 py-2 border-t first:border-t-0 border-[var(--border-muted)]">
                <span className="w-5 text-[10px] text-[var(--text-muted)] tabular-nums">{index + 1}</span>
                <input
                  type="text"
                  aria-label={`Forwarding channel ${index + 1} address`}
                  value={target.address}
                  placeholder="192.168.1.100"
                  onChange={event => setForwardTargets(forwardTargets.map((item, itemIndex) =>
                    itemIndex === index ? { ...item, address: event.target.value } : item))}
                  className={`${inputCls} flex-1 ${!targetValid ? 'border-red-600/60' : ''}`}
                />
                <input
                  type="number"
                  aria-label={`Forwarding channel ${index + 1} port`}
                  min={1}
                  max={65535}
                  value={target.port}
                  onChange={event => setForwardTargets(forwardTargets.map((item, itemIndex) =>
                    itemIndex === index ? { ...item, port: Number(event.target.value) } : item))}
                  className={`${inputCls} no-number-spinner !w-20 shrink-0 ${!targetValid ? 'border-red-600/60' : ''}`}
                />
                <button
                  type="button"
                  aria-label={`Remove forwarding channel ${index + 1}`}
                  onClick={() => setForwardTargets(forwardTargets.filter((_, itemIndex) => itemIndex !== index))}
                  className="p-1.5 rounded-md text-[var(--text-muted)] hover:text-red-400 hover:bg-red-500/10 transition-colors"
                >
                  <X size={14} />
                </button>
              </div>
            )
          })}
          {!forwardTargetsValid && (
            <p className="px-3 pb-2 text-[10px] text-red-400">Enter valid IPv4 destinations and ports. Forwarding back to this listener would create a packet loop.</p>
          )}
        </div>
      )}
      
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
          disabled={!portValid || !forwardTargetsValid || udpStatus === 'applying'}
          className={BUTTON_CLASS}
        >
          {udpStatus === 'applying' ? 'Restarting…' : udpStatus === 'ok' ? 'Applied' : 'Apply & Restart'}
        </button>
      </div>
    </div>
  )

  const renderProtocol = () => (
    <div className="flex flex-col gap-1">
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
    <div className="flex flex-col gap-1">
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
            className={BUTTON_CLASS}
          >
            Browse
          </button>
        </div>
      </Row>
    </div>
  )

  const renderAbout = () => (
    <div className="flex flex-col items-center justify-center text-center py-12 w-full max-w-[640px] select-none mx-auto my-auto">
      {/* Logo */}
      <img
        src={theme === 'light' ? iconTransparentLight : iconTransparent}
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
    <div className="w-full select-none">
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
              {items.map((item, itemIndex) => {
                const expanded = expandedLicense === item.name
                const licensePanelId = `attribution-license-${section.category}-${itemIndex}`
                return (
                  <div
                    key={item.name}
                    className="border border-[var(--border)] rounded-lg bg-[var(--bg-card)]/40 overflow-hidden"
                  >
                    <button
                      type="button"
                      onClick={() => setExpandedLicense(expanded ? null : item.name)}
                      aria-expanded={expanded}
                      aria-controls={licensePanelId}
                      className="w-full flex items-center gap-2 px-3 py-2.5 text-left hover:bg-[var(--bg-hover)] transition-colors"
                    >
                      <ChevronDown
                        size={13}
                        className={`shrink-0 text-[var(--text-muted)] transition-transform ${expanded ? 'rotate-180' : ''}`}
                      />
                      <span className="text-xs font-semibold font-mono text-[var(--text-primary)]">
                        {item.name}
                      </span>
                      {item.badge && (
                        <span className="text-[8px] font-mono font-bold uppercase tracking-wider text-[#9f7aea] border border-[#9f7aea]/40 bg-[#9f7aea]/10 px-1.5 py-0.5 rounded-full">
                          {item.badge}
                        </span>
                      )}
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
                    <div
                      id={licensePanelId}
                      aria-hidden={!expanded}
                      className={`attribution-license-panel grid ${expanded ? 'grid-rows-[1fr] opacity-100' : 'grid-rows-[0fr] opacity-0'}`}
                    >
                      <div className="min-h-0 overflow-hidden">
                        <pre className="max-h-[240px] overflow-y-auto whitespace-pre-wrap break-words bg-[var(--bg-input)] border-t border-[var(--border)] p-3 text-[10px] leading-relaxed font-mono text-[var(--text-secondary)]">
                          {item.licenseText.trim()}
                        </pre>
                      </div>
                    </div>
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
      case 'layout':
        return renderLayout()
      case 'graphs':
        return renderGraphs()
      case 'yAxis':
        return renderYAxis()
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
      data-state={modalPresence.visible ? 'open' : 'closed'}
      className="modal-backdrop fixed inset-0 z-50 flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
    >
      <div
        className="modal-panel bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[1080px] h-[650px] max-h-[85vh] flex flex-col overflow-hidden"
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
            className="w-9 h-9 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#e10600] transition-colors"
          >
            <X size={18} />
          </button>
        </div>

        {/* Body Container (Sidebar + Content) */}
        <div className="flex flex-1 min-h-0">
          {/* Sidebar */}
          <div ref={settingsSidebarRef} className="relative w-[220px] shrink-0 border-r border-[var(--border)] bg-[var(--bg-card)]/30 p-4 flex flex-col gap-1.5 overflow-y-auto select-none h-full">
            {sidebarIndicator && (
              <div
                aria-hidden="true"
                className="pointer-events-none absolute left-4 top-0 z-10 w-0.5 rounded-r-full bg-[var(--border-focus)] transition-[transform,height] duration-[220ms] ease-[cubic-bezier(0.22,1,0.36,1)]"
                style={{ height: sidebarIndicator.height, transform: `translateY(${sidebarIndicator.top}px)` }}
              />
            )}
            {CATEGORIES.map(cat => {
              const active = activeCategory === cat.id
              return (
                <button
                  key={cat.id}
                  ref={node => {
                    if (node) settingsSidebarItemRefs.current.set(cat.id, node)
                    else settingsSidebarItemRefs.current.delete(cat.id)
                  }}
                  onClick={() => navigateToSettingsView('category', cat.id)}
                  className={`w-full flex items-center gap-3 px-3 py-2.5 rounded text-xs font-semibold font-mono transition-colors text-left ${
                    active && view === 'category'
                      ? 'text-[var(--text-primary)]'
                      : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)]'
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
              ref={node => {
                if (node) settingsSidebarItemRefs.current.set('attributions', node)
                else settingsSidebarItemRefs.current.delete('attributions')
              }}
              onClick={() => navigateToSettingsView('attributions')}
              className={`w-full flex items-center gap-3 px-3 py-2.5 rounded text-xs font-semibold font-mono transition-colors text-left ${
                view === 'attributions'
                  ? 'text-[var(--text-primary)]'
                  : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)]'
              }`}
            >
              <ScrollText size={14} style={{ color: '#a0a8b8' }} />
              <span>Attribution</span>
            </button>

            {/* About Button */}
            <button
              ref={node => {
                if (node) settingsSidebarItemRefs.current.set('about', node)
                else settingsSidebarItemRefs.current.delete('about')
              }}
              onClick={() => navigateToSettingsView('about')}
              className={`w-full flex items-center gap-3 px-3 py-2.5 rounded text-xs font-semibold font-mono transition-colors text-left ${
                view === 'about'
                  ? 'text-[var(--text-primary)]'
                  : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)]'
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
            <div ref={settingsContentRef} className="w-full max-w-[858px]">
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
