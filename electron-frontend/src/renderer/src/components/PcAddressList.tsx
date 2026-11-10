import { useEffect, useState } from 'react'
import { Check, Copy } from 'lucide-react'
import type { SetupNetworkInterface } from '../types'

export function CopyButton({ value }: { value: string }) {
  const [copied, setCopied] = useState(false)
  useEffect(() => {
    if (!copied) return undefined
    const timer = window.setTimeout(() => setCopied(false), 1200)
    return () => window.clearTimeout(timer)
  }, [copied])
  return (
    <button
      type="button"
      aria-label={`Copy ${value}`}
      onClick={() => { void navigator.clipboard?.writeText(value).catch(() => {}); setCopied(true) }}
      className={`inline-flex items-center gap-1.5 justify-self-end text-[11px] transition-colors ${copied ? 'text-green-400' : 'text-[var(--text-muted)] hover:text-[var(--text-primary)]'}`}
    >
      {copied ? <Check size={11} /> : <Copy size={11} />} {copied ? 'Copied' : 'Copy'}
    </button>
  )
}

/** The address the game should send to: the first real adapter, else loopback. */
export function preferredAddress(list: SetupNetworkInterface[]): string {
  const preferred = list.find(item => item.kind !== 'virtual' && item.kind !== 'loopback') ?? list.find(item => item.kind === 'loopback')
  return preferred?.address ?? '127.0.0.1'
}

/** Loads this PC's network adapters once on mount. */
export function useNetworkInterfaces(): SetupNetworkInterface[] {
  const [interfaces, setInterfaces] = useState<SetupNetworkInterface[]>([])
  useEffect(() => {
    let current = true
    void window.setupBridge.networkInterfaces().then(list => { if (current) setInterfaces(list) })
    return () => { current = false }
  }, [])
  return interfaces
}

function interfaceLabel(item: SetupNetworkInterface): { title: string; detail: string } {
  if (item.kind === 'loopback') return { title: 'This PC only', detail: 'Game runs on this computer' }
  if (item.kind === 'virtual') return { title: item.name, detail: 'Virtual adapter' }
  return { title: item.name, detail: 'Console or another PC' }
}

export function PcAddressList({ interfaces, selectedAddress, onSelectAddress, copyable = false }: {
  interfaces: SetupNetworkInterface[]
  selectedAddress: string
  onSelectAddress: (address: string) => void
  /** Show a Copy button beside the selected address. */
  copyable?: boolean
}) {
  const [showVirtual, setShowVirtual] = useState(false)
  const virtualCount = interfaces.filter(item => item.kind === 'virtual').length
  const shown = interfaces.filter(item => showVirtual || item.kind !== 'virtual' || item.address === selectedAddress)
  return (
    <div role="radiogroup" aria-label="Address for the game" className="-mx-2.5 flex flex-col">
      {interfaces.length === 0 && <p className="px-2.5 py-2 text-xs text-[var(--text-muted)]">Looking for network adapters…</p>}
      {shown.map(item => {
        const selected = item.address === selectedAddress
        const { title, detail } = interfaceLabel(item)
        return (
          <div key={`${item.name}-${item.address}`} className="flex items-center gap-3 rounded transition-colors hover:bg-[var(--bg-hover)]">
            <button
              type="button"
              role="radio"
              aria-checked={selected}
              onClick={() => onSelectAddress(item.address)}
              className={`flex min-w-0 flex-1 items-center gap-3 px-2.5 py-2 text-left text-[13px] transition-colors hover:text-[var(--text-primary)] ${selected ? 'text-[var(--text-primary)]' : 'text-[var(--text-secondary)]'}`}
            >
              <span className="flex w-3.5 shrink-0 text-[var(--color-info)]">{selected && <Check size={13} />}</span>
              <span className="min-w-0 flex-1">
                <span className="block truncate">{title}</span>
                <span className="mt-0.5 block text-[11px] text-[var(--text-muted)]">{detail}</span>
              </span>
              <span className={`tabular-nums ${selected ? 'font-semibold text-[var(--color-info)]' : ''}`}>{item.address}</span>
            </button>
            {copyable && <span className="w-16 shrink-0 pr-2.5 text-right">{selected && <CopyButton value={item.address} />}</span>}
          </div>
        )
      })}
      {virtualCount > 0 && (
        <button
          type="button"
          onClick={() => setShowVirtual(!showVirtual)}
          className="px-2.5 py-1.5 text-left text-[11px] text-[var(--text-muted)] transition-colors hover:text-[var(--text-primary)]"
        >
          {showVirtual ? 'Hide virtual adapters' : `${virtualCount} virtual adapter${virtualCount === 1 ? '' : 's'} hidden. Show`}
        </button>
      )}
    </div>
  )
}
