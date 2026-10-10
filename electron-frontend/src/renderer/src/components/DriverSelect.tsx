import { useEffect, useMemo, useState } from 'react'
import type { FormatOptionLabelMeta } from 'react-select'
import Select from '../lib/AnimatedSelect'
import { buildSelectStyles, type SelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import type { DriverOption } from '../lib/driverOptions'

const MENU_WIDTH = '18rem'

// Team ids name the same team in every game format, so one merged lookup
// covers whichever game the recording came from.
let teamNamesPromise: Promise<Map<number, string>> | null = null
function loadTeamNames(): Promise<Map<number, string>> {
  teamNamesPromise ??= window.protocolBridge.getTeamColors()
    .then(config => new Map(Object.values(config.catalog).flat().map(team => [team.id, team.name] as const)))
    .catch(() => new Map<number, string>())
  return teamNamesPromise
}

function useTeamNames(): Map<number, string> {
  const [teamNames, setTeamNames] = useState<Map<number, string>>(() => new Map())
  useEffect(() => {
    let current = true
    void loadTeamNames().then(names => { if (current) setTeamNames(names) })
    return () => { current = false }
  }, [])
  return teamNames
}

function Tag({ children, selected }: { children: string; selected: boolean }) {
  return (
    <span
      className="shrink-0 rounded px-1 py-px text-[8px] font-semibold uppercase tracking-wider"
      style={selected
        ? { background: 'rgba(255,255,255,0.2)', color: '#fff' }
        : { background: 'var(--bg-hover)', color: 'var(--text-secondary)' }}
    >
      {children}
    </span>
  )
}

interface DriverSelectProps {
  options: DriverOption[]
  selectedDriverIdx: number | null
  onChange: (driverIndex: number | null) => void
  disabled?: boolean
  clearable?: boolean
  placeholder?: string
  isDark?: boolean
  solidBg?: boolean
  /** Which edge of the control the wider menu lines up with. */
  menuAlign?: 'left' | 'right'
  /** Render the menu in document.body, for controls inside clipped containers such as the title bar. */
  portal?: boolean
}

export default function DriverSelect({
  options, selectedDriverIdx, onChange, disabled = false, clearable = false,
  placeholder = 'Driver', isDark = true, solidBg = false, menuAlign = 'left', portal = false,
}: DriverSelectProps) {
  const teamNames = useTeamNames()

  const styles = useMemo((): SelectStyles => {
    const base = buildSelectStyles(isDark, { solidBg, menuWidth: MENU_WIDTH })
    if (menuAlign === 'left') return base
    return { ...base, menu: (css, state) => ({ ...base.menu!(css, state), left: 'auto', right: 0 }) }
  }, [isDark, solidBg, menuAlign])

  const formatOptionLabel = (option: DriverOption, { context, selectValue }: FormatOptionLabelMeta<DriverOption>) => {
    if (context === 'value') return option.label
    const selected = selectValue[0]?.value === option.value
    const secondary = selected ? 'rgba(255,255,255,0.7)' : 'var(--text-secondary)'
    return (
      <div className="flex items-center gap-2 min-w-0" style={{ opacity: option.isDisabled ? 0.4 : 1 }}>
        <span className="w-[3px] h-3.5 rounded-full shrink-0" style={{ background: option.teamColor }} />
        <span className="w-5 shrink-0 text-right tabular-nums font-semibold" style={{ color: secondary }}>
          {option.raceNumber}
        </span>
        <span className="truncate font-medium">{option.label}</span>
        <span className="ml-auto truncate text-[10px]" style={{ color: secondary }}>
          {teamNames.get(option.teamId) ?? ''}
        </span>
        {option.restricted && <Tag selected={selected}>Public</Tag>}
      </div>
    )
  }

  return (
    <Select<DriverOption>
      options={options}
      value={options.find(option => option.value === selectedDriverIdx) ?? null}
      onChange={option => onChange(option?.value ?? null)}
      isOptionDisabled={option => option.isDisabled}
      isDisabled={disabled}
      isClearable={clearable}
      isSearchable={false}
      placeholder={placeholder}
      formatOptionLabel={formatOptionLabel}
      styles={styles}
      components={selectComponents}
      menuPortalTarget={portal ? document.body : undefined}
    />
  )
}
