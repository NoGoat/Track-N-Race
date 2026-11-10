// Runs history decodes in a worker (historyDecode.worker.ts). The worker is
// started on demand and terminated once idle, which releases everything its
// decodes allocated; the UI thread's heap never holds those temporaries.
import DecodeWorker from './historyDecode.worker?worker&inline'
import { setHistoryDecoder, type DecodedV6History, type HistoryDecodeError } from './columnStore'

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
  started.onmessage = (event: MessageEvent<{ id: number; decoded?: DecodedV6History; error?: string; bytes?: Uint8Array }>) => {
    const request = pending.get(event.data.id)
    if (!request) return
    pending.delete(event.data.id)
    if (event.data.decoded) request.resolve(event.data.decoded)
    else {
      // The payload comes back with the failure, for the in-thread fallback.
      const error: HistoryDecodeError = new Error(event.data.error ?? 'history decode failed')
      error.bytes = event.data.bytes
      request.reject(error)
    }
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
  // A payload that owns its whole buffer is handed to the worker, which leaves
  // the caller's `bytes` detached; the worker returns it if the decode fails.
  // A view into a larger buffer is copied instead.
  const whole = bytes.buffer instanceof ArrayBuffer && bytes.byteOffset === 0 &&
    bytes.byteLength === bytes.buffer.byteLength
  const sent = whole ? bytes : bytes.slice()
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject })
    worker!.postMessage({ id, bytes: sent }, [sent.buffer as ArrayBuffer])
  })
}

export function installWorkerHistoryDecoder(): void {
  if (typeof Worker === 'undefined') return
  setHistoryDecoder(decodeInWorker)
}
