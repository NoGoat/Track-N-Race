import Stats from 'stats-gl'

interface RegisteredChart {
  canvas: HTMLCanvasElement
  element: HTMLElement
}

interface ActiveMonitor extends RegisteredChart {
  ready: boolean
  stats: Stats
}

export function installStatsGlDiagnostics(): void {
  if (window.__timeChartStats) return

  const charts = new Map<HTMLElement, RegisteredChart>()
  let active: ActiveMonitor | null = null
  let animationFrame = 0
  let generation = 0

  const disposeActive = (): void => {
    generation++
    if (!active) return
    active.stats.dispose()
    active.stats.dom.remove()
    active = null
  }

  const activate = (chart: RegisteredChart | undefined): void => {
    disposeActive()
    if (!chart) return

    const stats = new Stats({
      trackFPS: true,
      trackGPU: true,
      trackHz: true,
      horizontal: true,
      precision: 2,
    })
    stats.dom.style.top = '40px'
    stats.dom.style.left = '8px'
    stats.dom.style.zIndex = '2147483647'
    document.body.appendChild(stats.dom)

    const monitor: ActiveMonitor = { ...chart, ready: false, stats }
    active = monitor
    const currentGeneration = generation
    void stats.init(chart.canvas).then(() => {
      if (active === monitor && generation === currentGeneration) monitor.ready = true
    })
  }

  window.__timeChartStats = {
    register(element, canvas) {
      const chart = { element, canvas }
      charts.set(element, chart)
      if (!active) activate(chart)
    },
    unregister(element) {
      const wasActive = active?.element === element
      charts.delete(element)
      if (wasActive) activate(charts.values().next().value)
    },
    begin(element) {
      if (active?.ready && active.element === element) active.stats.begin()
    },
    end(element) {
      if (active?.ready && active.element === element) active.stats.end()
    },
  }

  const update = (): void => {
    active?.stats.update()
    animationFrame = requestAnimationFrame(update)
  }
  animationFrame = requestAnimationFrame(update)

  window.addEventListener('beforeunload', () => {
    cancelAnimationFrame(animationFrame)
    disposeActive()
    delete window.__timeChartStats
  }, { once: true })
}
