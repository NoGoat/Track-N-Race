import { X } from 'lucide-react'
import type { CoreLayout, InputLayout, MiscLayout, PowerLayout, SessionLayout, Tab, TyresLayout } from '../appConfig'
import { useModalPresence } from '../../lib/useModalPresence'

interface LayoutEditorProps {
  coreLayout: CoreLayout
  editOpen: boolean
  inputLayout: InputLayout
  miscLayout: MiscLayout
  powerLayout: PowerLayout
  sessionLayout: SessionLayout
  setCoreLayout: (layout: CoreLayout) => void
  setEditOpen: (open: boolean) => void
  setInputLayout: (layout: InputLayout) => void
  setMiscLayout: (layout: MiscLayout) => void
  setPowerLayout: (layout: PowerLayout) => void
  setSessionLayout: (layout: SessionLayout) => void
  setTyresLayout: (layout: TyresLayout) => void
  tab: Tab
  tyreView: 'cards' | 'graphs'
  tyreWearMode: 'wear' | 'life'
  tyresLayout: TyresLayout
}

export default function LayoutEditor(props: LayoutEditorProps) {
  const { coreLayout, editOpen, inputLayout, miscLayout, powerLayout, sessionLayout, setCoreLayout,
    setEditOpen, setInputLayout, setMiscLayout, setPowerLayout, setSessionLayout, setTyresLayout,
    tab, tyreView, tyreWearMode, tyresLayout } = props
  const editableTab = tab === 'core' || tab === 'input' || tab === 'misc' || tab === 'power' || tab === 'tyres' || tab === 'session'
  const modalPresence = useModalPresence(editOpen && editableTab)
  return (
    <>
      {/* Edit modal — centered overlay */}
      {modalPresence.mounted && (
        <div
          data-state={modalPresence.visible ? 'open' : 'closed'}
          className="modal-backdrop fixed inset-0 z-50 flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
          onClick={() => setEditOpen(false)}
        >
          <div
            className="modal-panel bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[920px] max-h-[90vh] flex flex-col overflow-hidden"
            onClick={e => e.stopPropagation()}
          >
            {/* Header */}
            <div className="flex items-center justify-between px-6 py-4 border-b border-[var(--border)] shrink-0">
              <div>
                <div className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest">
                  {tab === 'input' ? 'Edit Input Layout' : tab === 'misc' ? 'Edit Misc Layout' : tab === 'power' ? 'Edit Power Layout' : tab === 'tyres' ? 'Edit Tyres Layout' : tab === 'session' ? 'Edit Session Layout' : 'Edit Overview Layout'}
                </div>
                <div className="text-[10px] font-mono text-[var(--text-secondary)] mt-1 uppercase tracking-wider">Toggle sections to show or hide</div>
              </div>
              <button
                onClick={() => setEditOpen(false)}
                className="w-8 h-8 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#e10600] transition-colors"
              >
                <X size={14} />
              </button>
            </div>

            {/* Body */}
            <div className="overflow-y-auto p-6 flex flex-col gap-6">
              {tab === 'session' ? (<>

                <div className="flex flex-col gap-6">
                  {/* Screen Layout Schematic */}
                  <div className="flex flex-col gap-2">
                    <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Screen Layout Schematic</div>
                    <div className="w-full flex flex-col rounded-none overflow-hidden border border-[var(--border)] bg-[var(--bg-input)] divide-y divide-[var(--border)]">

                      {/* 1. Header Components */}
                      <div className="w-full flex divide-x divide-[var(--border)]">
                        <button
                          onClick={() => setSessionLayout({ ...sessionLayout, header: { ...sessionLayout.header, gpName: !sessionLayout.header.gpName } })}
                          className={`h-14 w-60 flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                            sessionLayout.header.gpName
                              ? 'bg-[#5794F2]/10 text-[#5794F2]'
                              : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                          }`}
                        >
                          <span className="text-xs font-bold uppercase tracking-wider">GP & Circuit</span>
                          <span className={`text-[8px] mt-1 tracking-wider uppercase font-bold opacity-60 ${sessionLayout.header.gpName ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                            {sessionLayout.header.gpName ? 'ON' : 'OFF'}
                          </span>
                        </button>

                        <button
                          onClick={() => setSessionLayout({ ...sessionLayout, header: { ...sessionLayout.header, marshalZones: !sessionLayout.header.marshalZones } })}
                          className={`h-14 flex-1 flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                            sessionLayout.header.marshalZones
                              ? 'bg-[#5794F2]/10 text-[#5794F2]'
                              : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                          }`}
                        >
                          <span className="text-xs font-bold uppercase tracking-wider">Marshal Zones</span>
                          <span className={`text-[8px] mt-1 tracking-wider uppercase font-bold opacity-60 ${sessionLayout.header.marshalZones ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                            {sessionLayout.header.marshalZones ? 'ON' : 'OFF'}
                          </span>
                        </button>

                        <button
                          onClick={() => setSessionLayout({ ...sessionLayout, header: { ...sessionLayout.header, timeLeft: !sessionLayout.header.timeLeft } })}
                          className={`h-14 w-48 flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                            sessionLayout.header.timeLeft
                              ? 'bg-[#5794F2]/10 text-[#5794F2]'
                              : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                          }`}
                        >
                          <span className="text-xs font-bold uppercase tracking-wider">Time Left</span>
                          <span className={`text-[8px] mt-1 tracking-wider uppercase font-bold opacity-60 ${sessionLayout.header.timeLeft ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                            {sessionLayout.header.timeLeft ? 'ON' : 'OFF'}
                          </span>
                        </button>
                      </div>

                      {/* 2. Stat Cards Row */}
                      <div className="w-full flex divide-x divide-[var(--border)]">
                        {([
                          { key: 'totalLaps',     label: 'Total Laps' },
                          { key: 'lapsRemaining', label: 'Laps Rem'   },
                          { key: 'pitSpeedLimit', label: 'Pit Speed'  },
                          { key: 'pitWindow',     label: 'Pit Window' },
                          { key: 'pitRejoin',     label: 'Rejoin'     },
                          { key: 'trackTemp',     label: 'Track Temp' },
                          { key: 'airTemp',       label: 'Air Temp'   },
                          { key: 'trackLength',   label: 'Track Len'  },
                          { key: 'timeOfDay',     label: 'Time/Day'   },
                        ] as { key: keyof SessionLayout['statsCards']; label: string }[]).map(({ key, label }) => {
                          const on = sessionLayout.statsCards[key]
                          return (
                            <button
                              key={key}
                              onClick={() => setSessionLayout({ ...sessionLayout, statsCards: { ...sessionLayout.statsCards, [key]: !on } })}
                              className={`flex-1 py-4 flex flex-col items-center justify-center rounded-none font-mono text-[10px] font-bold transition-all relative ${
                                on
                                  ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                  : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                              }`}
                            >
                              <span>{label}</span>
                              <span className={`text-[8px] mt-1 tracking-wider uppercase font-bold opacity-60 ${on ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                                {on ? 'ON' : 'OFF'}
                              </span>
                            </button>
                          )
                        })}
                      </div>

                      {/* 3. Main Area (Left: Map & Weather; Right: Proximity & Events) */}
                      <div className="w-full flex divide-x divide-[var(--border)]">
                        {/* Left Column: Track Map on top, Weather Strip on bottom */}
                        <div className="flex-1 flex flex-col divide-y divide-[var(--border)]">
                          <button
                            onClick={() => setSessionLayout({ ...sessionLayout, showMap: !sessionLayout.showMap })}
                            className={`h-48 w-full flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                              sessionLayout.showMap
                                ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                            }`}
                          >
                            <span className="text-sm font-bold uppercase tracking-wider">Track Map</span>
                            <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${sessionLayout.showMap ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                              {sessionLayout.showMap ? 'ACTIVE' : 'HIDDEN'}
                            </span>
                          </button>

                          <button
                            onClick={() => setSessionLayout({ ...sessionLayout, showWeather: !sessionLayout.showWeather })}
                            className={`h-20 w-full flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                              sessionLayout.showWeather
                                ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                            }`}
                          >
                            <span className="text-xs font-bold uppercase tracking-wider">Weather Forecast Strip</span>
                            <span className={`text-[8px] mt-1 tracking-wider uppercase font-bold opacity-60 ${sessionLayout.showWeather ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                              {sessionLayout.showWeather ? 'ACTIVE' : 'HIDDEN'}
                            </span>
                          </button>
                        </div>

                        {/* Right Column: Proximity on top, Events on bottom */}
                        <div className="w-64 flex flex-col divide-y divide-[var(--border)]">
                          <button
                            onClick={() => setSessionLayout({ ...sessionLayout, showProximity: !sessionLayout.showProximity })}
                            className={`flex-1 min-h-[136px] flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                              sessionLayout.showProximity
                                ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                            }`}
                          >
                            <span className="text-xs font-bold uppercase tracking-wider">Proximity</span>
                            <span className={`text-[8px] mt-1 tracking-wider uppercase font-bold opacity-60 ${sessionLayout.showProximity ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                              {sessionLayout.showProximity ? 'ACTIVE' : 'HIDDEN'}
                            </span>
                          </button>

                          <button
                            onClick={() => setSessionLayout({ ...sessionLayout, showEvents: !sessionLayout.showEvents })}
                            className={`flex-1 min-h-[136px] flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                              sessionLayout.showEvents
                                ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                            }`}
                          >
                            <span className="text-xs font-bold uppercase tracking-wider">Events Log</span>
                            <span className={`text-[8px] mt-1 tracking-wider uppercase font-bold opacity-60 ${sessionLayout.showEvents ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                              {sessionLayout.showEvents ? 'ACTIVE' : 'HIDDEN'}
                            </span>
                          </button>
                        </div>
                      </div>

                    </div>
                  </div>
                </div>

              </>) : tab === 'tyres' ? (<>

                <div className="flex flex-col gap-2">
                  <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Charts (Graph View)</div>
                  <div className="w-full grid grid-cols-2 rounded-none overflow-hidden border border-[var(--border)] bg-[var(--border)] gap-[1px]">
                    {([
                      { key: 'surfaceTemp', label: 'Surface Temp' },
                      { key: 'innerTemp',   label: 'Inner Temp'   },
                      { key: 'brakeTemp',   label: 'Brake Temp'   },
                      { key: 'tyreLife',    label: tyreWearMode === 'life' ? 'Tyre Life' : 'Tyre Wear' },
                    ] as { key: keyof TyresLayout['charts']; label: string }[]).map(({ key, label }) => {
                      const on = tyresLayout.charts[key]
                      return (
                        <button
                          key={key}
                          onClick={() => setTyresLayout({ ...tyresLayout, charts: { ...tyresLayout.charts, [key]: !on } })}
                          className={`h-32 flex flex-col items-center justify-center rounded-none font-mono text-xs font-semibold transition-all relative ${
                            on
                              ? 'bg-[#5794F2]/10 text-[#5794F2]'
                              : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                          }`}
                        >
                          <span className="font-bold">{label}</span>
                          <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${on ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                            {on ? 'ACTIVE' : 'HIDDEN'}
                          </span>
                        </button>
                      )
                    })}
                  </div>
                </div>

              </>) : tab === 'power' ? (<>

                <div className="flex flex-col gap-6">
                  {/* Stats Bar Preview */}
                  <div className="flex flex-col gap-2">
                    <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Stats Bar</div>
                    <div className="w-full flex rounded-none overflow-hidden border border-[var(--border)] divide-x divide-[var(--border)] bg-[var(--bg-input)]">
                      {([
                        { key: 'totalPower', label: 'Total Power' },
                        { key: 'ice',        label: 'ICE'         },
                        { key: 'mguk',       label: 'MGU-K'       },
                        { key: 'split',      label: 'Split'       },
                        { key: 'ersStore',   label: 'ERS Store'   },
                        { key: 'ersPct',     label: 'ERS %'       },
                        { key: 'fuel',       label: 'Fuel'        },
                      ] as { key: keyof PowerLayout['statsCards']; label: string }[]).map(({ key, label }) => {
                        const on = powerLayout.statsCards[key]
                        return (
                          <button
                            key={key}
                            onClick={() => setPowerLayout({ ...powerLayout, statsCards: { ...powerLayout.statsCards, [key]: !on } })}
                            className={`flex-1 py-6 flex flex-col items-center justify-center rounded-none font-mono text-[11px] font-semibold transition-all relative ${
                              on
                                ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                            }`}
                          >
                            <span className="font-bold">{label}</span>
                            <span className={`text-[8px] mt-1 tracking-wider uppercase font-bold opacity-60 ${on ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                              {on ? 'ON' : 'OFF'}
                            </span>
                          </button>
                        )
                      })}
                    </div>
                  </div>

                  {/* Charts Breakdown Preview */}
                  <div className="flex flex-col gap-2">
                    <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Charts</div>
                    <div className="w-full flex rounded-none overflow-hidden border border-[var(--border)] divide-x divide-[var(--border)] bg-[var(--bg-input)]">
                      {([
                        { key: 'powerSplit',  label: 'Power Split'  },
                        { key: 'ersHarvest',  label: 'ERS Harvest'  },
                        { key: 'ersStore',    label: 'ERS Store'    },
                        { key: 'fuelHistory', label: 'Fuel History' },
                      ] as { key: keyof PowerLayout['charts']; label: string }[]).map(({ key, label }) => {
                        const on = powerLayout.charts[key]
                        return (
                          <button
                            key={key}
                            onClick={() => setPowerLayout({ ...powerLayout, charts: { ...powerLayout.charts, [key]: !on } })}
                            className={`flex-1 h-44 flex flex-col items-center justify-center rounded-none font-mono text-xs font-semibold transition-all relative ${
                              on
                                ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                            }`}
                          >
                            <span className="font-bold">{label}</span>
                            <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${on ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                              {on ? 'ACTIVE' : 'HIDDEN'}
                            </span>
                          </button>
                        )
                      })}
                    </div>
                  </div>
                </div>

              </>) : tab === 'misc' ? (<>

                <div className="flex flex-col gap-2">
                  <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Panels</div>
                  <div className="w-full flex flex-col rounded-none overflow-hidden border border-[var(--border)] divide-y divide-[var(--border)] bg-[var(--bg-input)]">
                    <button
                      onClick={() => setMiscLayout({ ...miscLayout, showGForce: !miscLayout.showGForce })}
                      className={`h-44 w-full flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                        miscLayout.showGForce
                          ? 'bg-[#5794F2]/10 text-[#5794F2]'
                          : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                      }`}
                    >
                      <span className="text-sm font-bold">G-Force Chart</span>
                      <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${miscLayout.showGForce ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                        {miscLayout.showGForce ? 'ACTIVE' : 'HIDDEN'}
                      </span>
                    </button>
                    <button
                      onClick={() => setMiscLayout({ ...miscLayout, showRideHeight: !miscLayout.showRideHeight })}
                      className={`h-44 w-full flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                        miscLayout.showRideHeight
                          ? 'bg-[#5794F2]/10 text-[#5794F2]'
                          : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                      }`}
                    >
                      <span className="text-sm font-bold">Ride Height Chart</span>
                      <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${miscLayout.showRideHeight ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                        {miscLayout.showRideHeight ? 'ACTIVE' : 'HIDDEN'}
                      </span>
                    </button>
                  </div>
                </div>

              </>) : tab === 'input' ? (<>

                <div className="flex flex-col gap-2">
                  <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Panels</div>
                  <div className="w-full flex flex-col rounded-none overflow-hidden border border-[var(--border)] divide-y divide-[var(--border)] bg-[var(--bg-input)]">
                    {/* Top Row: Gear and Throttle/Brake Chart */}
                    <div className="w-full flex divide-x divide-[var(--border)]">
                      <button
                        onClick={() => setInputLayout({ ...inputLayout, showGear: !inputLayout.showGear })}
                        className={`flex-1 h-48 flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                          inputLayout.showGear
                            ? 'bg-[#5794F2]/10 text-[#5794F2]'
                            : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                        }`}
                      >
                        <span className="text-sm font-bold">Gear Indicator</span>
                        <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${inputLayout.showGear ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                          {inputLayout.showGear ? 'ACTIVE' : 'HIDDEN'}
                        </span>
                      </button>
                      <button
                        onClick={() => setInputLayout({ ...inputLayout, showInputs: !inputLayout.showInputs })}
                        className={`flex-1 h-48 flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                          inputLayout.showInputs
                            ? 'bg-[#5794F2]/10 text-[#5794F2]'
                            : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                        }`}
                      >
                        <span className="text-sm font-bold">Throttle / Brake Chart</span>
                        <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${inputLayout.showInputs ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                          {inputLayout.showInputs ? 'ACTIVE' : 'HIDDEN'}
                        </span>
                      </button>
                    </div>
                    {/* Bottom Row: Steering full width */}
                    <button
                      onClick={() => setInputLayout({ ...inputLayout, showSteering: !inputLayout.showSteering })}
                      className={`h-28 w-full flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                        inputLayout.showSteering
                          ? 'bg-[#5794F2]/10 text-[#5794F2]'
                          : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                      }`}
                    >
                      <span className="text-sm font-bold">Steering Telemetry</span>
                      <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${inputLayout.showSteering ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                        {inputLayout.showSteering ? 'ACTIVE' : 'HIDDEN'}
                      </span>
                    </button>
                  </div>
                </div>

              </>) : (<>

                <div className="flex flex-col gap-6">
                  {/* Cohesive Layout Preview Box */}
                  <div className="flex flex-col gap-2">
                    <div className="text-[10px] font-mono text-[var(--text-secondary)] uppercase tracking-wider">Screen Layout Schematic</div>
                    <div className="w-full flex flex-col rounded-none overflow-hidden border border-[var(--border)] bg-[var(--bg-input)] divide-y divide-[var(--border)]">
                      
                      {/* 1. Stats Bar Row */}
                      <div className="w-full flex divide-x divide-[var(--border)]">
                        {([
                          { key: 'speed',    label: 'Speed'    },
                          { key: 'rpm',      label: 'RPM'      },
                          { key: 'gear',     label: 'Gear'     },
                          { key: 'throttle', label: 'Throt'    },
                          { key: 'brake',    label: 'Brake'    },
                          { key: 'drs',      label: 'DRS'      },
                          { key: 'engine',   label: 'Eng'      },
                          { key: 'ers',      label: 'ERS'      },
                          { key: 'fuel',     label: 'Fuel'     },
                          { key: 'pos',      label: 'Pos'      },
                          { key: 'tyre',     label: 'Tyre'     },
                        ] as { key: keyof CoreLayout['statsCards']; label: string }[]).map(({ key, label }) => {
                          const on = coreLayout.statsCards[key]
                          return (
                            <button
                              key={key}
                              onClick={() => setCoreLayout({ ...coreLayout, statsCards: { ...coreLayout.statsCards, [key]: !on } })}
                              className={`flex-1 py-5 flex flex-col items-center justify-center rounded-none font-mono text-[10px] font-bold transition-all relative ${
                                on
                                  ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                  : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                              }`}
                            >
                              <span>{label}</span>
                            </button>
                          )
                        })}
                      </div>

                      {/* 2. Main Chart Row */}
                      <button
                        onClick={() => setCoreLayout({ ...coreLayout, showSpeedChart: !coreLayout.showSpeedChart })}
                        className={`h-44 w-full flex flex-col items-center justify-center rounded-none font-mono transition-all relative ${
                          coreLayout.showSpeedChart
                            ? 'bg-[#5794F2]/10 text-[#5794F2]'
                            : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                        }`}
                      >
                        <span className="text-sm font-bold uppercase tracking-wider">Speed + RPM + ERS Chart</span>
                        <span className={`text-[9px] mt-1.5 tracking-widest uppercase font-bold opacity-60 ${coreLayout.showSpeedChart ? 'text-[#5794F2]' : 'text-[var(--text-muted)]'}`}>
                          {coreLayout.showSpeedChart ? 'VISIBLE' : 'HIDDEN'}
                        </span>
                      </button>

                      {/* 3. Tyre/Thermal Section Row */}
                      {tyreView === 'graphs' ? (
                        <div className="w-full flex divide-x divide-[var(--border)]">
                          {([
                            { key: 'surfaceTemp', label: 'Surf Temp' },
                            { key: 'innerTemp',   label: 'Inner Temp'   },
                            { key: 'brakeTemp',   label: 'Brake Temp'   },
                            { key: 'tyreLife',    label: tyreWearMode === 'life' ? 'Tyre Life' : 'Tyre Wear' },
                          ] as { key: keyof CoreLayout['thermalGraphs']; label: string }[]).map(({ key, label }) => {
                            const on = coreLayout.thermalGraphs[key]
                            return (
                              <button
                                key={key}
                                onClick={() => setCoreLayout({ ...coreLayout, thermalGraphs: { ...coreLayout.thermalGraphs, [key]: !on } })}
                                className={`flex-1 py-7 flex flex-col items-center justify-center rounded-none font-mono text-xs font-semibold transition-all relative ${
                                  on
                                    ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                    : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                                }`}
                              >
                                <span className="font-bold">{label}</span>
                              </button>
                            )
                          })}
                        </div>
                      ) : (
                        <div className="w-full flex divide-x divide-[var(--border)]">
                          {([
                            { key: 'fl', label: 'Front Left'  },
                            { key: 'fr', label: 'Front Right' },
                            { key: 'rl', label: 'Rear Left'   },
                            { key: 'rr', label: 'Rear Right'  },
                          ] as { key: keyof CoreLayout['thermalCards']; label: string }[]).map(({ key, label }) => {
                            const on = coreLayout.thermalCards[key]
                            return (
                              <button
                                key={key}
                                onClick={() => setCoreLayout({ ...coreLayout, thermalCards: { ...coreLayout.thermalCards, [key]: !on } })}
                                className={`flex-1 py-7 flex flex-col items-center justify-center rounded-none font-mono text-xs font-semibold transition-all relative ${
                                  on
                                    ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                    : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                                }`}
                              >
                                <span className="font-bold">{label}</span>
                              </button>
                            )
                          })}
                        </div>
                      )}

                      {/* 4. Damage Row 1 (Tyres & Brakes) */}
                      <div className="w-full flex divide-x divide-[var(--border)]">
                        {([
                          { key: 'tyreDmgFl',  label: 'Tyre FL'   },
                          { key: 'brakeDmgFl', label: 'Brake FL'  },
                          { key: 'tyreDmgFr',  label: 'Tyre FR'   },
                          { key: 'brakeDmgFr', label: 'Brake FR'  },
                          { key: 'tyreDmgRl',  label: 'Tyre RL'   },
                          { key: 'brakeDmgRl', label: 'Brake RL'  },
                          { key: 'tyreDmgRr',  label: 'Tyre RR'   },
                          { key: 'brakeDmgRr', label: 'Brake RR'  },
                        ] as { key: keyof CoreLayout['damageItems']; label: string }[]).map(({ key, label }) => {
                          const on = coreLayout.damageItems[key]
                          return (
                            <button
                              key={key}
                              onClick={() => setCoreLayout({ ...coreLayout, damageItems: { ...coreLayout.damageItems, [key]: !on } })}
                              className={`flex-1 py-4 flex flex-col items-center justify-center rounded-none font-mono text-[10px] font-semibold transition-all relative ${
                                on
                                  ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                  : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                              }`}
                            >
                              <span>{label}</span>
                            </button>
                          )
                        })}
                      </div>

                      {/* 5. Damage Row 2 (Aero & Body) */}
                      <div className="w-full flex divide-x divide-[var(--border)]">
                        {([
                          { key: 'wingFl',     label: 'Wing FL'   },
                          { key: 'wingFr',     label: 'Wing FR'   },
                          { key: 'wingRear',   label: 'Rear Wing' },
                          { key: 'floor',      label: 'Floor'     },
                          { key: 'diffuser',   label: 'Diffuser'  },
                          { key: 'sidepod',    label: 'Sidepod'   },
                          { key: 'gearbox',    label: 'Gearbox'   },
                          { key: 'engine',     label: 'Engine'    },
                        ] as { key: keyof CoreLayout['damageItems']; label: string }[]).map(({ key, label }) => {
                          const on = coreLayout.damageItems[key]
                          return (
                            <button
                              key={key}
                              onClick={() => setCoreLayout({ ...coreLayout, damageItems: { ...coreLayout.damageItems, [key]: !on } })}
                              className={`flex-1 py-4 flex flex-col items-center justify-center rounded-none font-mono text-[10px] font-semibold transition-all relative ${
                                on
                                  ? 'bg-[#5794F2]/10 text-[#5794F2]'
                                  : 'bg-[var(--bg-input)] text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-secondary)]'
                              }`}
                            >
                              <span>{label}</span>
                            </button>
                          )
                        })}
                      </div>

                    </div>
                  </div>
                </div>

              </>)}
            </div>
          </div>
        </div>
      )}
    </>
  )
}

