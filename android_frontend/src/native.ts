import { registerPlugin, type PluginListenerHandle } from '@capacitor/core'

export type TelemetrySource = 'direct' | 'paired'

export interface AndroidSettings {
  source: TelemetrySource
  recordingEnabled: boolean
  hasSavedDesktop: boolean
  desktopName: string
  recordingDirectory: string
  usingCustomDirectory: boolean
}

export interface DiscoveredDesktop {
  serverId: string
  name: string
  host: string
  port: number
  pairing: boolean
}

export interface SourceStateEvent {
  state: string
  detail?: string
}

interface TelemetryPlugin {
  start(): Promise<AndroidSettings>
  stop(): Promise<void>
  exitApp(): Promise<void>
  reconnect(): Promise<void>
  getSettings(): Promise<AndroidSettings>
  setSource(options: { source: TelemetrySource }): Promise<AndroidSettings>
  setRecording(options: { enabled: boolean }): Promise<AndroidSettings>
  chooseRecordingDirectory(): Promise<AndroidSettings>
  useDefaultRecordingDirectory(): Promise<AndroidSettings>
  startDiscovery(): Promise<void>
  stopDiscovery(): Promise<void>
  scanPairingQr(): Promise<{ payload: string }>
  pairQr(options: { payload: string }): Promise<void>
  pairCode(options: DiscoveredDesktop & { code: string }): Promise<void>
  forgetDesktop(): Promise<AndroidSettings>
  addListener(eventName: 'telemetryRow', listener: (event: { row: string }) => void): Promise<PluginListenerHandle>
  addListener(eventName: 'telemetryBinaryFallback', listener: (event: { base64: string }) => void): Promise<PluginListenerHandle>
  addListener(eventName: 'sourceState', listener: (event: SourceStateEvent) => void): Promise<PluginListenerHandle>
  addListener(eventName: 'settingsChanged', listener: (event: AndroidSettings) => void): Promise<PluginListenerHandle>
  addListener(eventName: 'discoveredDesktop', listener: (event: DiscoveredDesktop) => void): Promise<PluginListenerHandle>
  addListener(eventName: 'recordingExport', listener: (event: { movedFiles: number; error?: string }) => void): Promise<PluginListenerHandle>
}

export const Telemetry = registerPlugin<TelemetryPlugin>('Telemetry')
