// Runs history decodes in a worker (historyDecode.worker.ts). The worker is
// started on demand and terminated once idle, which releases everything its
// decodes allocated; the UI thread's heap never holds those temporaries.
import DecodeWorker from './historyDecode.worker?worker&inline'
import { setHistoryDecoder, type DecodedV6History } from './columnStore'

const IDLE_MS = 10_000

type Pending = { resolve: (decoded: DecodedV6History) => void; reject: (error: Error) => void }

let worker: Worker | null = null
let nextId = 0
const pending = new Map<number, Pending>()
let idleTimer: ReturnType<typeof setTimeout> | null = null

function stopWorker(error?: Error): void {
  worker?.terminate()
  worker = null
  for (const request of pending.values()) request.reject(error ?? new Error('history decode worker stopped'))
  pending.clear()
}

function startWorker(): Worker {
  const started = new DecodeWorker()
  started.onmessage = (event: MessageEvent<{ id: number; decoded?: DecodedV6History; error?: string }>) => {
    const request = pending.get(event.data.id)
    if (!request) return
    pending.delete(event.data.id)
    if (event.data.decoded) request.resolve(event.data.decoded)
    else request.reject(new Error(event.data.error ?? 'history decode failed'))
    if (pending.size === 0) idleTimer = setTimeout(() => stopWorker(), IDLE_MS)
  }
  started.onerror = event => stopWorker(new Error(event.message || 'history decode worker error'))
  return started
}

function decodeInWorker(bytes: Uint8Array): Promise<DecodedV6History> {
  if (idleTimer !== null) {
    clearTimeout(idleTimer)
    idleTimer = null
  }
  worker ??= startWorker()
  const id = ++nextId
  // The worker gets its own copy; the caller keeps using `bytes`.
  const copy = bytes.slice()
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject })
    worker!.postMessage({ id, bytes: copy }, [copy.buffer])
  })
}

export function installWorkerHistoryDecoder(): void {
  if (typeof Worker === 'undefined') return
  setHistoryDecoder(decodeInWorker)
}
