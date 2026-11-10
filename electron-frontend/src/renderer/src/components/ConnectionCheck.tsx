import { useCallback, useEffect, useState, type ReactNode } from 'react'
import { CircleCheck, CircleX, Copy, Info, LoaderCircle, Shield, TriangleAlert } from 'lucide-react'
import { BUTTON_CLASS } from '../lib/buttonStyles'
import type { SetupFirewallStatus, SetupNetworkInterface, SetupUdpProbe } from '../types'

// The connection check: firewall, network, port and incoming telemetry, as a
// plain status list. Shown by first-time setup's Check connection step and by
// Settings ▸ Network ▸ Check connection.

const NO_TELEMETRY_HINT_MS = 30_000

function copyText(text: string): void {
  void navigator.clipboard?.writeText(text).catch(() => {})
}

export type CheckTone = 'ok' | 'warn' | 'bad' | 'wait' | 'info'
const TONE_TEXT: Record<CheckTone, string> = {
  ok: 'text-green-400',
  warn: 'text-amber-500',
  bad: 'text-red-400',
  wait: 'text-[var(--text-secondary)]',
  info: 'text-[var(--text-secondary)]',
}

export function CheckRow({ tone, title, detail, status, action, children }: {
  tone: CheckTone; title: string; detail: ReactNode; status?: string; action?: ReactNode; children?: ReactNode
}) {
  const Icon = tone === 'ok' ? CircleCheck : tone === 'warn' ? TriangleAlert : tone === 'bad' ? CircleX : tone === 'wait' ? LoaderCircle : Info
  return (
    <div className="flex items-start gap-3.5 border-t border-[var(--border-subtle)] py-3.5 first:border-t-0">
      <Icon size={18} className={`mt-px shrink-0 ${TONE_TEXT[tone]} ${tone === 'wait' ? 'animate-spin' : ''}`} />
      <div className="min-w-0 flex-1">
        <p className="text-sm font-semibold">{title}</p>
        <div className="mt-1 text-xs leading-normal text-[var(--text-muted)]">{detail}</div>
        {children}
      </div>
      <div className="shrink-0 self-center">
        {action ?? (status && <span className={`text-xs font-semibold ${TONE_TEXT[tone]}`}>{status}</span>)}
      </div>
    </div>
  )
}

export function ConnectionCheck({ platform, port, selectedAddress, interfaces, onChangePort }: {
  platform: string
  port: number
  /** The address the game sends to; picks the adapter the Network row reports. */
  selectedAddress: string
  interfaces: SetupNetworkInterface[]
  /** Shows a Change port button when the port can't be opened. */
  onChangePort?: () => void
}) {
  const [firewall, setFirewall] = useState<SetupFirewallStatus | null>(null)
  const [firewallBusy, setFirewallBusy] = useState(false)
  const [firewallError, setFirewallError] = useState('')
  const [listener, setListener] = useState<{ ok: boolean; error?: string } | null>(null)
  const [receiving, setReceiving] = useState(false)
  const [waitedLong, setWaitedLong] = useState(false)

  const checkFirewall = useCallback(() => {
    setFirewall(null)
    void window.setupBridge.firewallStatus().then(setFirewall)
  }, [])

  useEffect(() => {
    checkFirewall()
    void window.udpBridge.getStatus().then(setListener)
    const unsubscribe = window.udpBridge.onStatusChange(setListener)
    const timer = window.setTimeout(() => setWaitedLong(true), NO_TELEMETRY_HINT_MS)
    return () => { unsubscribe(); window.clearTimeout(timer) }
  }, [checkFirewall])

  // Datagram counter once a second; "receiving" means new datagrams in the last 3 s.
  useEffect(() => {
    let cancelled = false
    let previous = -1
    let lastArrival = 0
    const poll = async () => {
      const sample: SetupUdpProbe = await window.setupBridge.udpProbe()
      if (cancelled || !sample.available) return
      const now = performance.now()
      if (previous >= 0 && sample.datagrams > previous) lastArrival = now
      previous = sample.datagrams
      setReceiving(lastArrival > 0 && now - lastArrival < 3000)
    }
    void poll()
    const interval = window.setInterval(() => void poll(), 1000)
    return () => { cancelled = true; window.clearInterval(interval) }
  }, [])

  async function allow() {
    setFirewallBusy(true)
    setFirewallError('')
    const result = await window.setupBridge.allowThroughFirewall()
    setFirewallBusy(false)
    if (result && result !== 'cancelled') setFirewallError(result)
    if (!result) checkFirewall()
  }

  const realInterface = interfaces.find(item => item.address === selectedAddress && item.kind !== 'loopback')
    ?? interfaces.find(item => item.kind !== 'virtual' && item.kind !== 'loopback')
  const ufwCommand = `sudo ufw allow ${port}/udp`

  let firewallRow: ReactNode
  if (platform === 'win32') {
    if (!firewall) {
      firewallRow = <CheckRow tone="wait" title="Windows Firewall" detail="Checking…" />
    } else if (!firewall.supported || firewall.allowed === 'unknown') {
      firewallRow = <CheckRow tone="warn" title="Windows Firewall" detail={firewall.error || 'Couldn’t check the firewall.'} status="Unknown" />
    } else if (firewall.allowed === 'yes') {
      firewallRow = <CheckRow tone="ok" title="Windows Firewall" detail={firewall.enabled ? `UDP ${port} is allowed.` : 'The firewall is off.'} status="Allowed" />
    } else {
      firewallRow = (
        <CheckRow
          tone="bad" title="Windows Firewall"
          detail={<>
            {firewall.blockRule ? 'A firewall rule blocks Track N Race.' : firewall.publicNetwork ? 'Track N Race is blocked on Public networks.' : 'Track N Race isn’t allowed through the firewall.'}
            {firewallError && <span className="mt-1 block text-red-400">{firewallError}</span>}
          </>}
          action={
            <button type="button" className={BUTTON_CLASS} disabled={firewallBusy} onClick={() => void allow()}>
              {firewallBusy ? <LoaderCircle size={12} className="animate-spin" /> : <Shield size={12} />} Allow through firewall
            </button>
          }
        />
      )
    }
  } else if (platform === 'linux') {
    firewallRow = (
      <CheckRow
        tone="info" title="Firewall"
        detail={<>Not checked on Linux. If nothing arrives with ufw on, run <code className="text-[var(--text-secondary)]">{ufwCommand}</code></>}
        action={<button type="button" className={BUTTON_CLASS} onClick={() => copyText(ufwCommand)}><Copy size={11} /> Copy command</button>}
      />
    )
  } else {
    firewallRow = <CheckRow tone="info" title="Firewall" detail="macOS asks to allow incoming connections the first time Track N Race listens." />
  }

  const networkRow = platform === 'win32' && firewall?.supported && firewall.publicNetwork
    ? <CheckRow tone="warn" title="Network" detail={`${realInterface ? `${realInterface.name} is` : 'This network is'} set as a Public network.`} status="Public" />
    : realInterface
      ? <CheckRow tone="ok" title="Network" detail={`${realInterface.name} · ${realInterface.address}`} status="Connected" />
      : <CheckRow tone="warn" title="Network" detail="No network found. Only a game on this PC can send telemetry." status="Offline" />

  const portRow = !listener
    ? <CheckRow tone="wait" title={`Port ${port}`} detail="Checking…" />
    : listener.ok
      ? <CheckRow tone="ok" title={`Port ${port}`} detail="Listening for telemetry." status="Listening" />
      : <CheckRow
          tone="bad" title={`Port ${port}`} detail={listener.error || 'The port could not be opened.'}
          action={onChangePort && <button type="button" className={BUTTON_CLASS} onClick={onChangePort}>Change port</button>}
        />

  const telemetryRow = receiving
    ? <CheckRow tone="ok" title="Telemetry" detail="Packets are arriving from the game." status="Receiving" />
    : !waitedLong
      ? <CheckRow tone="wait" title="Telemetry" detail="Start a session in the game. This updates on its own." status="Waiting…" />
      : (
          <CheckRow tone="warn" title="Telemetry" detail="Nothing received yet." status="No packets">
            <ul className="mt-2.5 flex flex-col gap-1.5 text-xs text-[var(--text-secondary)]">
              <li>– <b className="font-semibold text-[var(--text-primary)]">UDP Telemetry</b> is On in the game.</li>
              <li>– The game’s IP is <b className="font-semibold text-[var(--text-primary)]">{selectedAddress || 'this PC'}</b> and its port is <b className="font-semibold text-[var(--text-primary)]">{port}</b>.</li>
              <li>– You’re on track or in the garage, not in a menu.</li>
              <li>– The console and this PC are on the same network.</li>
            </ul>
          </CheckRow>
        )

  return (
    <div>
      {firewallRow}
      {networkRow}
      {portRow}
      {telemetryRow}
    </div>
  )
}
