import { useEffect, useLayoutEffect, useMemo, useRef, useState, type CSSProperties } from 'react'
import { createPortal } from 'react-dom'
import { ChevronDown } from 'lucide-react'
import { DriverOptionRow, useTeamCatalog, type DriverRowFields } from '../../components/DriverSelect'
import type { DriverOption } from '../../lib/driverOptions'
import type { SecondaryFileData } from '../../components/AnalyzeScreen'
import { SELECT_MENU_ANIMATION_MS } from '../../lib/selectStyles'

const NO_DRAG = { WebkitAppRegion: 'no-drag' } as CSSProperties
const FALLBACK_TEAM_COLOR = '#8e8e8e'
// Two of the single picker's menus side by side.
const MENU_WIDTH = '36rem'

/** One of file B's cars as a picker row. -1 is the recorded player when B lists no cars. */
export interface SecondaryDriverOption extends DriverRowFields {
  value: number
  isPlayer: boolean
}

/** A row of the menu: the same driver in both files, or one file's only. */
interface PairRow {
  key: string
  a: DriverOption | null
  b: SecondaryDriverOption | null
}

interface TitlebarDriversProps {
  /** File A's cars (the single picker's options). */
  aOptions: DriverOption[]
  aSelected: number | null
  /** File A saved only its player's car. */
  aDisabled: boolean
  onChangeA: (driverIndex: number) => void
  secondaryFile: SecondaryFileData
  /** The car followed in B; -1 is its recorded player. */
  bSelected: number
  onChangeB: (driverIndex: number) => void
  dateA: string
  dateB: string
  /** No room in the title bar: three-letter codes instead of names. */
  compact: boolean
}

/**
 * The title bar's driver picker while two files are open (Drivers 2): one plain
 * button, "ANTONELLI vs VERSTAPPEN", opening A's and B's fields side by side
 * with the same driver on the same row, plus "Same driver in both" and "Swap".
 */
export default function TitlebarDrivers({
  aOptions, aSelected, aDisabled, onChangeA, secondaryFile, bSelected, onChangeB, dateA, dateB, compact,
}: TitlebarDriversProps) {
  const { names: teamNames, colors: teamColors } = useTeamCatalog()
  const bOptions = useMemo(() => secondaryDriverOptions(secondaryFile, teamColors), [secondaryFile, teamColors])
  // -1 follows B's recorded player: shown as that car.
  const bValue = bSelected >= 0 ? bSelected : bOptions.find(option => option.isPlayer)?.value ?? -1
  const aOption = aOptions.find(option => option.value === aSelected) ?? null
  const bOption = bOptions.find(option => option.value === bValue) ?? null
  const rows = useMemo(() => pairRows(aOptions, bOptions), [aOptions, bOptions])

  const aName = aOption?.label ?? 'Driver'
  const bName = bOption?.label ?? 'Driver'
  // B's match for A's driver, and A's for B's: for "Same driver in both" and "Swap".
  const bMatchForA = aOption ? rows.find(row => row.a === aOption)?.b ?? null : null
  const aMatchForB = bOption ? rows.find(row => row.b === bOption)?.a ?? null : null
  const canSame = bMatchForA !== null && bMatchForA !== bOption && !bMatchForA.isDisabled
  const canSwap = !aDisabled && aMatchForB !== null && bMatchForA !== null && !aMatchForB.isDisabled
    && (aMatchForB !== aOption || bMatchForA !== bOption)

  const [open, setOpen] = useState(false)
  const buttonRef = useRef<HTMLButtonElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const [anchor, setAnchor] = useState<{ top: number; right: number } | null>(null)
  useLayoutEffect(() => {
    if (!open) return
    const rect = buttonRef.current?.getBoundingClientRect()
    if (rect) setAnchor({ top: rect.bottom + 4, right: Math.max(4, window.innerWidth - rect.right) })
  }, [open])
  // Closes on a click elsewhere, Escape, a resize or the window losing focus.
  useEffect(() => {
    if (!open) return
    const close = () => setOpen(false)
    const onPointer = (event: PointerEvent) => {
      const target = event.target as Node
      if (menuRef.current?.contains(target) || buttonRef.current?.contains(target)) return
      close()
    }
    const onKey = (event: KeyboardEvent) => { if (event.key === 'Escape') close() }
    document.addEventListener('pointerdown', onPointer, true)
    document.addEventListener('keydown', onKey)
    window.addEventListener('resize', close)
    window.addEventListener('blur', close)
    return () => {
      document.removeEventListener('pointerdown', onPointer, true)
      document.removeEventListener('keydown', onKey)
      window.removeEventListener('resize', close)
      window.removeEventListener('blur', close)
    }
  }, [open])

  const pickA = (option: DriverOption) => {
    if (aDisabled || option.isDisabled) return
    onChangeA(option.value)
  }
  const pickB = (option: SecondaryDriverOption) => {
    if (option.isDisabled) return
    onChangeB(option.value)
  }
  const sameDriver = () => {
    if (canSame && bMatchForA) onChangeB(bMatchForA.value)
  }
  const swap = () => {
    if (!canSwap || !aMatchForB || !bMatchForA) return
    if (aMatchForB !== aOption) onChangeA(aMatchForB.value)
    if (bMatchForA !== bOption) onChangeB(bMatchForA.value)
  }

  const fullLabel = <>{aName}<span className="font-normal text-[var(--text-secondary)]">vs</span>{bName}</>
  return (
    <div style={NO_DRAG} className="shrink-0">
      <button
        ref={buttonRef}
        type="button"
        onClick={() => setOpen(value => !value)}
        aria-haspopup="dialog"
        aria-expanded={open}
        title={`${dateA}: ${aName}  ·  ${dateB}: ${bName}`}
        className="relative flex items-center h-7 pl-2 pr-1.5 rounded-md text-[11px] font-bold text-[var(--text-primary)] whitespace-nowrap transition-colors hover:bg-[var(--bg-hover)]"
      >
        <span data-titlebar-drivers-label className="flex items-center gap-2">
          {compact
            ? <>{driverCode(aName)}<span className="font-normal text-[var(--text-secondary)]">vs</span>{driverCode(bName)}</>
            : fullLabel}
        </span>
        {/* The full names, measured (never shown) so the title bar knows when they fit again. */}
        <span aria-hidden className="absolute left-0 top-0 w-0 h-0 overflow-hidden invisible">
          <span data-titlebar-drivers-full className="inline-flex items-center gap-2 whitespace-nowrap">{fullLabel}</span>
        </span>
        <ChevronDown
          size={12}
          className="ml-3 shrink-0"
          style={{
            color: open ? 'var(--text-primary)' : 'var(--text-secondary)',
            transform: open ? 'rotate(180deg)' : 'rotate(0deg)',
            transition: 'transform 0.2s ease, color 0.15s ease',
          }}
        />
      </button>

      {open && anchor && createPortal(
        <div
          ref={menuRef}
          role="dialog"
          aria-label="Drivers"
          className="fixed z-[9999] select-none"
          style={{
            top: anchor.top,
            right: anchor.right,
            width: MENU_WIDTH,
            maxWidth: 'calc(100vw - 8px)',
            background: 'var(--bg-menu)',
            border: '1px solid var(--border)',
            borderRadius: 6,
            boxShadow: '0 8px 32px rgba(0,0,0,0.6)',
            transformOrigin: 'top center',
            animation: `selectMenuEnterBottom ${SELECT_MENU_ANIMATION_MS}ms cubic-bezier(0.2, 0, 0, 1) both`,
          }}
        >
          <div className="grid grid-cols-[minmax(0,1fr)_1px_minmax(0,1fr)] gap-x-1 p-1">
            <ColumnHeader date={dateA} colorVar="--file-a" />
            <span />
            <ColumnHeader date={dateB} colorVar="--file-b" />
            <div className="max-h-[60vh] overflow-y-auto col-span-3 grid grid-cols-[minmax(0,1fr)_1px_minmax(0,1fr)] gap-x-1">
              {rows.map(row => (
                <PairRowCells
                  key={row.key}
                  row={row}
                  aSelected={row.a !== null && row.a === aOption}
                  bSelected={row.b !== null && row.b === bOption}
                  aLocked={aDisabled}
                  teamNames={teamNames}
                  onPickA={pickA}
                  onPickB={pickB}
                />
              ))}
            </div>
          </div>
          <div className="flex items-center gap-2 px-2 py-1.5 border-t border-[var(--border)]">
            <button
              type="button"
              onClick={sameDriver}
              disabled={!canSame}
              title={bMatchForA ? `Follow ${bMatchForA.label} in ${dateB} too` : `${aName} is not in ${dateB}`}
              className="h-6 px-2.5 rounded text-[11px] font-bold transition-colors bg-[var(--bg-hover)] text-[var(--text-primary)] hover:bg-[var(--border)] disabled:opacity-40 disabled:cursor-not-allowed"
            >
              Same driver in both
            </button>
            <button
              type="button"
              onClick={swap}
              disabled={!canSwap}
              title={canSwap ? `${dateA} follows ${bName}, ${dateB} follows ${aName}` : undefined}
              className="h-6 px-2.5 rounded text-[11px] border border-[var(--border)] text-[var(--text-secondary)] transition-colors hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-40 disabled:cursor-not-allowed"
            >
              Swap
            </button>
            {aDisabled && <span className="ml-auto text-[10px] text-[var(--text-secondary)]">{dateA} saved only your driver.</span>}
          </div>
        </div>,
        document.body,
      )}
    </div>
  )
}

function ColumnHeader({ date, colorVar }: { date: string; colorVar: '--file-a' | '--file-b' }) {
  return (
    <div className="flex items-center h-7 px-2 text-[11px] font-mono font-bold" style={{ color: `var(${colorVar})` }}>{date}</div>
  )
}

function PairRowCells({ row, aSelected, bSelected, aLocked, teamNames, onPickA, onPickB }: {
  row: PairRow
  aSelected: boolean
  bSelected: boolean
  aLocked: boolean
  teamNames: Map<number, string>
  onPickA: (option: DriverOption) => void
  onPickB: (option: SecondaryDriverOption) => void
}) {
  return <>
    {row.a
      ? <DriverCell option={row.a} selected={aSelected} disabled={row.a.isDisabled || (aLocked && !aSelected)} teamNames={teamNames} onPick={() => onPickA(row.a!)} />
      : <span />}
    <span className="bg-[var(--border)]" />
    {row.b
      ? <DriverCell option={row.b} selected={bSelected} disabled={!!row.b.isDisabled} teamNames={teamNames} onPick={() => onPickB(row.b!)} />
      : <span />}
  </>
}

function DriverCell({ option, selected, disabled, teamNames, onPick }: {
  option: DriverRowFields
  selected: boolean
  disabled: boolean
  teamNames: Map<number, string>
  onPick: () => void
}) {
  return (
    <button
      type="button"
      onClick={onPick}
      disabled={disabled}
      aria-pressed={selected}
      className={`w-full min-w-0 text-left text-[11px] rounded px-2 py-[5px] transition-colors ${
        selected
          ? 'bg-[var(--border-focus)] text-white'
          : disabled ? 'text-[var(--text-primary)] cursor-default' : 'text-[var(--text-primary)] hover:bg-[var(--bg-hover)]'
      }`}
    >
      <DriverOptionRow option={{ ...option, isDisabled: disabled && !selected }} selected={selected} teamNames={teamNames} />
    </button>
  )
}

/** File B's cars, ordered as file A's are (by race number; a car without one last). */
function secondaryDriverOptions(file: SecondaryFileData, teamColors: Map<number, string>): SecondaryDriverOption[] {
  if (file.analysisDrivers.length === 0) {
    return [{ value: -1, label: 'Recorded driver', raceNumber: -1, teamId: -1, teamColor: FALLBACK_TEAM_COLOR, restricted: false, isPlayer: true }]
  }
  return file.analysisDrivers
    .map(driver => ({
      value: driver.driverIndex,
      label: (driver.driverName ?? '').trim() || `Car ${driver.driverIndex + 1}`,
      raceNumber: driver.raceNumber ?? -1,
      teamId: driver.teamId ?? -1,
      teamColor: teamColors.get(driver.teamId ?? -1) ?? FALLBACK_TEAM_COLOR,
      restricted: driver.restricted ?? false,
      isPlayer: driver.isPlayer,
    }))
    .sort((a, b) => (a.raceNumber < 0 ? 1 : 0) - (b.raceNumber < 0 ? 1 : 0) || a.raceNumber - b.raceNumber || a.value - b.value)
}

const nameKey = (label: string) => label.trim().toLocaleLowerCase()

/**
 * The menu's rows: each of B's cars on the row of the same driver in A, matched
 * by name and then by race number; a driver in only one file gets a row of its
 * own with the other side empty.
 */
function pairRows(aOptions: DriverOption[], bOptions: SecondaryDriverOption[]): PairRow[] {
  const rows: PairRow[] = aOptions.map(option => ({ key: `a${option.value}`, a: option, b: null }))
  const byName = new Map<string, PairRow>()
  const byNumber = new Map<number, PairRow>()
  for (const row of rows) {
    if (!byName.has(nameKey(row.a!.label))) byName.set(nameKey(row.a!.label), row)
    if (row.a!.raceNumber > 0 && !byNumber.has(row.a!.raceNumber)) byNumber.set(row.a!.raceNumber, row)
  }
  const unmatched: SecondaryDriverOption[] = []
  for (const option of bOptions) {
    let row = byName.get(nameKey(option.label))
    if (!row || row.b) row = option.raceNumber > 0 ? byNumber.get(option.raceNumber) : undefined
    if (row && !row.b) row.b = option
    else unmatched.push(option)
  }
  for (const option of unmatched) rows.push({ key: `b${option.value}`, a: null, b: option })
  const number = (row: PairRow) => {
    const value = row.a?.raceNumber ?? row.b?.raceNumber ?? -1
    return value > 0 ? value : Number.MAX_SAFE_INTEGER
  }
  return rows.sort((x, y) => number(x) - number(y))
}

/** "VER" from "VERSTAPPEN": the first three letters of the name, upper case. */
export function driverCode(name: string): string {
  const letters = Array.from(name.normalize('NFC')).filter(char => /\p{L}|\p{N}/u.test(char))
  return letters.length > 0 ? letters.slice(0, 3).join('').toLocaleUpperCase() : name
}
