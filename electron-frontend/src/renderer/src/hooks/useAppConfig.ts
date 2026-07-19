import { useState, useCallback } from 'react'

declare global {
  interface Window {
    electronStore: {
      get: (key: string, defaultValue: unknown) => unknown
      set: (key: string, value: unknown) => void
    }
  }
}

export function useAppConfig<T>(key: string, defaultValue: T): [T, (v: T) => void] {
  const [value, setValue] = useState<T>(() => {
    try {
      return (window.electronStore.get(key, defaultValue) as T) ?? defaultValue
    } catch {
      return defaultValue
    }
  })

  const set = useCallback((v: T) => {
    setValue(v)
    try { window.electronStore.set(key, v) } catch {}
  }, [key])

  return [value, set]
}
