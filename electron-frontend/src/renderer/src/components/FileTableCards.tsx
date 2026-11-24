import type { ReactNode } from 'react'

/** One card's values for file A and file B. */
export interface FileTableCard {
  key: string
  label: string
  a: FileTableValue
  b: FileTableValue
}

export interface FileTableValue {
  value: string
  /** The value as separately coloured pieces (the tyre cards' temperatures and wear), in place of one colour. */
  parts?: Array<{ text: string; color?: string }>
  unit?: string
  /** The card's colour; B's value carries it too, a size smaller below A's. */
  color?: string
  title?: string
}

const MISSING = '—'

/**
 * The cards as one small table while two files are open: each card a column
 * with its label, file A's value and, a size smaller under it, file B's. The
 * size tells the files apart, so there are no date row headers.
 */
export default function FileTableCards({ cards }: { dateA?: string; dateB?: string; cards: FileTableCard[] }) {
  if (cards.length === 0) return null
  return (
    <div className="shrink-0 grid border-b border-[var(--border)] font-mono select-none"
      style={{ gridTemplateColumns: `repeat(${cards.length}, minmax(0, 1fr))` }}>
      {cards.map((card, index) => (
        <Column key={card.key} bordered={index > 0}>
          <span className="text-[9.5px] uppercase tracking-[.07em] text-[var(--text-secondary)] whitespace-nowrap truncate">{card.label}</span>
          <Value value={card.a} primary />
          <Value value={card.b} />
        </Column>
      ))}
    </div>
  )
}

function Column({ children, bordered }: { children: ReactNode; bordered?: boolean }) {
  return (
    <div className={`grid content-center gap-y-px min-w-0 py-2 px-3 ${bordered ? 'border-l border-[var(--border)]' : ''}`}
      style={{ gridTemplateRows: '16px 18px 16px' }}>
      {children}
    </div>
  )
}

function Value({ value, primary }: { value: FileTableValue; primary?: boolean }) {
  const missing = value.value === MISSING
  return (
    <span title={value.title}
      className={`font-bold whitespace-nowrap truncate ${primary ? 'text-[14px] leading-[18px]' : 'text-[12.5px] leading-4 opacity-85'}`}
      style={{ color: value.color ?? 'var(--text-primary)' }}>
      {value.parts
        ? value.parts.map((part, index) => <span key={index} style={part.color ? { color: part.color } : undefined}>{index > 0 ? ' ' : ''}{part.text}</span>)
        : value.value}
      {value.unit && !missing && <span className="ml-0.5 text-[9.5px] font-normal text-[var(--text-secondary)]">{value.unit}</span>}
    </span>
  )
}
