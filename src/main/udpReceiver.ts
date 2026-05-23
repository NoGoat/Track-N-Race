import * as dgram from 'dgram'
import { HEADER_SIZE, parseHeader } from './packetHeader'
import { PARSERS } from './packetParsers'
import { RateLimiter } from './rateLimiter'
import { broadcastToWindows } from './index'

const SAMPLE_EVERY = parseInt(process.env.SAMPLE_EVERY_N_FRAMES ?? '1', 10)

let activeSocket: dgram.Socket | null = null

export function stopUdpReceiver(): void {
  if (activeSocket) {
    try { activeSocket.close() } catch { /* already closed */ }
    activeSocket = null
  }
}

export function startUdpReceiver(config: { port: number; bindAddress: string }): void {
  const { port, bindAddress } = config
  const rateLimiter = new RateLimiter(SAMPLE_EVERY)
  const socket = dgram.createSocket('udp4')
  activeSocket = socket

  socket.on('message', (msg: Buffer) => {
    if (msg.length < HEADER_SIZE) return
    const hdr = parseHeader(msg)
    const parsers = PARSERS[hdr.packetId]
    if (!parsers) return
    if (!rateLimiter.allow(hdr.packetId, hdr.overallFrameId)) return

    for (const parser of parsers) {
      for (const row of parser(msg, hdr)) {
        broadcastToWindows(row)
      }
    }
  })

  socket.on('error', (err) => {
    console.error('[UDP] Error:', err.message)
    activeSocket = null
  })

  socket.bind(port, bindAddress, () => {
    console.log(`[UDP] Listening on udp://${bindAddress}:${port}`)
  })
}
