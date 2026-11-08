import { useCallback, useEffect, useMemo, useState, type ReactNode } from 'react'
import { Check, FolderOpen, Smartphone } from 'lucide-react'
import QRCode from 'qrcode'
import { Row, SegmentedControl, Toggle } from './Settings'
import { StatCard } from './LiveStats'
import { CopyButton, PcAddressList, preferredAddress, useNetworkInterfaces } from './PcAddressList'
import { CheckRow, ConnectionCheck } from './ConnectionCheck'
import { BUTTON_CLASS, PRIMARY_BUTTON_CLASS } from '../lib/buttonStyles'
import { fmtMs, fmtSector } from '../lib/lapTimeFormat'
import { compactPreset, compactPresetOf, type CompactState, type DensityMode } from '../lib/graphSections'
import type { Theme } from '../app/appConfig'
import type {
  FirstTimeSetupState, PairServiceState, SetupNetworkInterface,
} from '../types'
import iconTransparent from '../assets/icon_transparent.png'
import iconTransparentLight from '../assets/icon_transparent_light.png'

type StepKey = 'recordings' | 'density' | 'theme' | 'game' | 'check' | 'pairing' | 'summary'
type StepStatus = 'todo' | 'done' | 'skipped'
type RecordingScope = 'all_drivers' | 'driver_only' | 'both' | 'ask'
type ScopeCategory = 'practice' | 'qualifying' | 'race' | 'timeTrial'

const STEPS: { key: StepKey; title: string; description: string }[] = [
  { key: 'recordings', title: 'Recordings', description: 'Save sessions to replay and analyse later.' },
  { key: 'density', title: 'Density', description: 'How tightly cards and tables are packed.' },
  { key: 'theme', title: 'Theme', description: 'Pick a look. You can switch any time.' },
  { key: 'game', title: 'Connect the game', description: 'Tell the game where to send telemetry.' },
  { key: 'check', title: 'Check connection', description: 'Makes sure telemetry can reach this PC.' },
  { key: 'pairing', title: 'Paired mode', description: 'Show this session on an Android phone or tablet.' },
  { key: 'summary', title: 'Summary', description: 'Here’s what you picked. Change anything before you start.' },
]
const STEP_INDEX = Object.fromEntries(STEPS.map((step, index) => [step.key, index])) as Record<StepKey, number>

const SCOPE_OPTIONS: { value: RecordingScope; label: string }[] = [
  { value: 'all_drivers', label: 'All Drivers' },
  { value: 'driver_only', label: 'Driver Only' },
  { value: 'both', label: 'Both' },
  { value: 'ask', label: 'Ask' },
]
const SCOPE_CATEGORIES: { key: ScopeCategory; label: string }[] = [
  { key: 'practice', label: 'Practice' },
  { key: 'qualifying', label: 'Qualifying' },
  { key: 'race', label: 'Race' },
  { key: 'timeTrial', label: 'Time Trial' },
]
const DENSITIES: { value: DensityMode; label: string }[] = [
  { value: 'compact', label: 'Compact' },
  { value: 'normal', label: 'Normal' },
  { value: 'spacious', label: 'Spacious' },
]
const THEMES: { value: Theme; label: string; hint: string }[] = [
  { value: 'dark', label: 'Dark', hint: 'The default' },
  { value: 'midnight', label: 'Midnight', hint: 'Darker, near black' },
  { value: 'light', label: 'Light', hint: 'For bright rooms' },
]

const PAIR_WINDOW_MS = 2 * 60 * 1000
const INPUT_CLASS = 'bg-[var(--bg-input)] border border-[var(--border-muted)] rounded-lg text-xs text-[var(--text-primary)] px-3 h-8 outline-none focus:border-[var(--border-focus)] transition-colors tabular-nums'
const LINK_CLASS = 'text-[11px] text-[var(--color-info)] hover:underline'

function isIpv4(value: string): boolean {
  const parts = value.trim().split('.')
  return parts.length === 4 && parts.every(part => /^\d{1,3}$/.test(part) && Number(part) <= 255)
}

function formatFree(bytes: number): string {
  const gb = bytes / 1e9
  if (gb >= 1) return `${gb >= 100 ? Math.round(gb) : gb.toFixed(1)} GB free`
  return `${Math.round(bytes / 1e6)} MB free`
}

function driveOf(directory: string): string {
  const match = /^([A-Za-z]:)[\\/]/.exec(directory)
  return match ? match[1] : ''
}

function readScopes(): Record<ScopeCategory, RecordingScope> {
  const read = (category: ScopeCategory): RecordingScope => {
    const value = window.electronStore.get(`logging.scope.${category}`, 'all_drivers')
    return SCOPE_OPTIONS.some(option => option.value === value) ? value as RecordingScope : 'all_drivers'
  }
  return { practice: read('practice'), qualifying: read('qualifying'), race: read('race'), timeTrial: read('timeTrial') }
}

interface Props {
  state: FirstTimeSetupState
  theme: Theme
  onThemeChange: (theme: Theme) => void
  compact: CompactState
  onCompactChange: (compact: CompactState) => void
  onDone: () => void
}

export default function FirstTimeSetup({ state, theme, onThemeChange, compact, onCompactChange, onDone }: Props) {
  const platform = window.platform
  const [stepIndex, setStepIndex] = useState(0)
  const [direction, setDirection] = useState<'forward' | 'back' | null>(null)
  const [statuses, setStatuses] = useState<StepStatus[]>(() => STEPS.map(() => 'todo'))
  const step = STEPS[stepIndex]
  const lastStep = stepIndex === STEPS.length - 1

  // ── Recordings ──
  const [recordEnabled, setRecordEnabled] = useState<boolean>(() => {
    const stored = window.electronStore.get('logging.enabled', null)
    return typeof stored === 'boolean' ? stored : true
  })
  const [recordDirectory, setRecordDirectory] = useState<string>(() =>
    (window.electronStore.get('logging.directory', '') as string) || state.defaultRecordingDirectory)
  const [scopes, setScopes] = useState(readScopes)
  const [freeBytes, setFreeBytes] = useState<number | null>(null)

  // ── Density and theme ──
  const [density, setDensity] = useState<DensityMode>(() => compactPresetOf(compact) ?? 'normal')
  const [themeAtStepStart, setThemeAtStepStart] = useState(theme)

  // ── Network ──
  const interfaces = useNetworkInterfaces()
  const [selectedAddress, setSelectedAddress] = useState<string>('')
  const [port, setPort] = useState<string>(() => String(window.electronStore.get('udp.port', 20777)))
  const [bindAddress, setBindAddress] = useState<string>(() => window.electronStore.get('udp.bindAddress', '0.0.0.0') as string)
  const portNumber = Number(port)
  const portValid = Number.isInteger(portNumber) && portNumber >= 1 && portNumber <= 65535
  const bindValid = bindAddress.trim() === '' || isIpv4(bindAddress)

  const stepValid = step.key !== 'game' || (portValid && bindValid)

  useEffect(() => {
    if (interfaces.length > 0) setSelectedAddress(address => address || preferredAddress(interfaces))
  }, [interfaces])

  useEffect(() => {
    let current = true
    setFreeBytes(null)
    if (recordDirectory) void window.setupBridge.freeBytes(recordDirectory).then(bytes => { if (current) setFreeBytes(bytes) })
    return () => { current = false }
  }, [recordDirectory])

  // ── Step commits: Continue (and moving away with the dots) saves a step's
  //    choices; Skip leaves the saved settings as they were. ──
  const commitStep = useCallback(async (key: StepKey) => {
    switch (key) {
      case 'recordings': {
        if (recordEnabled) await window.setupBridge.ensureDirectory(recordDirectory)
        for (const { key: category } of SCOPE_CATEGORIES) window.electronStore.set(`logging.scope.${category}`, scopes[category])
        window.electronStore.set('logging.directory', recordDirectory)
        window.electronStore.set('logging.enabled', recordEnabled)
        break
      }
      case 'density':
        if (compactPresetOf(compact) !== density) onCompactChange(compactPreset(compact, density))
        break
      case 'game': {
        const nextBind = bindAddress.trim() || '0.0.0.0'
        const changed = portNumber !== Number(window.electronStore.get('udp.port', 20777)) ||
          nextBind !== window.electronStore.get('udp.bindAddress', '0.0.0.0')
        if (changed && portValid && bindValid) {
          window.electronStore.set('udp.port', portNumber)
          window.electronStore.set('udp.bindAddress', nextBind)
          await window.udpBridge.restart()
        }
        break
      }
      default:
        break
    }
  }, [bindAddress, bindValid, compact, density, onCompactChange, portNumber, portValid, recordDirectory, recordEnabled, scopes])

  const goTo = useCallback((index: number, currentTheme: Theme = theme) => {
    const target = Math.max(0, Math.min(STEPS.length - 1, index))
    setThemeAtStepStart(currentTheme)
    setDirection(target >= stepIndex ? 'forward' : 'back')
    setStepIndex(target)
  }, [stepIndex, theme])

  const leaveStep = useCallback(async (target: number, outcome: 'done' | 'skipped') => {
    let nextTheme = theme
    if (outcome === 'done') {
      await commitStep(step.key)
    } else if (step.key === 'theme' && themeAtStepStart !== theme) {
      // Skipping the theme step puts back the theme it started with.
      nextTheme = themeAtStepStart
      onThemeChange(nextTheme)
    }
    if (step.key !== 'summary') setStatuses(current => current.map((value, index) => index === stepIndex ? outcome : value))
    goTo(target, nextTheme)
  }, [commitStep, goTo, onThemeChange, step.key, stepIndex, theme, themeAtStepStart])

  const finish = useCallback(async () => {
    await window.setupBridge.complete()
    onDone()
  }, [onDone])

  const headerPadding = platform === 'darwin'
    ? { paddingLeft: 84 }
    : {
        paddingLeft: 'calc(env(titlebar-area-x, 0px) + 1rem)',
        paddingRight: 'calc(100% - env(titlebar-area-x, 0px) - env(titlebar-area-width, 100%) + 1rem)',
      }
  const nativeTitlebar = window.electronStore.get('nativeTitlebar', false) === true
  const animationClass = direction === 'forward' ? 'setup-step-in-forward' : direction === 'back' ? 'setup-step-in-back' : ''

  return (
    <div className="fixed inset-0 z-[200] flex flex-col bg-[var(--bg-panel)] text-[var(--text-primary)]" role="dialog" aria-modal="true" aria-label="First-time setup">
      <header
        className="flex h-10 shrink-0 items-center gap-2 select-none"
        style={{ WebkitAppRegion: nativeTitlebar ? 'no-drag' : 'drag', ...(nativeTitlebar ? { paddingLeft: '1rem' } : headerPadding) } as React.CSSProperties}
      >
        <img src={theme === 'light' ? iconTransparentLight : iconTransparent} alt="" className="h-[18px] w-auto pointer-events-none opacity-90" draggable="false" />
        <span className="text-xs text-[var(--text-secondary)]">Track N Race</span>
      </header>

      <main className="min-h-0 flex-1 overflow-y-auto overflow-x-hidden">
        <div key={stepIndex} className={`mx-auto w-[560px] max-w-full pb-6 pt-[72px] ${animationClass}`}>
          <h1 className="text-[22px] font-semibold leading-tight">{step.title}</h1>
          <p className="mb-6 mt-1.5 text-[12.5px] text-[var(--text-secondary)]">{step.description}</p>
          {step.key === 'recordings' && (
            <RecordingsStep
              enabled={recordEnabled} onEnabledChange={setRecordEnabled}
              directory={recordDirectory} onDirectoryChange={setRecordDirectory}
              freeBytes={freeBytes} scopes={scopes}
              onScopeChange={(category, scope) => setScopes(current => ({ ...current, [category]: scope }))}
            />
          )}
          {step.key === 'density' && <DensityStep value={density} onChange={setDensity} />}
          {step.key === 'theme' && <ThemeStep value={theme} onChange={onThemeChange} />}
          {step.key === 'game' && (
            <GameStep
              interfaces={interfaces} selectedAddress={selectedAddress} onSelectAddress={setSelectedAddress}
              port={port} onPortChange={setPort} portValid={portValid}
              bindAddress={bindAddress} onBindAddressChange={setBindAddress} bindValid={bindValid}
            />
          )}
          {step.key === 'check' && (
            <CheckStep
              platform={platform} port={portValid ? portNumber : 20777}
              selectedAddress={selectedAddress} interfaces={interfaces}
              onChangePort={() => void leaveStep(STEP_INDEX.game, 'done')}
            />
          )}
          {step.key === 'pairing' && <PairingStep platform={platform} />}
          {step.key === 'summary' && (
            <SummaryStep
              statuses={statuses} onChange={index => goTo(index)}
              recordEnabled={recordEnabled} recordDirectory={recordDirectory}
              density={density} savedDensity={compactPresetOf(compact)} theme={theme}
              address={selectedAddress} port={portValid ? portNumber : 20777}
            />
          )}
        </div>
      </main>

      <footer className="shrink-0 border-t border-[var(--border)]">
        <div className="grid grid-cols-[1fr_auto_1fr] items-center gap-2 px-4 py-3">
          <span className="flex">
            {stepIndex > 0 && (
              <button type="button" className={BUTTON_CLASS} onClick={() => void leaveStep(stepIndex - 1, 'done')}>Back</button>
            )}
          </span>
          {/* The step count hangs off the right so the dots themselves sit at the centre. */}
          <nav aria-label="Setup steps" className="relative flex items-center gap-1.5">
            {STEPS.map((item, index) => {
              const current = index === stepIndex
              return (
                <button
                  key={item.key}
                  type="button"
                  aria-label={item.title}
                  aria-current={current ? 'step' : undefined}
                  onClick={() => { if (!current) void leaveStep(index, 'done') }}
                  className={`h-1.5 rounded-full transition-[width,background-color] duration-200 ${
                    current ? 'w-[18px] bg-[var(--color-info)]'
                      : statuses[index] !== 'todo' ? 'w-1.5 bg-[var(--text-muted)]'
                      : 'w-1.5 bg-[var(--border)]'
                  }`}
                />
              )
            })}
            <span className="absolute left-full ml-2.5 whitespace-nowrap text-[11px] text-[var(--text-muted)]">{stepIndex + 1} of {STEPS.length}</span>
          </nav>
          <span className="flex items-center justify-end gap-2">
            {!lastStep && (
              <button type="button" className={BUTTON_CLASS} onClick={() => void leaveStep(stepIndex + 1, 'skipped')}>Skip</button>
            )}
            {lastStep ? (
              <button type="button" className={PRIMARY_BUTTON_CLASS} onClick={() => void finish()}>Start using Track N Race</button>
            ) : (
              <button type="button" className={PRIMARY_BUTTON_CLASS} disabled={!stepValid} onClick={() => void leaveStep(stepIndex + 1, 'done')}>Continue</button>
            )}
          </span>
        </div>
      </footer>
    </div>
  )
}

// ── Shared pieces ────────────────────────────────────────────────────────────

function GroupLabel({ children, className = '' }: { children: ReactNode; className?: string }) {
  return <p className={`mb-1.5 text-[10px] uppercase tracking-[0.2em] text-[var(--text-muted)] ${className}`}>{children}</p>
}

function Hint({ children, className = 'mt-[22px]' }: { children: ReactNode; className?: string }) {
  return <p className={`text-xs leading-relaxed text-[var(--text-muted)] ${className}`}>{children}</p>
}


// ── Steps ────────────────────────────────────────────────────────────────────

function RecordingsStep({ enabled, onEnabledChange, directory, onDirectoryChange, freeBytes, scopes, onScopeChange }: {
  enabled: boolean; onEnabledChange: (value: boolean) => void
  directory: string; onDirectoryChange: (value: string) => void
  freeBytes: number | null
  scopes: Record<ScopeCategory, RecordingScope>; onScopeChange: (category: ScopeCategory, scope: RecordingScope) => void
}) {
  const drive = driveOf(directory)
  return (
    <div className="-mx-4 flex flex-col">
      <Row label="Record Session Data" description="Save live telemetry as .tnrd files.">
        <Toggle value={enabled} onChange={onEnabledChange} />
      </Row>
      <div className={`transition-opacity ${enabled ? '' : 'pointer-events-none select-none opacity-35'}`} aria-disabled={!enabled}>
        <Row
          label="Output Directory"
          description={freeBytes === null ? 'Where recordings are saved.' : <>Where recordings are saved. <span className="text-[var(--text-secondary)]">{formatFree(freeBytes)}</span>{drive ? ` on ${drive}` : ''}</>}
        >
          <div className="flex items-center gap-2">
            <input readOnly value={directory} title={directory} aria-label="Output directory" className={`${INPUT_CLASS} w-48 truncate text-[var(--text-secondary)] opacity-80`} />
            <button
              type="button"
              className={BUTTON_CLASS}
              onClick={() => void window.fsBridge.selectDirectory().then(selected => { if (selected) onDirectoryChange(selected) })}
            >
              <FolderOpen size={12} /> Browse
            </button>
          </div>
        </Row>
        {SCOPE_CATEGORIES.map(({ key, label }) => (
          <Row key={key} label={`${label} Sessions`} description="">
            <SegmentedControl<RecordingScope> options={SCOPE_OPTIONS} value={scopes[key]} onChange={scope => onScopeChange(key, scope)} />
          </Row>
        ))}
      </div>
    </div>
  )
}

// Sample laps for the density previews: 24 laps, an out-lap and a pit lap,
// soft then medium. Fixed values so every preview shows the same rows.
const SAMPLE_LAPS = (() => {
  let seed = 11
  const random = () => (seed = (seed * 9301 + 49297) % 233280) / 233280
  const times = Array.from({ length: 24 }, (_, index) => {
    const lap = index + 1
    const seconds = 91.4 + random() * 1.2 - (lap > 14 ? 0.4 : 0) + (lap === 1 ? 4.8 : 0) + (lap === 14 ? 21.6 : 0)
    return Math.round(seconds * 1000)
  })
  const best = Math.min(...times)
  return times.map((ms, index) => ({ lap: index + 1, ms, delta: ms - best }))
})()

const PREVIEW_HEIGHT = 320
const LAP_TABLE_CLASS: Record<DensityMode, { row: string; cell: string; head: string }> = {
  compact: { row: 'h-[22px]', cell: 'px-2 text-[11px]', head: 'px-2 text-[9px]' },
  normal: { row: 'h-[30px]', cell: 'px-2.5 text-xs', head: 'px-2.5 text-[10px]' },
  spacious: { row: 'h-[42px]', cell: 'px-3.5 text-sm', head: 'px-3.5 text-[11px]' },
}

function SampleLapTable({ density }: { density: DensityMode }) {
  const style = LAP_TABLE_CLASS[density]
  return (
    <table className="w-full border-collapse tabular-nums">
      <thead>
        <tr className={style.row}>
          {['Lap', 'Time', 'Δ'].map((label, index) => (
            <th key={label} className={`${style.head} border-b border-[var(--border)] font-semibold uppercase tracking-[0.1em] text-[var(--text-muted)] ${index === 2 ? 'text-right' : 'text-left'}`}>{label}</th>
          ))}
        </tr>
      </thead>
      <tbody>
        {SAMPLE_LAPS.map(lap => {
          const best = lap.delta === 0
          return (
            <tr key={lap.lap} className={style.row}>
              <td className={`${style.cell} border-b border-[var(--border-subtle)] text-[var(--text-muted)]`}>{lap.lap}</td>
              <td className={`${style.cell} border-b border-[var(--border-subtle)] font-semibold ${best ? 'text-[var(--color-fastest)]' : 'text-[var(--text-primary)]'}`}>{fmtMs(lap.ms)}</td>
              <td className={`${style.cell} border-b border-[var(--border-subtle)] text-right ${best ? 'text-[var(--color-fastest)]' : 'text-[var(--text-secondary)]'}`}>{best ? 'Best' : `+${fmtSector(lap.delta)}`}</td>
            </tr>
          )
        })}
      </tbody>
    </table>
  )
}

function DensityStep({ value, onChange }: { value: DensityMode; onChange: (value: DensityMode) => void }) {
  return (
    <div role="radiogroup" aria-label="Density" className="-mx-[160px] mt-1 grid w-[880px] grid-cols-3 gap-4">
      {DENSITIES.map(option => {
        const selected = value === option.value
        return (
          <button
            key={option.value}
            type="button"
            role="radio"
            aria-checked={selected}
            onClick={() => onChange(option.value)}
            className="group flex min-w-0 flex-col gap-2.5 text-left"
          >
            <div
              aria-hidden="true"
              style={{ height: PREVIEW_HEIGHT }}
              className={`relative w-full overflow-hidden rounded-lg bg-[var(--bg-base)] transition-shadow ${
                selected ? 'shadow-[0_0_0_2px_var(--color-info)]' : 'shadow-[0_0_0_1px_var(--border)] group-hover:shadow-[0_0_0_1px_var(--border-focus-hover)]'
              }`}
            >
              <div className="flex divide-x divide-[var(--border)] border-b border-[var(--border)]">
                <StatCard label="Speed" value="287" unit="kph" compact={option.value} />
                <StatCard label="Gear" value="7" compact={option.value} />
                <StatCard label="Pos" value="P3" sub="Lap 12" compact={option.value} />
              </div>
              <SampleLapTable density={option.value} />
              <div className="pointer-events-none absolute inset-x-0 bottom-0 h-10 bg-gradient-to-b from-transparent to-[var(--bg-base)]" />
            </div>
            <span className={`flex items-center justify-between text-[13px] font-semibold ${selected ? 'text-[var(--text-primary)]' : 'text-[var(--text-secondary)]'}`}>
              {option.label}
              {selected && <Check size={15} className="text-[var(--color-info)]" />}
            </span>
          </button>
        )
      })}
    </div>
  )
}

// ── Overview skeleton for the theme previews ─────────────────────────────────

const SK_SPEED = '0,8 20,9 30,22 36,48 42,40 50,22 62,15 72,14 80,40 84,46 92,32 104,22 118,22 124,32 140,32 148,52 156,38 166,48 170,40 178,22 190,15 196,11'
const SK_RPM = '0,18 18,18 22,30 24,26 26,34 28,22 30,20 32,32 34,36 36,26 38,24 42,18 50,16 60,14 70,13 72,24 74,28 76,22 78,32 80,22 84,18 86,26 88,24 96,20 104,18 112,18 114,24 116,28 118,22 120,24 126,22 132,22 134,28 136,30 138,24 140,24 142,34 146,38 148,30 150,26 152,32 154,22 158,30 160,24 162,20 168,16 170,26 172,30 174,24 176,22 180,18 186,16 188,22 190,20 196,19'
const SK_ERS = '0,60 10,60 18,46 24,44 30,52 36,60 60,60 70,58 76,46 80,46 86,60 100,60 104,54 114,54 118,40 124,36 130,36 136,26 146,26 150,10 154,8 158,8 166,20 176,34 186,42 196,46'
const SK_STAT_COLORS = ['green', 'red', 'red', 'green', 'text', 'green', 'red', 'red', 'green', 'text', 'red']
const SK_TYRE_COLORS = ['red', 'blue', 'green', 'yellow']

// Surface temp, inner temp, brake temp and wear traces, one per corner.
const SK_TYRE_LINES = [0, 1, 2, 3].map(kind => SK_TYRE_COLORS.map((_, corner) =>
  Array.from({ length: 26 }, (_, i) => {
    let y: number
    if (kind === 0) y = 24 - Math.sin(i * 0.9) * 5 - Math.sin(i * 2.3) * 1.5 - (i > 10 && i < 13 && corner === 0 ? 10 : 0) + corner * 1.4
    else if (kind === 1) y = 10 + corner * 0.6
    else if (kind === 2) y = 32 - ((i % 6) === 1 ? 26 : 26 - (i % 6) * 4.5) + corner * 1.1
    else y = 36 - corner * 0.4 - i * 0.03
    return `${i * 4},${Math.max(1, Math.min(39, y)).toFixed(1)}`
  }).join(' ')))

function SkGrid({ width, height }: { width: number; height: number }) {
  return (
    <>
      {[1, 2, 3].map(i => <line key={`h${i}`} x1={0} x2={width} y1={(height / 4) * i} y2={(height / 4) * i} stroke="var(--border-subtle)" strokeWidth={1} vectorEffect="non-scaling-stroke" />)}
      {[0.29, 0.7].map(f => <line key={`v${f}`} x1={width * f} x2={width * f} y1={0} y2={height} stroke="var(--border)" strokeWidth={1} vectorEffect="non-scaling-stroke" />)}
    </>
  )
}

function SkLine({ points, color }: { points: string; color: string }) {
  return <polyline points={points} fill="none" stroke={`var(--sk-${color})`} strokeWidth={1} strokeLinejoin="round" vectorEffect="non-scaling-stroke" />
}

function OverviewSkeleton({ theme }: { theme: Theme }) {
  return (
    <span className="setup-sk" data-theme={theme} aria-hidden="true">
      <span className="setup-sk-tb">
        <i className="logo" /><i className="sq" /><i className="tp" style={{ width: 16 }} /><i className="t" style={{ width: 22 }} /><i className="pill" />
        <span className="g" />
        <i className="tp" style={{ width: 9, height: 3 }} /><i className="tp" style={{ width: 12 }} /><i className="sq" /><i className="act" /><i className="sq" /><i className="sq" /><i className="sq" />
        <span style={{ width: 6 }} /><i className="t" style={{ width: 4 }} /><i className="sq" /><i className="t" style={{ width: 4 }} />
      </span>
      <span className="setup-sk-stats">
        {SK_STAT_COLORS.map((color, i) => <span key={i}><i className="t" /><i className="v" style={{ background: color === 'text' ? 'var(--text-primary)' : `var(--sk-${color})` }} /></span>)}
      </span>
      <span className="setup-sk-chart">
        <span className="setup-sk-hd"><i className="t" style={{ width: '16%' }} /><i className="tp" style={{ width: '7%' }} /><span className="g" /><i className="lg" style={{ background: 'var(--sk-green)' }} /><i className="lg" style={{ background: 'var(--sk-red)' }} /><i className="lg" style={{ background: 'var(--sk-yellow)' }} /></span>
        <svg viewBox="0 0 200 62" preserveAspectRatio="none"><SkGrid width={200} height={62} /><SkLine points={SK_ERS} color="yellow" /><SkLine points={SK_SPEED} color="green" /><SkLine points={SK_RPM} color="red" /></svg>
      </span>
      <span className="setup-sk-tyres">
        {SK_TYRE_LINES.map((lines, kind) => (
          <span key={kind} className="setup-sk-tp">
            <span className="setup-sk-hd"><i className="t" style={{ width: '22%' }} /><i className="tp" style={{ width: '12%' }} /><span className="g" />{SK_TYRE_COLORS.map(color => <i key={color} className="lg" style={{ width: 4, background: `var(--sk-${color})` }} />)}</span>
            <svg viewBox="0 0 100 40" preserveAspectRatio="none"><SkGrid width={100} height={40} />{lines.map((points, corner) => <SkLine key={corner} points={points} color={SK_TYRE_COLORS[corner]} />)}</svg>
          </span>
        ))}
      </span>
      <span className="setup-sk-dmg">{Array.from({ length: 8 }, (_, i) => <span key={i}><i className="t" /><i className="v" /></span>)}</span>
      <span className="setup-sk-play"><i className="pa" /><i className="pa" /><i className="pa" /><i className="t" style={{ width: 12 }} /><span className="tr" /><i className="t" style={{ width: 14 }} /><i className="t" style={{ width: 5 }} /><i className="t" style={{ width: 4 }} /><i className="pa" /></span>
    </span>
  )
}

function ThemeStep({ value, onChange }: { value: Theme; onChange: (value: Theme) => void }) {
  return (
    <>
      <div role="radiogroup" aria-label="Theme" className="-mx-[160px] grid w-[880px] grid-cols-3 gap-4">
        {THEMES.map(option => {
          const selected = value === option.value
          return (
            <button
              key={option.value}
              type="button"
              role="radio"
              aria-checked={selected}
              data-selected={selected}
              onClick={() => onChange(option.value)}
              className="setup-theme-option group flex min-w-0 flex-col gap-2.5 text-left"
            >
              <OverviewSkeleton theme={option.value} />
              <span className={`flex items-center justify-between text-[13px] font-semibold ${selected ? 'text-[var(--text-primary)]' : 'text-[var(--text-secondary)]'}`}>
                {option.label}
                {selected && <Check size={15} className="text-[var(--color-info)]" />}
              </span>
              <span className="-mt-1.5 text-[11px] text-[var(--text-muted)]">{option.hint}</span>
            </button>
          )
        })}
      </div>
    </>
  )
}

// The game decides which UDP Format years it offers; Track N Race reads 2024,
// 2025 and 2026, so recommend the newest one each game has that we support.
type GameTitle = 'f1_25' | 'f1_24'
const GAME_OPTIONS: { value: GameTitle; label: string }[] = [
  { value: 'f1_25', label: 'F1 25 + 2026 Season Pack' },
  { value: 'f1_24', label: 'F1 24' },
]
const GAME_FORMATS: Record<GameTitle, { value: string; note: string }> = {
  f1_25: { value: '2026', note: '2024 and 2025 also work' },
  f1_24: { value: '2024', note: '2022 and 2023 aren’t supported' },
}

function GameStep({
  interfaces, selectedAddress, onSelectAddress,
  port, onPortChange, portValid, bindAddress, onBindAddressChange, bindValid,
}: {
  interfaces: SetupNetworkInterface[]; selectedAddress: string; onSelectAddress: (address: string) => void
  port: string; onPortChange: (value: string) => void; portValid: boolean
  bindAddress: string; onBindAddressChange: (value: string) => void; bindValid: boolean
}) {
  const [showAdvanced, setShowAdvanced] = useState(() => !portValid || !bindValid)
  const [game, setGame] = useState<GameTitle>('f1_25')
  const format = GAME_FORMATS[game]
  const settings: [string, string, boolean?, string?][] = [
    ['UDP Telemetry', 'On'],
    ['UDP Broadcast Mode', 'Off'],
    ['UDP IP Address', selectedAddress || '—', true],
    ['UDP Port', portValid ? port : '—', true],
    ['UDP Send Rate', '60 Hz'],
    ['UDP Format', format.value, false, format.note],
  ]
  return (
    <>
      <GroupLabel>This PC’s address</GroupLabel>
      <div className="mb-6">
        <PcAddressList interfaces={interfaces} selectedAddress={selectedAddress} onSelectAddress={onSelectAddress} />
      </div>

      <div className="-mx-4">
        <Row label="Your game" description="Picks the UDP Format to use.">
          <SegmentedControl<GameTitle> options={GAME_OPTIONS} value={game} onChange={setGame} />
        </Row>
      </div>

      <GroupLabel className="mt-2">In the game ▸ Settings ▸ Telemetry Settings</GroupLabel>
      <div>
        {settings.map(([label, value, copyable, note]) => (
          <div key={label} className="grid grid-cols-[minmax(0,1fr)_auto_64px] items-center gap-4 border-t border-[var(--border-subtle)] py-2 text-xs first:border-t-0">
            <span className="text-[var(--text-secondary)]">{label}</span>
            <span className="text-right">
              <b className={`font-semibold tabular-nums ${copyable ? 'text-[var(--color-info)]' : ''}`}>{value}</b>
              {note && <span className="mt-0.5 block text-[10.5px] text-[var(--text-muted)]">{note}</span>}
            </span>
            {copyable && value !== '—' ? <CopyButton value={value} /> : <span />}
          </div>
        ))}
      </div>

      <button type="button" className={`${LINK_CLASS} mt-3`} onClick={() => setShowAdvanced(open => !open)}>
        {showAdvanced ? 'Hide advanced' : 'Advanced'}
      </button>
      {showAdvanced && (
        <div className="-mx-4">
          <Row label="UDP Port" description="Must match the game.">
            <input
              type="number" min={1} max={65535} value={port} aria-label="UDP port"
              onChange={event => onPortChange(event.target.value)}
              className={`${INPUT_CLASS} w-28 ${portValid ? '' : 'border-red-600/60'}`}
            />
          </Row>
          <Row label="Bind Address" description="0.0.0.0 listens on every network.">
            <input
              value={bindAddress} placeholder="0.0.0.0" aria-label="Bind address"
              onChange={event => onBindAddressChange(event.target.value)}
              className={`${INPUT_CLASS} w-36 ${bindValid ? '' : 'border-red-600/60'}`}
            />
          </Row>
        </div>
      )}
    </>
  )
}

function CheckStep({ platform, port, selectedAddress, interfaces, onChangePort }: {
  platform: string; port: number; selectedAddress: string; interfaces: SetupNetworkInterface[]; onChangePort: () => void
}) {
  return (
    <>
      <ConnectionCheck platform={platform} port={port} selectedAddress={selectedAddress} interfaces={interfaces} onChangePort={onChangePort} />
      <Hint className="mt-3.5">You can continue without a signal. The header shows the connection status later too.</Hint>
    </>
  )
}

function PairingCountdown({ expiresAt }: { expiresAt: number }) {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    const interval = window.setInterval(() => setNow(Date.now()), 1000)
    return () => window.clearInterval(interval)
  }, [])
  const remaining = Math.max(0, expiresAt - now)
  const seconds = Math.ceil(remaining / 1000)
  return (
    <>
      <div className="mt-4 h-0.5 w-full overflow-hidden rounded-full bg-[var(--border-subtle)]">
        <div className="h-full bg-[var(--color-info)] transition-[width] duration-1000 ease-linear" style={{ width: `${Math.min(100, (remaining / PAIR_WINDOW_MS) * 100)}%` }} />
      </div>
      <span className="text-[11px] tabular-nums text-[var(--text-muted)]">Open for {Math.floor(seconds / 60)}:{String(seconds % 60).padStart(2, '0')}</span>
    </>
  )
}

function PairingStep({ platform }: { platform: string }) {
  const [pairing, setPairing] = useState<PairServiceState | null>(null)
  const [qr, setQr] = useState<string | null>(null)

  useEffect(() => {
    if (platform === 'darwin') return undefined
    void window.pairingBridge.getState().then(setPairing)
    return window.pairingBridge.onState(setPairing)
  }, [platform])

  useEffect(() => {
    let current = true
    if (!pairing?.qrPayload) {
      setQr(null)
      return () => { current = false }
    }
    void QRCode.toDataURL(pairing.qrPayload, { width: 264, margin: 1, color: { dark: '#111827', light: '#ffffff' } })
      .then(value => { if (current) setQr(value) })
    return () => { current = false }
  }, [pairing?.qrPayload])

  if (platform === 'darwin') {
    return (
      <CheckRow tone="warn" title="Paired mode is off on macOS" detail="Turn it on later in Settings ▸ Paired Devices." />
    )
  }

  const enabled = pairing?.enabled ?? false
  let body: ReactNode = null
  if (enabled && pairing) {
    if (pairing.pendingDevice) {
      body = (
        <>
          <GroupLabel className="mt-3.5">Pair a device</GroupLabel>
          <div className="flex items-center justify-between gap-4 py-2">
            <div className="min-w-0">
              <p className="text-sm font-semibold">Allow {pairing.pendingDevice.name}?</p>
              <p className="mt-1 text-xs text-[var(--text-muted)]">This phone entered the pairing code. Only allow a phone you recognise.</p>
            </div>
            <div className="flex shrink-0 gap-2">
              <button type="button" className={BUTTON_CLASS} onClick={() => void window.pairingBridge.respond(false).then(setPairing)}>Deny</button>
              <button type="button" className={BUTTON_CLASS} onClick={() => void window.pairingBridge.respond(true).then(setPairing)}>Allow</button>
            </div>
          </div>
        </>
      )
    } else if (pairing.pairingOpen) {
      body = (
        <>
          <GroupLabel className="mt-3.5">Pair a device</GroupLabel>
          <div className="grid grid-cols-[132px_minmax(0,1fr)] items-start gap-7 pt-1.5">
            {qr ? <img src={qr} alt="Pairing QR code" className="h-[132px] w-[132px] rounded bg-white p-1.5" /> : <div className="h-[132px] w-[132px] rounded bg-white" />}
            <div className="min-w-0">
              <p className="text-[11px] text-[var(--text-muted)]">Matching code</p>
              <p className="mt-0.5 text-[32px] font-bold tabular-nums tracking-[0.16em]">
                {pairing.matchingCode ? <>{pairing.matchingCode.slice(0, 4)}<span className="text-[var(--text-muted)]"> </span>{pairing.matchingCode.slice(4)}</> : ''}
              </p>
              <p className="mt-2.5 text-xs leading-relaxed text-[var(--text-muted)]">Scan the QR in Android Settings, or pick this PC on the phone and enter the code.</p>
              <PairingCountdown expiresAt={pairing.pairingExpiresAt} />
              <div className="mt-2 flex justify-end">
                <button type="button" className={LINK_CLASS} onClick={() => void window.pairingBridge.closeWindow().then(setPairing)}>Cancel pairing</button>
              </div>
            </div>
          </div>
        </>
      )
    } else {
      body = (
        <>
          <GroupLabel className="mt-3.5">Devices</GroupLabel>
          <div className="flex items-center justify-between gap-4 py-2 text-xs">
            <span className={`min-w-0 truncate ${pairing.devices.length ? 'text-[var(--text-primary)]' : 'text-[var(--text-muted)]'}`}>
              {pairing.devices.length ? pairing.devices.map(device => device.name).join(', ') : 'No devices paired yet.'}
            </span>
            <button type="button" className={BUTTON_CLASS} onClick={() => void window.pairingBridge.openWindow().then(setPairing)}>
              <Smartphone size={12} /> Pair a device
            </button>
          </div>
          <Hint className="mt-1.5">Opens discovery, QR, and matching-code pairing for two minutes.</Hint>
        </>
      )
    }
  }

  return (
    <>
      <div className="-mx-4">
        <Row label="Enable Paired Mode" description="Let Android displays find this PC on the local network.">
          <Toggle value={enabled} onChange={value => void window.pairingBridge.setEnabled(value).then(setPairing)} />
        </Row>
      </div>
      {body}
      {!enabled && <Hint className="mt-1">Optional. You can pair later in Settings ▸ Paired Devices.</Hint>}
      {pairing?.error && <p className="mt-3 text-xs text-red-400">{pairing.error}</p>}
    </>
  )
}

function SummaryStep({ statuses, onChange, recordEnabled, recordDirectory, density, savedDensity, theme, address, port }: {
  statuses: StepStatus[]; onChange: (index: number) => void
  recordEnabled: boolean; recordDirectory: string
  density: DensityMode; savedDensity: DensityMode | null; theme: Theme; address: string; port: number
}) {
  const [pairEnabled, setPairEnabled] = useState<boolean | null>(null)
  useEffect(() => {
    if (window.platform === 'darwin') return
    void window.pairingBridge.getState().then(state => setPairEnabled(state.enabled))
  }, [])

  const savedRecording = window.electronStore.get('logging.enabled', false) === true
  const savedDirectory = window.electronStore.get('logging.directory', '') as string
  const savedPort = Number(window.electronStore.get('udp.port', 20777))
  const densityLabel = (mode: DensityMode | null) => DENSITIES.find(option => option.value === mode)?.label ?? 'Mixed'
  const densitySkipped = statuses[STEP_INDEX.density] === 'skipped'
  const gameSkipped = statuses[STEP_INDEX.game] === 'skipped'

  const rows = useMemo<{ step: StepKey; label: string; value: string; detail?: string }[]>(() => [
    statuses[STEP_INDEX.recordings] === 'skipped'
      ? { step: 'recordings', label: 'Recordings', value: savedRecording ? 'On' : 'Off', detail: savedRecording ? savedDirectory : undefined }
      : { step: 'recordings', label: 'Recordings', value: recordEnabled ? 'On' : 'Off', detail: recordEnabled ? recordDirectory : 'Sessions won’t be saved' },
    { step: 'density', label: 'Density', value: densityLabel(densitySkipped ? savedDensity : density), detail: densitySkipped ? undefined : 'Every section' },
    { step: 'theme', label: 'Theme', value: THEMES.find(option => option.value === theme)?.label ?? '' },
    { step: 'game', label: 'Game address', value: `${address || '—'} : ${gameSkipped ? savedPort : port}` },
    { step: 'pairing', label: 'Paired mode', value: window.platform === 'darwin' ? 'Off on macOS' : pairEnabled ? 'On' : 'Off' },
  ], [address, density, densitySkipped, gameSkipped, pairEnabled, port, recordDirectory, recordEnabled, savedDensity, savedDirectory, savedPort, savedRecording, statuses, theme])

  return (
    <div className="flex flex-col">
      {rows.map((row, position) => {
        const index = STEP_INDEX[row.step]
        const skipped = statuses[index] === 'skipped'
        return (
          <div key={row.step} className="grid grid-cols-[26px_150px_minmax(0,1fr)_auto] items-center gap-3.5 border-t border-[var(--border-subtle)] py-[11px] first:border-t-0">
            <span className="text-[10.5px] tabular-nums text-[var(--text-muted)]">{String(position + 1).padStart(2, '0')}</span>
            <span className="text-xs text-[var(--text-secondary)]">{row.label}</span>
            <span className={`min-w-0 truncate text-[13px] ${skipped ? 'text-[var(--text-muted)]' : 'font-semibold'}`} title={row.detail}>
              {skipped ? `Skipped · kept ${row.value}` : row.value}
              {row.detail && !skipped && <small className="ml-2 text-[11px] font-normal text-[var(--text-muted)]">{row.detail}</small>}
            </span>
            <button type="button" className={LINK_CLASS} onClick={() => onChange(index)}>Change</button>
          </div>
        )
      })}
    </div>
  )
}
