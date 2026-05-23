import * as dgram from 'dgram'
import { HEADER_SIZE } from './packetHeader'
import { dispatchPacket } from './protocolDispatcher'

let activeSocket: dgram.Socket | null = null

export function stopUdpReceiver(): void {
  if (activeSocket) {
    try { activeSocket.close() } catch { /* already closed */ }
    activeSocket = null
  }
}

export function startUdpReceiver(config: { port: number; bindAddress: string }): void {
  const { port, bindAddress } = config
  const socket = dgram.createSocket('udp4')
  activeSocket = socket

  socket.on('message', (msg: Buffer) => {
    if (msg.length < HEADER_SIZE) return
    dispatchPacket(msg)
  })

  socket.on('error', (err) => {
    console.error('[UDP] Error:', err.message)
    activeSocket = null
  })

  socket.bind(port, bindAddress, () => {
    console.log(`[UDP] Listening on udp://${bindAddress}:${port}`)
  })
}
