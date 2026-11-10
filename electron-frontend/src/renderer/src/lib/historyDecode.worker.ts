// Decodes V6H1 history payloads off the UI thread. Each payload's temporary
// arrays live and die in this worker's heap; only the finished column chunks
// are transferred back.
import { decodedV6HistoryBuffers, decodeV6HistoryChunks } from './columnStore'

interface DecodeRequest { id: number; bytes: Uint8Array }

self.onmessage = async (event: MessageEvent<DecodeRequest>) => {
  const { id, bytes } = event.data
  try {
    const decoded = await decodeV6HistoryChunks(bytes)
    self.postMessage({ id, decoded }, { transfer: decodedV6HistoryBuffers(decoded) })
  } catch (error) {
    // Hand the payload back so the caller can decode it in-thread.
    self.postMessage({ id, error: error instanceof Error ? error.message : String(error), bytes },
                     { transfer: [bytes.buffer as ArrayBuffer] })
  }
}
