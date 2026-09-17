export interface DebugSettings {
  additionalLogging: boolean
  memoryLog: boolean
  nodeApiExceptions: boolean
}

function readDebugSettings(): DebugSettings {
  return {
    additionalLogging: window.electronStore.get('debug.additionalLogging', false) === true,
    memoryLog: window.electronStore.get('debug.memoryLog', false) === true,
    nodeApiExceptions: window.electronStore.get('debug.nodeApiExceptions', false) === true,
  }
}

let settings = readDebugSettings()
const listeners = new Set<(value: DebugSettings) => void>()

window.debugBridge.onChange((value) => {
  settings = value
  for (const listener of listeners) listener(value)
})

export function getDebugSettings(): DebugSettings {
  return settings
}

export function isAdditionalLoggingEnabled(): boolean {
  return settings.additionalLogging
}

export function subscribeDebugSettings(listener: (value: DebugSettings) => void): () => void {
  listeners.add(listener)
  return () => { listeners.delete(listener) }
}
