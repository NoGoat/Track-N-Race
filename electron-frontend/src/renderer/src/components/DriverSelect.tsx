import { useEffect, useMemo, useState } from 'react'
import type { FormatOptionLabelMeta } from 'react-select'
import Select from '../lib/AnimatedSelect'
import { buildSelectStyles, type SelectStyles } from '../lib/selectStyles'
import { selectComponents } from '../lib/selectComponents'
import type { DriverOption } from '../lib/driverOptions'

export const DRIVER_MENU_WIDTH = '18rem'

export interface TeamCatalog {
  names: Map<number, string>
  // Livery colours with the user's overrides applied.
  colors: Map<number, string>
}
const EMPTY_TEAM_CATALOG: TeamCatalog = { names: new Map(), colors: new Map() }

// Team ids name the same team in every game format, so one merged lookup
// covers whichever game the recording came from.
let teamCatalogPromise: Promise<TeamCatalog> | null = null
function loadTeamCatalog(): Promise<TeamCatalog> {
  teamCatalogPromise ??= window.protocolBridge.getTeamColors()
    .then(config => {
      const teams = Object.values(config.catalog).flat()
      const colors = new Map(teams.map(team => [team.id, team.color] as const))
      for (const overrides of Object.values(config.overrides))
        for (const [id, color] of Object.entries(overrides)) colors.set(Number(id), color)
      return { names: new Map(teams.map(team => [team.id, team.name] as const)), colors }
    })
    .catch(() => EMPTY_TEAM_CATALOG)
  return teamCatalogPromise
}

export function useTeamCatalog(): TeamCatalog {
  const [catalog, setCatalog] = useState<TeamCatalog>(EMPTY_TEAM_CATALOG)
  useEffect(() => {
    let current = true
    void loadTeamCatalog().then(loaded => { if (current) setCatalog(loaded) })
    return () => { current = false }
  }, [])
  return catalog
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

// What a driver row shows: shared by every driver picker.
export interface DriverRowFields {
  label: string
  raceNumber: number
  teamId: number
  teamColor: string
  restricted: boolean
  isDisabled?: boolean
}

/** A driver's menu row: team colour, race number, name, team and access tag. */
export function DriverOptionRow({ option, selected, teamNames }: {
  option: DriverRowFields; selected: boolean; teamNames: Map<number, string>
}) {
  const secondary = selected ? 'rgba(255,255,255,0.7)' : 'var(--text-secondary)'
  return (
    <div className="flex items-center gap-2 min-w-0" style={{ opacity: option.isDisabled ? 0.4 : 1 }}>
      <span className="w-[3px] h-3.5 rounded-full shrink-0" style={{ background: option.teamColor }} />
      <span className="w-5 shrink-0 text-right tabular-nums font-semibold" style={{ color: secondary }}>
        {option.raceNumber >= 0 ? option.raceNumber : ''}
      </span>
      <span className="truncate font-medium">{option.label}</span>
      <span className="ml-auto truncate text-[10px]" style={{ color: secondary }}>
        {teamNames.get(option.teamId) ?? ''}
      </span>
      {option.restricted && <Tag selected={selected}>Public</Tag>}
    </div>
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
  const { names: teamNames } = useTeamCatalog()

  const styles = useMemo((): SelectStyles => {
    const base = buildSelectStyles(isDark, { solidBg, menuWidth: DRIVER_MENU_WIDTH })
    if (menuAlign === 'left') return base
    return { ...base, menu: (css, state) => ({ ...base.menu!(css, state), left: 'auto', right: 0 }) }
  }, [isDark, solidBg, menuAlign])

  const formatOptionLabel = (option: DriverOption, { context, selectValue }: FormatOptionLabelMeta<DriverOption>) => {
    if (context === 'value') return option.label
    return <DriverOptionRow option={option} selected={selectValue[0]?.value === option.value} teamNames={teamNames} />
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
