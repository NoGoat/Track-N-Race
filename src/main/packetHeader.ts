export const HEADER_SIZE = 29

const OFF_PACKET_ID    = 6
const OFF_SESSION_TIME = 15
const OFF_OVERALL_FID  = 23
const OFF_PLAYER_IDX   = 27

export interface PacketHeader {
  packetId: number
  playerCarIndex: number
  overallFrameId: number
  sessionTime: number
}

export function parseHeader(data: Buffer): PacketHeader {
  return {
    packetId:       data[OFF_PACKET_ID],
    playerCarIndex: data[OFF_PLAYER_IDX],
    overallFrameId: data.readUInt32LE(OFF_OVERALL_FID),
    sessionTime:    data.readFloatLE(OFF_SESSION_TIME),
  }
}
