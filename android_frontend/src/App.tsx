import { useCallback, useEffect, useRef, useState } from 'react'
import {
  Check,
  ChevronDown,
  ChevronLeft,
  ChevronRight,
  FolderOpen,
  Gauge,
  Monitor,
  RadioTower,
  RefreshCw,
  RotateCcw,
  ScanLine,
  Settings as SettingsIcon,
  Trash2,
} from 'lucide-react'
import Dashboard from './Dashboard'
import {
  Telemetry,
  type AndroidSettings,
  type DiscoveredDesktop,
  type SourceStateEvent,
  type TelemetrySource,
} from './native'
import { installTelemetryRuntime, subscribeSourceState } from './telemetryRuntime'

type Screen = 'dashboard' | 'settings' | 'pairing' | 'licenses'

const initialSettings: AndroidSettings = {
  source: 'direct',
  recordingEnabled: false,
  hasSavedDesktop: false,
  desktopName: '',
  recordingDirectory: 'Downloads/Track N Race',
  usingCustomDirectory: false,
}

interface SettingsProps {
  settings: AndroidSettings
  onSettings: (settings: AndroidSettings) => void
  onNavigate: (screen: Screen) => void
  onError: (message: string) => void
}

function Settings({ settings, onSettings, onNavigate, onError }: SettingsProps) {
  const updateSource = async (source: TelemetrySource) => {
    if (source === 'paired' && !settings.hasSavedDesktop) {
      onNavigate('pairing')
      return
    }
    try {
      onSettings(await Telemetry.setSource({ source }))
    } catch (error) {
      onError(error instanceof Error ? error.message : String(error))
    }
  }

  const updateRecording = async (enabled: boolean) => {
    try {
      onSettings(await Telemetry.setRecording({ enabled }))
    } catch (error) {
      onError(error instanceof Error ? error.message : String(error))
    }
  }

  const chooseDirectory = async () => {
    try {
      onSettings(await Telemetry.chooseRecordingDirectory())
    } catch (error) {
      onError(error instanceof Error ? error.message : String(error))
    }
  }

  const useDefaultDirectory = async () => {
    try {
      onSettings(await Telemetry.useDefaultRecordingDirectory())
    } catch (error) {
      onError(error instanceof Error ? error.message : String(error))
    }
  }

  const forgetDesktop = async () => {
    try {
      onSettings(await Telemetry.forgetDesktop())
    } catch (error) {
      onError(error instanceof Error ? error.message : String(error))
    }
  }

  return (
    <main className="page settings-page">
      <section className="setting-group">
        <h2>Telemetry source</h2>
        <div className="segmented" role="radiogroup" aria-label="Telemetry source">
          <button className={settings.source === 'direct' ? 'selected' : ''} onClick={() => void updateSource('direct')}>
            <RadioTower size={18} />
            <span>Direct UDP<small>Listen on this phone</small></span>
          </button>
          <button className={settings.source === 'paired' ? 'selected' : ''} onClick={() => void updateSource('paired')}>
            <Monitor size={18} />
            <span>Paired desktop<small>{settings.hasSavedDesktop ? settings.desktopName : 'Pair first'}</small></span>
          </button>
        </div>
        <button className="row-button" onClick={() => onNavigate('pairing')}>
          <span className="row-leading"><Monitor size={19} /><span><strong>Desktop pairing</strong><small>{settings.hasSavedDesktop ? settings.desktopName : 'No desktop paired'}</small></span></span>
          <ChevronRight size={19} aria-hidden="true" />
        </button>
        {settings.hasSavedDesktop && <button className="danger-link action-link" onClick={() => void forgetDesktop()}><Trash2 size={16} />Forget paired desktop</button>}
      </section>

      <section className="setting-group">
        <h2>Recording</h2>
        <label className="switch-row">
          <span><strong>Record sessions</strong><small>Record direct UDP sessions as .tnrd files</small></span>
          <input type="checkbox" checked={settings.recordingEnabled} onChange={event => void updateRecording(event.target.checked)} />
        </label>
        <button className="row-button" onClick={() => void chooseDirectory()}>
          <span className="row-leading"><FolderOpen size={19} /><span><strong>Recording folder</strong><small>{settings.recordingDirectory}</small></span></span>
          <ChevronRight size={19} aria-hidden="true" />
        </button>
        {settings.usingCustomDirectory && <button className="text-button action-link" onClick={() => void useDefaultDirectory()}><RotateCcw size={16} />Use default folder</button>}
      </section>

      <section className="setting-group">
        <h2>About</h2>
        <button className="row-button" onClick={() => onNavigate('licenses')}>
          <span className="row-leading"><Check size={19} /><span><strong>Open-source licenses</strong><small>Capacitor, React, and bundled libraries</small></span></span>
          <ChevronRight size={19} aria-hidden="true" />
        </button>
      </section>
    </main>
  )
}

interface PairingProps {
  onSettings: (settings: AndroidSettings) => void
  onDone: () => void
  onError: (message: string) => void
}

function Pairing({ onSettings, onDone, onError }: PairingProps) {
  const [desktops, setDesktops] = useState<Map<string, DiscoveredDesktop>>(new Map())
  const [selectedId, setSelectedId] = useState('')
  const [code, setCode] = useState('')
  const [busy, setBusy] = useState(false)

  useEffect(() => {
    let cancelled = false
    let handle: { remove: () => Promise<void> } | undefined
    void Telemetry.addListener('discoveredDesktop', desktop => {
      setDesktops(current => new Map(current).set(desktop.serverId, desktop))
      setSelectedId(current => current || desktop.serverId)
    }).then(value => {
      if (cancelled) void value.remove()
      else handle = value
    })
    void Telemetry.startDiscovery().catch(error => onError(error instanceof Error ? error.message : String(error)))
    return () => {
      cancelled = true
      if (handle) void handle.remove()
      void Telemetry.stopDiscovery()
    }
  }, [onError])

  const finishPair = async (operation: () => Promise<void>) => {
    setBusy(true)
    try {
      await operation()
      onSettings(await Telemetry.getSettings())
      onDone()
    } catch (error) {
      onError(error instanceof Error ? error.message : String(error))
    } finally {
      setBusy(false)
    }
  }

  const scanQr = async () => {
    await finishPair(async () => {
      const { payload } = await Telemetry.scanPairingQr()
      if (!payload) throw new Error('QR scan cancelled')
      await Telemetry.pairQr({ payload })
    })
  }

  const pairCode = async () => {
    const desktop = desktops.get(selectedId)
    if (!desktop) {
      onError('Select a desktop broadcasting on your network')
      return
    }
    await finishPair(() => Telemetry.pairCode({ ...desktop, code }))
  }

  return (
    <main className="page pairing-page">
      <section className="hero-card">
        <span className="pair-icon" aria-hidden="true"><ScanLine size={28} /></span>
        <h2>Connect to Track N Race desktop</h2>
        <p>Keep both devices on the same network. Open paired mode on the desktop, then scan its QR code or enter the matching code.</p>
        <button className="primary-button button-with-icon" disabled={busy} onClick={() => void scanQr()}><ScanLine size={18} />Scan desktop QR code</button>
      </section>

      <section className="setting-group">
        <h2>Nearby desktops</h2>
        {desktops.size === 0 ? <p className="muted">Searching the local network…</p> : (
          <div className="desktop-list">
            {[...desktops.values()].map(desktop => (
              <button key={desktop.serverId} className={selectedId === desktop.serverId ? 'desktop selected' : 'desktop'} onClick={() => setSelectedId(desktop.serverId)}>
                <span><strong>{desktop.name}</strong><small>{desktop.host}:{desktop.port}</small></span>
                <span className={desktop.pairing ? 'available' : ''}>{desktop.pairing ? 'Ready' : 'Found'}</span>
              </button>
            ))}
          </div>
        )}
        <label className="code-field">
          <span>Matching code</span>
          <input inputMode="numeric" autoComplete="one-time-code" maxLength={8} value={code} onChange={event => setCode(event.target.value.replace(/\D/g, ''))} placeholder="000000" />
        </label>
        <button className="primary-button button-with-icon" disabled={busy || !selectedId || !code} onClick={() => void pairCode()}><Monitor size={18} />Pair selected desktop</button>
      </section>
    </main>
  )
}

function Licenses() {
  const [text, setText] = useState('Loading licenses…')
  useEffect(() => {
    void Promise.all([
      fetch('./licenses/third-party.txt').then(response => response.text()),
      fetch('./licenses/apache-2.0.txt').then(response => response.text()),
      fetch('./licenses/lucide-react.txt').then(response => response.text()),
      fetch('./licenses/react-toastify.txt').then(response => response.text()),
      fetch('./licenses/clsx.txt').then(response => response.text()),
      fetch('./licenses/cascadia-code.txt').then(response => response.text()),
    ]).then(parts => setText(parts.join('\n\n')))
      .catch(() => setText('License file unavailable.'))
  }, [])
  return <main className="page license-page"><pre>{text}</pre></main>
}

export default function App() {
  const [screen, setScreen] = useState<Screen>('dashboard')
  const [settings, setSettings] = useState(initialSettings)
  const [sourceState, setSourceState] = useState<SourceStateEvent>({ state: 'starting' })
  const [titlebarVisible, setTitlebarVisible] = useState(true)
  const [pageMenuOpen, setPageMenuOpen] = useState(false)
  const [reconnecting, setReconnecting] = useState(false)
  const [isLandscape, setIsLandscape] = useState(
    () => window.matchMedia('(orientation: landscape)').matches,
  )
  const titlebarTimer = useRef<number | undefined>(undefined)

  const reportError = useCallback((value: string) => console.error(value), [])

  const revealTitlebar = useCallback(() => {
    setTitlebarVisible(true)
    window.clearTimeout(titlebarTimer.current)
    if (screen === 'dashboard' && isLandscape) {
      titlebarTimer.current = window.setTimeout(() => {
        setPageMenuOpen(false)
        setTitlebarVisible(false)
      }, 3_000)
    }
  }, [isLandscape, screen])

  useEffect(() => {
    const orientation = window.matchMedia('(orientation: landscape)')
    const updateOrientation = () => setIsLandscape(orientation.matches)
    orientation.addEventListener('change', updateOrientation)
    return () => orientation.removeEventListener('change', updateOrientation)
  }, [])

  useEffect(() => {
    window.clearTimeout(titlebarTimer.current)
    setTitlebarVisible(true)
    if (screen === 'dashboard' && isLandscape) {
      titlebarTimer.current = window.setTimeout(() => {
        setPageMenuOpen(false)
        setTitlebarVisible(false)
      }, 3_000)
    }
    return () => window.clearTimeout(titlebarTimer.current)
  }, [isLandscape, screen])

  useEffect(() => {
    if (!pageMenuOpen) return
    const closeMenu = (event: PointerEvent) => {
      if (event.target instanceof Element && !event.target.closest('.page-switcher')) {
        setPageMenuOpen(false)
      }
    }
    document.addEventListener('pointerdown', closeMenu)
    return () => document.removeEventListener('pointerdown', closeMenu)
  }, [pageMenuOpen])

  useEffect(() => {
    let startY: number | undefined
    const onTouchStart = (event: TouchEvent) => {
      const y = event.touches[0]?.clientY
      startY = y !== undefined && y <= 64 ? y : undefined
    }
    const onTouchMove = (event: TouchEvent) => {
      const y = event.touches[0]?.clientY
      if (startY !== undefined && y !== undefined && y - startY >= 32) {
        startY = undefined
        setPageMenuOpen(false)
        revealTitlebar()
      }
    }
    const clearTouch = () => { startY = undefined }
    window.addEventListener('touchstart', onTouchStart, { passive: true })
    window.addEventListener('touchmove', onTouchMove, { passive: true })
    window.addEventListener('touchend', clearTouch, { passive: true })
    window.addEventListener('touchcancel', clearTouch, { passive: true })
    return () => {
      window.removeEventListener('touchstart', onTouchStart)
      window.removeEventListener('touchmove', onTouchMove)
      window.removeEventListener('touchend', clearTouch)
      window.removeEventListener('touchcancel', clearTouch)
    }
  }, [revealTitlebar])

  useEffect(() => {
    let cancelled = false
    let runtimeCleanup = () => {}
    const handles: Array<{ remove: () => Promise<void> }> = []
    const unsubscribe = subscribeSourceState(setSourceState)
    void (async () => {
      try {
        const disposeRuntime = await installTelemetryRuntime()
        if (cancelled) {
          disposeRuntime()
          return
        }
        runtimeCleanup = disposeRuntime
        handles.push(await Telemetry.addListener('settingsChanged', setSettings))
        handles.push(await Telemetry.addListener('recordingExport', result => {
          if (result.error) console.error(result.error)
          else console.info(`${result.movedFiles} recording${result.movedFiles === 1 ? '' : 's'} exported`)
        }))
        const currentSettings = await Telemetry.start()
        if (!cancelled) setSettings(currentSettings)
      } catch (error) {
        if (!cancelled) console.error(error)
      }
    })()
    return () => {
      cancelled = true
      unsubscribe()
      runtimeCleanup()
      for (const handle of handles) void handle.remove()
      void Telemetry.stop()
    }
  }, [])

  const title = screen === 'settings' ? 'Settings' : screen === 'pairing' ? 'Pair desktop' : 'Licenses'
  const showReconnect = settings.source === 'paired' && settings.hasSavedDesktop &&
    (sourceState.state === 'error' || sourceState.state === 'disconnected')

  const reconnect = async () => {
    setReconnecting(true)
    try {
      await Telemetry.reconnect()
    } catch (error) {
      reportError(error instanceof Error ? error.message : String(error))
    } finally {
      setReconnecting(false)
    }
  }

  const navigateBack = useCallback(() => {
    if (pageMenuOpen) {
      setPageMenuOpen(false)
      return
    }
    if (screen === 'dashboard') {
      void Telemetry.exitApp()
      return
    }
    setScreen(screen === 'settings' ? 'dashboard' : 'settings')
  }, [pageMenuOpen, screen])

  useEffect(() => {
    window.addEventListener('tnrBackButton', navigateBack)
    return () => window.removeEventListener('tnrBackButton', navigateBack)
  }, [navigateBack])

  return (
    <div className={`app-shell ${titlebarVisible ? 'titlebar-visible' : ''}`}>
      <header className={`topbar ${screen === 'dashboard' ? 'dashboard-topbar' : ''} ${titlebarVisible ? '' : 'topbar-hidden'}`} onPointerDown={revealTitlebar}>
        {screen === 'dashboard' ? (
          <>
            <div className="page-switcher">
              <button className="page-switcher-button" aria-haspopup="menu" aria-expanded={pageMenuOpen} onClick={() => setPageMenuOpen(open => !open)}>
                <Gauge size={18} aria-hidden="true" />
                <span>Dashboard</span>
                <ChevronDown size={17} className={pageMenuOpen ? 'chevron-open' : ''} aria-hidden="true" />
              </button>
              {pageMenuOpen && (
                <div className="page-menu" role="menu">
                  <button role="menuitem" onClick={() => { setPageMenuOpen(false); setScreen('dashboard') }}>
                    <Gauge size={17} /><span>Dashboard</span><Check size={16} />
                  </button>
                </div>
              )}
            </div>
            <button className="icon-button" aria-label="Settings" onClick={() => setScreen('settings')}><SettingsIcon size={21} /></button>
          </>
        ) : (
          <>
            <button className="icon-button" aria-label="Back" onClick={navigateBack}><ChevronLeft size={25} /></button>
            <h1>{title}</h1>
            <span className="topbar-spacer" />
          </>
        )}
      </header>

      {screen === 'dashboard' && <Dashboard />}
      {screen === 'settings' && <Settings settings={settings} onSettings={setSettings} onNavigate={setScreen} onError={reportError} />}
      {screen === 'pairing' && <Pairing onSettings={setSettings} onDone={() => setScreen('settings')} onError={reportError} />}
      {screen === 'licenses' && <Licenses />}

      {showReconnect && (
        <div className="source-cluster">
          <button className="reconnect-button" disabled={reconnecting} onClick={() => void reconnect()}><RefreshCw size={14} className={reconnecting ? 'spinning' : ''} />Reconnect</button>
        </div>
      )}
    </div>
  )
}
