import { useEffect, useRef } from 'react'
import { dashboardState, type DashboardState } from './telemetryRuntime'

const RPM_LIGHT_COUNT = 15
const TYRE_LABELS: Record<number, string> = { 16: 'SOFT', 17: 'MED', 18: 'HARD', 7: 'INTER', 8: 'WET' }
const TYRE_TONES: Record<number, string> = { 16: 'red', 17: 'amber', 18: 'primary', 7: 'green', 8: 'accent' }

function clamp(value: number): number {
  return Math.max(0, Math.min(1, value))
}

function formatTime(milliseconds: number): string {
  if (milliseconds <= 0) return '—'
  const minutes = Math.floor(milliseconds / 60_000)
  const seconds = Math.floor(milliseconds / 1_000) % 60
  const millis = Math.floor(milliseconds % 1_000)
  return `${minutes}:${seconds.toString().padStart(2, '0')}.${millis.toString().padStart(3, '0')}`
}

function gearLabel(gear: number): string {
  return gear < 0 ? 'R' : gear === 0 ? 'N' : String(gear)
}

function tyreLabel(data: DashboardState): string {
  const compound = TYRE_LABELS[data.tyreCompound] ?? 'TYRE'
  return data.tyreAgeLaps > 0 ? `${compound} ${data.tyreAgeLaps}L` : `${compound} —`
}

function tyreTone(compound: number): string {
  return TYRE_TONES[compound] ?? 'secondary'
}

function label(data: DashboardState, key: string, fallback: string): string {
  return data.labels[key] ?? fallback
}

function setText(element: HTMLElement | null, value: string): void {
  if (element && element.textContent !== value) element.textContent = value
}

function setTone(element: HTMLElement | null, tone: string): void {
  if (element && element.dataset.tone !== tone) element.dataset.tone = tone
}

function setAriaLabel(element: HTMLElement | null, value: string): void {
  if (element && element.getAttribute('aria-label') !== value) {
    element.setAttribute('aria-label', value)
  }
}

function setProgress(element: HTMLElement | null, value: number): void {
  if (!element) return
  const transform = `scaleX(${clamp(value)})`
  if (element.style.transform !== transform) element.style.transform = transform
}

export default function Dashboard() {
  const rpmLightsRef = useRef<Array<HTMLSpanElement | null>>([])
  const aeroRef = useRef<HTMLSpanElement>(null)
  const tyreRef = useRef<HTMLSpanElement>(null)
  const gearRef = useRef<HTMLSpanElement>(null)
  const speedRef = useRef<HTMLSpanElement>(null)
  const speedMetaRef = useRef<HTMLSpanElement>(null)
  const brakeRef = useRef<HTMLSpanElement>(null)
  const throttleRef = useRef<HTMLSpanElement>(null)
  const positionRef = useRef<HTMLSpanElement>(null)
  const lapRef = useRef<HTMLSpanElement>(null)
  const currentLabelRef = useRef<HTMLSpanElement>(null)
  const currentLapRef = useRef<HTMLSpanElement>(null)
  const ersRef = useRef<HTMLSpanElement>(null)
  const fuelRef = useRef<HTMLSpanElement>(null)
  const lastLapRef = useRef<HTMLSpanElement>(null)

  useEffect(() => {
    let animationFrame = 0
    let previousRevision = -1
    const previous: Partial<DashboardState> = {}

    const update = () => {
      const data = dashboardState
      if (data.revision === previousRevision) {
        animationFrame = requestAnimationFrame(update)
        return
      }
      previousRevision = data.revision

      if (data.revLightsBitValue !== previous.revLightsBitValue) {
        for (let index = 0; index < RPM_LIGHT_COUNT; index += 1) {
          const lit = data.revLightsBitValue !== null
            && (data.revLightsBitValue & (1 << index)) !== 0
          rpmLightsRef.current[index]?.classList.toggle('lit', lit)
        }
        previous.revLightsBitValue = data.revLightsBitValue
      }

      if (data.aeroMode !== previous.aeroMode || data.drs !== previous.drs ||
          data.slm !== previous.slm || data.labels !== previous.labels) {
        const aeroActive = data.aeroMode === 'slm' ? data.slm > 0 : data.drs > 0
        setText(aeroRef.current, label(data, 'ui.overview.drs', data.aeroMode.toUpperCase()))
        setAriaLabel(aeroRef.current,
          label(data, 'drs.label', data.aeroMode === 'slm' ? 'Straight Line Mode' : 'DRS'))
        setTone(aeroRef.current, aeroActive ? 'green' : 'secondary')
        previous.aeroMode = data.aeroMode
        previous.drs = data.drs
        previous.slm = data.slm
        previous.labels = data.labels
      }
      if (data.tyreCompound !== previous.tyreCompound || data.tyreAgeLaps !== previous.tyreAgeLaps) {
        setText(tyreRef.current, tyreLabel(data))
        setTone(tyreRef.current, tyreTone(data.tyreCompound))
        previous.tyreCompound = data.tyreCompound
        previous.tyreAgeLaps = data.tyreAgeLaps
      }
      if (data.gear !== previous.gear) {
        setText(gearRef.current, gearLabel(data.gear))
        previous.gear = data.gear
      }
      if (data.speedKph !== previous.speedKph) {
        setText(speedRef.current, String(data.speedKph))
        previous.speedKph = data.speedKph
      }
      if (data.rpm !== previous.rpm) {
        setText(speedMetaRef.current, `KM/H  •  ${data.rpm} RPM`)
        previous.rpm = data.rpm
      }
      if (data.brake !== previous.brake) {
        setProgress(brakeRef.current, data.brake)
        previous.brake = data.brake
      }
      if (data.throttle !== previous.throttle) {
        setProgress(throttleRef.current, data.throttle)
        previous.throttle = data.throttle
      }

      if (data.position !== previous.position) {
        setText(positionRef.current, data.position > 0 ? `P${data.position}` : '—')
        previous.position = data.position
      }
      if (data.lapNumber !== previous.lapNumber || data.totalLaps !== previous.totalLaps) {
        setText(
          lapRef.current,
          data.lapNumber <= 0
            ? '—'
            : data.totalLaps > 0
              ? `${data.lapNumber} / ${data.totalLaps}`
              : String(data.lapNumber),
        )
        previous.lapNumber = data.lapNumber
        previous.totalLaps = data.totalLaps
      }
      if (data.lapInvalid !== previous.lapInvalid) {
        setText(currentLabelRef.current, data.lapInvalid ? 'CURRENT • INVALID' : 'CURRENT LAP')
        setTone(currentLapRef.current, data.lapInvalid ? 'red' : 'primary')
        previous.lapInvalid = data.lapInvalid
      }
      if (data.currentLapMs !== previous.currentLapMs) {
        setText(currentLapRef.current, formatTime(data.currentLapMs))
        previous.currentLapMs = data.currentLapMs
      }
      if (data.ersPercent !== previous.ersPercent) {
        setText(ersRef.current, `${data.ersPercent}%`)
        setTone(ersRef.current, data.ersPercent < 20 ? 'amber' : 'green')
        previous.ersPercent = data.ersPercent
      }
      if (data.fuelLaps !== previous.fuelLaps) {
        setText(fuelRef.current, data.fuelLaps > 0 ? `${data.fuelLaps.toFixed(1)} LAPS` : '—')
        previous.fuelLaps = data.fuelLaps
      }
      if (data.lastLapMs !== previous.lastLapMs) {
        setText(lastLapRef.current, formatTime(data.lastLapMs))
        previous.lastLapMs = data.lastLapMs
      }

      animationFrame = requestAnimationFrame(update)
    }

    animationFrame = requestAnimationFrame(update)
    return () => cancelAnimationFrame(animationFrame)
  }, [])

  return (
    <main className="dashboard" aria-label="Live racing dashboard">
      <div className="rpm-lights" aria-hidden="true">
        {Array.from({ length: RPM_LIGHT_COUNT }, (_, index) => (
          <span
            key={index}
            className={`rpm-light rpm-zone-${index < 5 ? 'green' : index < 10 ? 'red' : 'purple'}`}
            ref={element => { rpmLightsRef.current[index] = element }}
          />
        ))}
      </div>

      <section className="dashboard-panel dashboard-center" aria-label="Current telemetry">
        <div className="dashboard-pills">
          <span ref={aeroRef} className="dashboard-pill" data-tone="secondary">DRS</span>
          <span ref={tyreRef} className="dashboard-pill" data-tone="secondary">TYRE —</span>
        </div>
        <div className="dashboard-readout">
          <span ref={gearRef} className="dashboard-gear">N</span>
          <span ref={speedRef} className="dashboard-speed">0</span>
          <span ref={speedMetaRef} className="dashboard-speed-meta">KM/H&nbsp;&nbsp;•&nbsp;&nbsp;0 RPM</span>
        </div>
        <div className="dashboard-pedals">
          <div className="dashboard-pedal">
            <span className="pedal-label">BRK</span>
            <span className="pedal-track"><span ref={brakeRef} className="pedal-fill pedal-brake" /></span>
          </div>
          <div className="dashboard-pedal">
            <span className="pedal-label">THR</span>
            <span className="pedal-track"><span ref={throttleRef} className="pedal-fill pedal-throttle" /></span>
          </div>
        </div>
      </section>

      <section className="dashboard-panel dashboard-metric metric-position">
        <span className="metric-label">POSITION</span>
        <span ref={positionRef} className="metric-value" data-tone="accent">—</span>
      </section>
      <section className="dashboard-panel dashboard-metric metric-lap">
        <span className="metric-label">LAP</span>
        <span ref={lapRef} className="metric-value">—</span>
      </section>
      <section className="dashboard-panel dashboard-metric metric-current">
        <span ref={currentLabelRef} className="metric-label">CURRENT LAP</span>
        <span ref={currentLapRef} className="metric-value" data-tone="primary">—</span>
      </section>
      <section className="dashboard-panel dashboard-metric metric-ers">
        <span className="metric-label">ERS</span>
        <span ref={ersRef} className="metric-value" data-tone="amber">0%</span>
      </section>
      <section className="dashboard-panel dashboard-metric metric-fuel">
        <span className="metric-label">FUEL</span>
        <span ref={fuelRef} className="metric-value">—</span>
      </section>
      <section className="dashboard-panel dashboard-metric metric-last">
        <span className="metric-label">LAST LAP</span>
        <span ref={lastLapRef} className="metric-value">—</span>
      </section>
    </main>
  )
}
