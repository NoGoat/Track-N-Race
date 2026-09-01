// A MessageChannel posts a fresh renderer task without the nested-timer clamp.
// Running one callback per message gives Chromium a scheduling boundary for
// input and paint between large seek-decode/chart synchronization chunks.
const queue: Array<() => void> = []
const channel = new MessageChannel()

channel.port1.onmessage = () => {
  const callback = queue.shift()
  try {
    callback?.()
  } finally {
    if (queue.length > 0) channel.port2.postMessage(0)
  }
}

export function scheduleCooperativeTask(callback: () => void): void {
  const wasEmpty = queue.length === 0
  queue.push(callback)
  if (wasEmpty) channel.port2.postMessage(0)
}

export function yieldToMainThread(): Promise<void> {
  return new Promise(resolve => scheduleCooperativeTask(resolve))
}
