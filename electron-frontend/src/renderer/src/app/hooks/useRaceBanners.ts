import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { subscribeRaceEvent, useTelemetryStore } from '../../stores/telemetryStore'
import { buildBanner, lastName, type BannerItem } from '../bannerHelpers'

export function useRaceBanners(durationSeconds: number) {
  const participants = useTelemetryStore(state => state.participants)
  const session = useTelemetryStore(state => state.session)
  const protocolWarning = useTelemetryStore(state => state.protocolWarning)
  const [safetyCarBanner, setSafetyCarBanner] = useState<BannerItem | null>(null)
  const [transientBanner, setTransientBanner] = useState<BannerItem | null>(null)
  const queueRef = useRef<BannerItem[]>([])
  const showingRef = useRef(false)
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const participantsRef = useRef(participants)
  const durationRef = useRef(durationSeconds)
  participantsRef.current = participants
  durationRef.current = durationSeconds

  const dequeueRef = useRef<() => void>(() => {})
  dequeueRef.current = () => {
    if (queueRef.current.length === 0) {
      setTransientBanner(null)
      showingRef.current = false
      return
    }
    setTransientBanner(queueRef.current.shift()!)
    showingRef.current = true
    timerRef.current = setTimeout(() => dequeueRef.current(), durationRef.current * 1000)
  }

  const enqueue = useCallback((item: BannerItem) => {
    queueRef.current.push(item)
    if (!showingRef.current) dequeueRef.current()
  }, [])

  const handleLeaderChange = useCallback((idx: number) => {
    enqueue({ label: 'New Race Leader', sub: lastName(participantsRef.current, idx), color: '#5794F2' })
  }, [enqueue])

  useEffect(() => subscribeRaceEvent(event => {
    const item = buildBanner(event, participantsRef.current)
    if (item) enqueue(item)
  }), [enqueue])
  useEffect(() => () => { if (timerRef.current) clearTimeout(timerRef.current) }, [])
  useEffect(() => {
    if (!session) return
    const status = session.safety_car_status
    if (status === 0) { setSafetyCarBanner(null); return }
    const labels: Record<number, string> = { 1: 'SAFETY CAR', 2: 'VIRTUAL SAFETY CAR', 3: 'FORMATION LAP' }
    const colors: Record<number, string> = { 1: '#ffd700', 2: '#ffb347', 3: '#ffd700' }
    setSafetyCarBanner({ label: labels[status] ?? 'SAFETY CAR', color: colors[status] ?? '#ffd700' })
  }, [session])

  const warningBanner = useMemo<BannerItem | null>(() => protocolWarning ? {
    label: 'PROTOCOL MISMATCH DETECTED',
    sub: `Receiving ${protocolWarning.detected_format} packets - override is set to ${protocolWarning.forced_format}`,
    color: '#ff4646',
  } : null, [protocolWarning])
  const activeBanner = useMemo(() => warningBanner ?? transientBanner ?? safetyCarBanner, [warningBanner, transientBanner, safetyCarBanner])
  return { activeBanner, handleLeaderChange }
}
