export interface AlignedTelemetryBufferStats {
  rows: number
  channels: number
  allocatedPages: number
  cpuBytes: number
}

interface AlignedTelemetryBuffer {
  telemetryRetentionStats(): AlignedTelemetryBufferStats
}

const alignedBuffers = new Map<number, WeakRef<AlignedTelemetryBuffer>>()
const alignedBufferFinalizer = new FinalizationRegistry<number>((id) => {
  alignedBuffers.delete(id)
})
let nextAlignedBufferId = 1

let gpuPageCount = 0
let gpuTextureBytes = 0

export function registerAlignedTelemetryBuffer(buffer: AlignedTelemetryBuffer): void {
  const id = nextAlignedBufferId++
  alignedBuffers.set(id, new WeakRef(buffer))
  alignedBufferFinalizer.register(buffer, id)
}

export function retainTelemetryGpuPage(bytes: number): void {
  gpuPageCount++
  gpuTextureBytes += Math.max(0, Math.trunc(bytes))
}

export function releaseTelemetryGpuPage(bytes: number): void {
  gpuPageCount = Math.max(0, gpuPageCount - 1)
  gpuTextureBytes = Math.max(0, gpuTextureBytes - Math.max(0, Math.trunc(bytes)))
}

export function getTelemetryChartRetentionDiagnostics(): {
  bufferCount: number
  rows: number
  channels: number
  allocatedPages: number
  cpuBytes: number
  gpuPageCount: number
  gpuTextureBytes: number
} {
  let bufferCount = 0
  let rows = 0
  let channels = 0
  let allocatedPages = 0
  let cpuBytes = 0

  for (const [id, reference] of alignedBuffers) {
    const buffer = reference.deref()
    if (!buffer) {
      alignedBuffers.delete(id)
      continue
    }
    const stats = buffer.telemetryRetentionStats()
    bufferCount++
    rows += stats.rows
    channels += stats.channels
    allocatedPages += stats.allocatedPages
    cpuBytes += stats.cpuBytes
  }

  return {
    bufferCount,
    rows,
    channels,
    allocatedPages,
    cpuBytes,
    gpuPageCount,
    gpuTextureBytes,
  }
}
