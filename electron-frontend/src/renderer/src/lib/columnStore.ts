// ─────────────────────────────────────────────────────────────────────────────
// Columnar history store.
//
// Every renderer history family (telemetry, motion, motion_ex, status, damage,
// lap) is held as typed columns rather than an array of row objects: session
// times as Float64, every numeric/boolean field as Float32 (NaN = the row does
// not carry that field), other values as plain arrays. Charts, tables and lap
// analysis read samples by index through ColumnView; row objects are only
// materialised for the few consumers that want one sample (cards, tooltips).
//
// Columns are lists of fixed-size chunks, so a table grows without copying and
// a trim releases whole chunks. Chunks are shared between storages wherever
// rows are reused (a trim, a rewind, a history install), and copied before any
// write to one that is shared. A frozen view taken earlier therefore keeps
// reading exactly the rows it was published with, while costing only the chunks
// no newer storage holds. The one exception is the V6 patch merge into the
// newest row at the same timestamp, which the old row-array path also made
// visible to already-published arrays.
// ─────────────────────────────────────────────────────────────────────────────

export type HistoryFamily = 'telemetry' | 'motion' | 'motion_ex' | 'status' | 'damage' | 'lap'

const NUM = 0
const BOOL = 1
type NumericKind = typeof NUM | typeof BOOL

// Keys that describe a message rather than a sample value.
const META_FIELDS = new Set(['type', 'session_time', '_v6_type', 'available'])

// Fields each V6 type carries. An `available:false` sample clears them.
export const V6_PATCH_FIELDS: Record<number, readonly string[]> = {
  1: ['speed_kph'],
  2: ['rpm', 'rev_lights_pct', 'rev_lights_bit_value'],
  3: ['gear'],
  4: ['throttle'],
  5: ['brake'],
  6: ['steering'],
  7: ['drs', 'slm', 'drs_allowed'],
  8: ['tyre_temp_surface_fl', 'tyre_temp_surface_fr', 'tyre_temp_surface_rl', 'tyre_temp_surface_rr'],
  9: ['tyre_temp_inner_fl', 'tyre_temp_inner_fr', 'tyre_temp_inner_rl', 'tyre_temp_inner_rr'],
  10: ['brake_temp_fl', 'brake_temp_fr', 'brake_temp_rl', 'brake_temp_rr'],
  11: ['engine_temp'],
  12: ['tyre_wear_fl', 'tyre_wear_fr', 'tyre_wear_rl', 'tyre_wear_rr'],
  13: ['tyre_compound', 'visual_compound', 'tyre_age_laps', 'sets', 'fitted_idx'],
  14: ['tyre_dmg_fl', 'tyre_dmg_fr', 'tyre_dmg_rl', 'tyre_dmg_rr',
    'brake_dmg_fl', 'brake_dmg_fr', 'brake_dmg_rl', 'brake_dmg_rr', 'wing_fl', 'wing_fr', 'wing_rear',
    'floor_damage', 'diffuser_damage', 'sidepod_damage', 'gearbox_damage', 'engine_damage',
    'drs_fault', 'ers_fault', 'blisters_fl', 'blisters_fr', 'blisters_rl', 'blisters_rr',
    'engine_mguh_wear', 'engine_es_wear', 'engine_ce_wear', 'engine_ice_wear', 'engine_mguk_wear', 'engine_tc_wear',
    'engine_blown', 'engine_seized'],
  15: ['fuel_kg', 'fuel_laps', 'fuel_mix'],
  16: ['ers_j', 'ers_pct', 'ers_mode'],
  17: ['ers_harvested_mguk_j', 'ers_harvested_mguh_j'],
  18: ['ers_deployed_j'],
  19: ['engine_power_ice_kw', 'engine_power_mguk_kw'],
  20: ['front_brake_bias'],
  21: ['g_lat', 'g_long', 'g_vert'],
  22: ['front_aero_height_mm', 'rear_aero_height_mm'],
  23: ['x', 'z'],
  24: ['lap_distance_m', 'position', 'lap_num', 'current_lap_ms', 'last_lap_ms', 's1_ms',
    's2_ms', 'gap_ms', 'pit_status', 'num_pit_stops', 'lap_invalid', 'penalties_s', 'num_dt_pens',
    'num_sg_pens', 'sector', 'result_status', 'driver_status'],
}

// ── Chunked columns ──────────────────────────────────────────────────────────

// 4096-row chunks: a trimmed table (three laps in Current-lap mode, about 13K
// rows) wastes at most one chunk behind its head and one partly used chunk.
const CHUNK_BITS = 12
const CHUNK_ROWS = 1 << CHUNK_BITS
const CHUNK_MASK = CHUNK_ROWS - 1
const MIN_CHUNK_ROWS = 256

// Numeric fields whose values need more than Float32's 24-bit mantissa. The
// game sends single-precision floats and integers below 2^24 (milliseconds,
// joules), so none do today; session time is always Float64.
const FLOAT64_FIELDS = new Set<string>()

type NumChunk = Float32Array | Float64Array

// Rows a fresh chunk is sized for. A chunk grows (by copying, at most 4K
// rows) until it is full; a known count of rows from the chunk's start sizes
// it exactly instead.
function chunkRows(currentRows: number, offset: number, rowsHint: number): number {
  if (rowsHint > 0) return Math.min(CHUNK_ROWS, Math.max(offset + 1, currentRows, rowsHint))
  let rows = Math.max(MIN_CHUNK_ROWS, currentRows)
  while (rows <= offset) rows *= 4
  return Math.min(CHUNK_ROWS, rows)
}

/**
 * A column as chunks: chunk k holds rows [k * CHUNK_ROWS, (k + 1) * CHUNK_ROWS).
 * A missing or short chunk reads as absent. `owned[k]` is false for a chunk
 * shared with another storage, which is copied before it is written.
 */
class NumColumn {
  chunks: (NumChunk | undefined)[] = []
  owned: boolean[] = []

  constructor(readonly kind: NumericKind, readonly wide: boolean) {}

  get(p: number): number {
    const chunk = this.chunks[p >> CHUNK_BITS]
    const offset = p & CHUNK_MASK
    return chunk !== undefined && offset < chunk.length ? chunk[offset] : NaN
  }

  set(p: number, value: number, rowsHint = 0): void {
    this.writable(p, rowsHint)[p & CHUNK_MASK] = value
  }

  writable(p: number, rowsHint = 0): NumChunk {
    const k = p >> CHUNK_BITS
    const offset = p & CHUNK_MASK
    let chunk = this.chunks[k]
    if (chunk === undefined || !this.owned[k] || offset >= chunk.length) {
      const rows = chunkRows(chunk?.length ?? 0, offset, Math.max(0, rowsHint - (k << CHUNK_BITS)))
      const next = this.wide ? new Float64Array(rows) : new Float32Array(rows)
      next.fill(NaN)
      if (chunk) next.set(chunk)
      chunk = next
      this.chunks[k] = chunk
      this.owned[k] = true
    }
    return chunk
  }
}

/** Non-numeric values (strings, objects) as chunks of plain arrays. */
class OtherColumn {
  chunks: (unknown[] | undefined)[] = []
  owned: boolean[] = []

  get(p: number): unknown {
    return this.chunks[p >> CHUNK_BITS]?.[p & CHUNK_MASK]
  }

  set(p: number, value: unknown): void {
    const k = p >> CHUNK_BITS
    let chunk = this.chunks[k]
    if (chunk === undefined || !this.owned[k]) {
      chunk = chunk ? chunk.slice() : []
      this.chunks[k] = chunk
      this.owned[k] = true
    }
    chunk[p & CHUNK_MASK] = value
  }
}

// Every slot at or past a storage's `length` reads as absent: chunks shared
// from another storage lie wholly below `length`, and a partly used chunk is
// copied up to `length` only. Appending a row therefore never inherits values.
class Storage {
  head = 0
  length = 0
  readonly time = new NumColumn(NUM, true)
  readonly nums = new Map<string, NumColumn>()
  readonly others = new Map<string, OtherColumn>()

  /** Rows this storage is expected to hold, to size its chunks exactly. */
  constructor(readonly rowsHint = 0) {}

  timeAt(p: number): number { return this.time.get(p) }

  numColumn(field: string, kind: NumericKind): NumColumn {
    let column = this.nums.get(field)
    if (!column) {
      column = new NumColumn(kind, FLOAT64_FIELDS.has(field))
      this.nums.set(field, column)
    }
    return column
  }

  otherColumn(field: string): OtherColumn {
    let column = this.others.get(field)
    if (!column) {
      column = new OtherColumn()
      this.others.set(field, column)
    }
    return column
  }

  write(p: number, field: string, value: unknown): void {
    if (value === undefined || value === null || META_FIELDS.has(field)) return
    if (typeof value === 'number') this.numColumn(field, NUM).set(p, value, this.rowsHint)
    else if (typeof value === 'boolean') this.numColumn(field, BOOL).set(p, value ? 1 : 0, this.rowsHint)
    else this.otherColumn(field).set(p, value)
  }

  clearField(p: number, field: string): void {
    const column = this.nums.get(field)
    if (column && column.get(p) === column.get(p)) column.set(p, NaN)
    const other = this.others.get(field)
    if (other && other.get(p) !== undefined) other.set(p, undefined)
  }

  /** Copies row `from`'s values into the empty row `to`. */
  copyRow(from: number, to: number): void {
    for (const column of this.nums.values()) {
      const value = column.get(from)
      if (value === value) column.set(to, value, this.rowsHint)
    }
    for (const column of this.others.values()) {
      const value = column.get(from)
      if (value !== undefined) column.set(to, value)
    }
  }

  lowerBound(start: number, end: number, sessionTime: number, inclusive: boolean): number {
    let lo = start, hi = end
    while (lo < hi) {
      const mid = (lo + hi) >> 1
      const t = this.time.get(mid)
      if (inclusive ? t < sessionTime : t <= sessionTime) lo = mid + 1
      else hi = mid
    }
    return lo
  }

  materialize(rowType: string, p: number): Record<string, unknown> {
    const out: Record<string, unknown> = { type: rowType, session_time: this.time.get(p) }
    for (const [field, column] of this.nums) {
      const value = column.get(p)
      if (value === value) out[field] = column.kind === BOOL ? value !== 0 : value
    }
    for (const [field, column] of this.others) {
      const value = column.get(p)
      if (value !== undefined) out[field] = value
    }
    return out
  }

  /**
   * Rows [start, end) as a new storage that shares this one's chunks. Indices
   * are rebased to the first kept chunk; a chunk that `end` cuts through is
   * copied up to `end`, so the result's free slots stay absent. `keepOwnership`
   * hands the shared chunks over when this storage will not be written again.
   */
  slice(start: number, end: number, keepOwnership = false): Storage {
    const first = start >> CHUNK_BITS
    const base = first << CHUNK_BITS
    const out = new Storage(this.rowsHint)
    out.head = start - base
    out.length = end - base
    const lastChunk = (end - 1) >> CHUNK_BITS
    const cut = end & CHUNK_MASK
    const share = <C>(from: { chunks: (C | undefined)[]; owned: boolean[] }, to: { chunks: (C | undefined)[]; owned: boolean[] },
                      copy: (chunk: C, rows: number) => C) => {
      for (let k = first; k <= lastChunk && k < from.chunks.length; k++) {
        const chunk = from.chunks[k]
        if (chunk === undefined) continue
        if (k === lastChunk && cut !== 0) {
          to.chunks[k - first] = copy(chunk, cut)
          to.owned[k - first] = true
        } else {
          to.chunks[k - first] = chunk
          to.owned[k - first] = keepOwnership && from.owned[k]
        }
      }
    }
    // Every column carries over, even with no rows: a column's kind (number
    // or boolean) is fixed by the storage it was first written in.
    adoptColumns(this, out)
    if (end > start) {
      const copyNum = (chunk: NumChunk, rows: number): NumChunk => {
        const next = chunk instanceof Float64Array ? new Float64Array(chunk.length) : new Float32Array(chunk.length)
        next.fill(NaN)
        next.set(chunk.subarray(0, Math.min(rows, chunk.length)))
        return next
      }
      share(this.time, out.time, copyNum)
      for (const [field, column] of this.nums) share(column, out.nums.get(field)!, copyNum)
      for (const [field, column] of this.others) {
        share(column, out.others.get(field)!, (chunk: unknown[], rows) => chunk.slice(0, rows))
      }
    } else {
      out.head = out.length = 0
    }
    return out
  }
}

// Copies rows into a fresh Storage, taking the union of the sources' columns.
// Whole chunks that line up with the destination are shared, not copied.
class StorageBuilder {
  private storage: Storage

  constructor(rowsHint: number) {
    this.storage = new Storage(Math.max(0, rowsHint))
  }

  get length(): number { return this.storage.length - this.storage.head }

  lastTime(): number {
    const s = this.storage
    return s.length > s.head ? s.timeAt(s.length - 1) : -Infinity
  }

  /**
   * Starts the destination at the same position within a chunk as `sourceStart`,
   * so a range copied from there shares the source's whole chunks. Only
   * before anything is appended.
   */
  alignTo(sourceStart: number): void {
    const s = this.storage
    if (s.length !== s.head || s.length !== 0) return
    s.head = s.length = sourceStart & CHUNK_MASK
  }

  appendRange(source: Storage, start: number, end: number): void {
    if (end <= start) return
    const target = this.storage
    adoptColumns(source, target)
    let from = start
    while (from < end) {
      const to = target.length
      const offset = from & CHUNK_MASK
      const rows = Math.min(end - from, CHUNK_ROWS - offset, CHUNK_ROWS - (to & CHUNK_MASK))
      if (offset === 0 && (to & CHUNK_MASK) === 0 && rows === CHUNK_ROWS) shareChunk(source, from, target, to)
      else copyRows(source, from, target, to, rows)
      target.length = to + rows
      from += rows
    }
  }

  // Appends one row: `base` fields, then `overlay` fields where present.
  appendMerged(base: Storage | null, bp: number, overlay: Storage | null, op: number, time: number): void {
    const s = this.storage
    const p = s.length
    s.time.set(p, time, s.rowsHint)
    for (const [source, sp] of [[base, bp], [overlay, op]] as const) {
      if (!source) continue
      for (const [field, column] of source.nums) {
        const value = column.get(sp)
        if (value === value) s.numColumn(field, column.kind).set(p, value, s.rowsHint)
      }
      for (const [field, column] of source.others) {
        const value = column.get(sp)
        if (value !== undefined) s.otherColumn(field).set(p, value)
      }
    }
    s.length = p + 1
  }

  build(): Storage { return this.storage }
}

// Gives `target` every column `source` has, keeping a column `target` already
// has as it is.
function adoptColumns(source: Storage, target: Storage): void {
  for (const [field, column] of source.nums) target.numColumn(field, column.kind)
  for (const field of source.others.keys()) target.otherColumn(field)
}

// Shares the full chunk holding source row `from` as the destination chunk
// holding row `to`; both are chunk-aligned.
function shareChunk(source: Storage, from: number, target: Storage, to: number): void {
  const k = from >> CHUNK_BITS, t = to >> CHUNK_BITS
  const share = <C>(a: { chunks: (C | undefined)[] }, b: { chunks: (C | undefined)[]; owned: boolean[] }) => {
    const chunk = a.chunks[k]
    if (chunk === undefined) return
    b.chunks[t] = chunk
    b.owned[t] = false
  }
  share(source.time, target.time)
  for (const [field, column] of source.nums) share(column, target.numColumn(field, column.kind))
  for (const [field, column] of source.others) share(column, target.otherColumn(field))
}

// Copies `rows` rows from source row `from` to destination row `to`, within
// one chunk on each side.
function copyRows(source: Storage, from: number, target: Storage, to: number, rows: number): void {
  const k = from >> CHUNK_BITS, offset = from & CHUNK_MASK
  const copy = (a: NumColumn, b: NumColumn) => {
    const chunk = a.chunks[k]
    if (chunk === undefined || offset >= chunk.length) return
    const available = Math.min(rows, chunk.length - offset)
    b.writable(to + available - 1, target.rowsHint).set(chunk.subarray(offset, offset + available), to & CHUNK_MASK)
  }
  copy(source.time, target.time)
  for (const [field, column] of source.nums) copy(column, target.numColumn(field, column.kind))
  for (const [field, column] of source.others) {
    const chunk = column.chunks[k]
    if (chunk === undefined) continue
    let out: OtherColumn | null = null
    for (let i = 0; i < rows; i++) {
      const value = chunk[offset + i]
      if (value === undefined) continue
      out ??= target.otherColumn(field)
      out.set(to + i, value)
    }
  }
}

// ── Views ────────────────────────────────────────────────────────────────────

export interface ColumnView<T extends { session_time: number } = { session_time: number }> {
  readonly length: number
  readonly rowType: string
  /** session_time of row i. */
  time(i: number): number
  /** Numeric or boolean (1/0) field of row i; NaN when the row lacks it. */
  num(field: string, i: number): number
  /** Raw field value of row i, booleans as booleans; undefined when absent. */
  value(field: string, i: number): unknown
  /** Materialises row i. Allocates: keep it out of per-sample loops. */
  row(i: number): T
  /** First index whose time is >= t (inclusive) or > t (exclusive). */
  lowerBound(sessionTime: number, inclusive: boolean): number
  slice(start: number, end?: number): ColumnView<T>
}

function storageValue(storage: Storage, field: string, p: number): unknown {
  const column = storage.nums.get(field)
  if (column) {
    const value = column.get(p)
    if (value !== value) return undefined
    return column.kind === BOOL ? value !== 0 : value
  }
  return storage.others.get(field)?.get(p)
}

class FrozenView<T extends { session_time: number }> implements ColumnView<T> {
  constructor(
    private readonly storage: Storage,
    private readonly start: number,
    private readonly end: number,
    readonly rowType: string,
  ) {}
  get length(): number { return this.end - this.start }
  time(i: number): number { return this.storage.time.get(this.start + i) }
  num(field: string, i: number): number {
    const column = this.storage.nums.get(field)
    return column ? column.get(this.start + i) : NaN
  }
  value(field: string, i: number): unknown { return storageValue(this.storage, field, this.start + i) }
  row(i: number): T { return this.storage.materialize(this.rowType, this.start + i) as unknown as T }
  lowerBound(sessionTime: number, inclusive: boolean): number {
    return this.storage.lowerBound(this.start, this.end, sessionTime, inclusive) - this.start
  }
  slice(start: number, end = this.length): ColumnView<T> {
    const s = Math.max(0, Math.min(this.length, start))
    const e = Math.max(s, Math.min(this.length, end))
    return new FrozenView<T>(this.storage, this.start + s, this.start + e, this.rowType)
  }
}

// Follows its table as rows are appended; used for the full-session (All
// Laps) publication, whose charts are woken imperatively instead of through a
// new React value per packet.
class LiveView<T extends { session_time: number }> implements ColumnView<T> {
  constructor(private readonly table: ColumnTable<T>) {}
  get rowType(): string { return this.table.rowType }
  get length(): number { return this.table.length }
  time(i: number): number { const s = this.table.storage; return s.time.get(s.head + i) }
  num(field: string, i: number): number {
    const s = this.table.storage
    const column = s.nums.get(field)
    return column ? column.get(s.head + i) : NaN
  }
  value(field: string, i: number): unknown { const s = this.table.storage; return storageValue(s, field, s.head + i) }
  row(i: number): T { const s = this.table.storage; return s.materialize(this.table.rowType, s.head + i) as unknown as T }
  lowerBound(sessionTime: number, inclusive: boolean): number {
    const s = this.table.storage
    return s.lowerBound(s.head, s.length, sessionTime, inclusive) - s.head
  }
  slice(start: number, end?: number): ColumnView<T> { return this.table.frozen().slice(start, end) }
}

const EMPTY_STORAGE = new Storage()
const emptyViews = new Map<string, ColumnView>()
export function emptyView<T extends { session_time: number }>(rowType = ''): ColumnView<T> {
  let view = emptyViews.get(rowType)
  if (!view) {
    view = new FrozenView<T>(EMPTY_STORAGE, 0, 0, rowType)
    emptyViews.set(rowType, view)
  }
  return view as ColumnView<T>
}

// ── Tables ───────────────────────────────────────────────────────────────────

export class ColumnTable<T extends { session_time: number } = { session_time: number }> {
  /** @internal */ storage: Storage
  private live: LiveView<T> | null = null

  constructor(readonly rowType: string, storage?: Storage) {
    this.storage = storage ?? new Storage()
  }

  get length(): number { return this.storage.length - this.storage.head }

  firstTime(): number | undefined {
    return this.length > 0 ? this.storage.timeAt(this.storage.head) : undefined
  }

  lastTime(): number | undefined {
    return this.length > 0 ? this.storage.timeAt(this.storage.length - 1) : undefined
  }

  /** A snapshot of rows [start, end). */
  frozen(start = 0, end = this.length): ColumnView<T> {
    const s = this.storage
    const from = s.head + Math.max(0, Math.min(this.length, start))
    const to = s.head + Math.max(0, Math.min(this.length, end))
    return new FrozenView<T>(s, from, Math.max(from, to), this.rowType)
  }

  /** A view that grows with the table. Replaced when the storage is. */
  liveView(): ColumnView<T> {
    this.live ??= new LiveView<T>(this)
    return this.live
  }

  last(): T | null {
    return this.length > 0 ? this.storage.materialize(this.rowType, this.storage.length - 1) as unknown as T : null
  }

  lowerBound(sessionTime: number, inclusive: boolean): number {
    const s = this.storage
    return s.lowerBound(s.head, s.length, sessionTime, inclusive) - s.head
  }

  lastNum(field: string): number {
    if (this.length === 0) return NaN
    const column = this.storage.nums.get(field)
    return column ? column.get(this.storage.length - 1) : NaN
  }

  private replace(storage: Storage): void {
    this.storage = storage
    this.live = null
  }

  /**
   * Releases the chunks wholly before the head: the table moves to a storage
   * sharing the rest (same rows, same logical indices, so the live view stays),
   * and the dropped chunks are freed once no earlier view holds them.
   */
  private releaseHead(): void {
    const s = this.storage
    if (s.head < CHUNK_ROWS) return
    this.storage = s.slice(s.head, s.length, true)
  }

  /** Appends a complete row. Fields the row lacks read as absent. */
  append(row: { session_time: number }, maxRows = Infinity): void {
    const s = this.storage
    const p = s.length
    s.time.set(p, row.session_time, s.rowsHint)
    const fields = row as Record<string, unknown>
    for (const key in fields) s.write(p, key, fields[key])
    s.length = p + 1
    this.capRows(maxRows)
  }

  /**
   * Appends a V6 patch: fields it carries overwrite the previous row's, every
   * other field carries forward, and a sample at the newest row's timestamp
   * updates that row. `available:false` clears that type's fields.
   */
  appendPatch(patch: { session_time: number }, maxRows = Infinity): void {
    const fields = patch as Record<string, unknown>
    const v6Type = Number(fields._v6_type)
    const s = this.storage
    const lastIndex = s.length - 1
    if (this.length > 0 && s.timeAt(lastIndex) === patch.session_time) {
      this.applyPatch(s, lastIndex, fields, v6Type)
      return
    }
    const p = s.length
    s.time.set(p, patch.session_time, s.rowsHint)
    if (this.length > 0) s.copyRow(p - 1, p)
    s.length = p + 1
    this.applyPatch(s, p, fields, v6Type)
    this.capRows(maxRows)
  }

  private applyPatch(s: Storage, p: number, patch: Record<string, unknown>, v6Type: number): void {
    if (patch.available === false) {
      for (const field of V6_PATCH_FIELDS[v6Type] ?? []) s.clearField(p, field)
    }
    for (const key in patch) s.write(p, key, patch[key])
  }

  private capRows(maxRows: number): void {
    // Front trim only advances the head; whole chunks behind it are released.
    if (this.length > maxRows + 4096) {
      this.storage.head = this.storage.length - maxRows
      this.releaseHead()
    }
  }

  /** Drops rows before `cutoff` (keeping one predecessor when asked). */
  trimBefore(cutoff: number, preservePredecessor = false): void {
    let start = this.lowerBound(cutoff, true)
    if (preservePredecessor && start > 0) start--
    if (start > 0) {
      this.storage.head += start
      this.releaseHead()
    }
  }

  /** Keeps rows in [from, to) as a new storage; used for rewinds. */
  retainRange(from: number, to: number): void {
    const s = this.storage
    const start = s.lowerBound(s.head, s.length, from, true)
    const end = s.lowerBound(s.head, s.length, to, true)
    this.replace(s.slice(start, Math.max(start, end)))
  }

  /** Keeps rows with time <= target. */
  truncateAt(target: number): void {
    const s = this.storage
    const end = s.lowerBound(s.head, s.length, target, false)
    if (end === s.length) return
    this.replace(s.slice(s.head, end))
  }

  /** Drops every row but the newest. */
  keepLastOnly(): void {
    const s = this.storage
    if (s.length - s.head > 1) {
      s.head = s.length - 1
      this.releaseHead()
    }
  }

  clear(): void {
    this.replace(new Storage())
  }

  /** Replaces the contents with a copy of `view`'s rows. */
  replaceWithView(view: ColumnView): void {
    this.replace(storageOfView(view))
  }

  /** @internal */ replaceStorage(storage: Storage): void {
    this.replace(storage)
  }
}

function storageOf(source: ColumnTable | ColumnView): Storage | null {
  return source instanceof ColumnTable ? source.storage : viewParts(source)?.storage ?? null
}

/**
 * Bytes allocated by the chunks behind a table or view, rows trimmed from the
 * front of a shared chunk included. A chunk (or storage) already in `seen`
 * counts 0, so callers can total several holders without counting shared
 * chunks twice. Non-numeric columns count their reference slots only.
 */
export function columnStorageBytes(source: ColumnTable | ColumnView, seen: Set<object>): number {
  const storage = storageOf(source)
  if (!storage || seen.has(storage)) return 0
  seen.add(storage)
  let bytes = 0
  const count = (chunk: object | undefined, size: number) => {
    if (chunk === undefined || seen.has(chunk)) return
    seen.add(chunk)
    bytes += size
  }
  for (const chunk of storage.time.chunks) count(chunk, chunk?.byteLength ?? 0)
  for (const column of storage.nums.values()) for (const chunk of column.chunks) count(chunk, chunk?.byteLength ?? 0)
  for (const column of storage.others.values()) for (const chunk of column.chunks) count(chunk, (chunk?.length ?? 0) * 8)
  return bytes
}

/** The storages counted by columnStorageBytes calls with this `seen` set. */
export function countColumnStorages(seen: Set<object>): number {
  let count = 0
  for (const entry of seen) if (entry instanceof Storage) count++
  return count
}

function viewParts(view: ColumnView): { storage: Storage; start: number; end: number } | null {
  const frozen = view instanceof LiveView ? view.slice(0) : view
  if (!(frozen instanceof FrozenView)) return null
  const internals = frozen as unknown as { storage: Storage; start: number; end: number }
  return { storage: internals.storage, start: internals.start, end: internals.end }
}

function storageOfView(view: ColumnView): Storage {
  const parts = viewParts(view)
  const builder = new StorageBuilder(view.length)
  if (parts) {
    builder.alignTo(parts.start)
    builder.appendRange(parts.storage, parts.start, parts.end)
  }
  return builder.build()
}

export function tableFromView<T extends { session_time: number }>(view: ColumnView<T>): ColumnTable<T> {
  return new ColumnTable<T>(view.rowType, storageOfView(view))
}

/** Builds a table from row objects, merging V6 patches as they stream. */
export function tableFromRows<T extends { session_time: number }>(
  rowType: string,
  rows: readonly { session_time: number }[],
  mergePatches: boolean,
): ColumnTable<T> {
  const table = new ColumnTable<T>(rowType, new Storage(rows.length))
  for (const row of rows) {
    if (mergePatches && Number.isInteger(Number((row as Record<string, unknown>)._v6_type))) table.appendPatch(row)
    else table.append(row)
  }
  return table
}

export function viewOfRows<T extends { session_time: number }>(
  rowType: string,
  rows: readonly { session_time: number }[],
  mergePatches = true,
): ColumnView<T> {
  return tableFromRows<T>(rowType, rows, mergePatches).frozen()
}

/** `prefix` followed by the rows of `rest` after prefix's last time. */
export function concatAfter<T extends { session_time: number }>(prefix: ColumnView<T>, rest: ColumnView<T>): ColumnView<T> {
  const a = viewParts(prefix), b = viewParts(rest)
  const builder = new StorageBuilder(prefix.length + rest.length)
  if (a) {
    builder.alignTo(a.start)
    builder.appendRange(a.storage, a.start, a.end)
  }
  const lastTime = prefix.length ? prefix.time(prefix.length - 1) : -Infinity
  if (b) builder.appendRange(b.storage, b.start + rest.lowerBound(lastTime, false), b.end)
  const built = builder.build()
  return new FrozenView<T>(built, built.head, built.length, prefix.rowType || rest.rowType)
}

export type InstallMode = 'authoritative' | 'prefix' | 'overlay' | 'replaceRange'

/**
 * Installs a decoded history range next to already-held rows.
 *   authoritative: the incoming range, then held rows newer than it.
 *   overlay: time-ordered union; at a shared timestamp the held row is kept
 *            and the incoming fields are laid over it (V6 patch history).
 *   prefix: incoming rows older than the held rows, then the held rows.
 *   replaceRange: held rows before `range.from`, the incoming rows inside
 *            [from, through], then held rows after `range.through`. An empty
 *            incoming view still clears the held rows inside the range.
 * The result keeps at most `maxRows` newest rows. Whole chunks of the first
 * part are shared with its source rather than copied.
 */
export function installHistory<T extends { session_time: number }>(
  table: ColumnTable<T>,
  incoming: ColumnView<T>,
  mode: InstallMode,
  maxRows: number,
  range?: { from: number; through: number },
): void {
  const held = table.frozen()
  const inc = viewParts(incoming)
  const old = viewParts(held)
  const builder = new StorageBuilder(incoming.length + held.length)
  if (mode === 'replaceRange') {
    const from = range?.from ?? -Infinity
    const through = range?.through ?? Infinity
    if (old) {
      builder.alignTo(old.start)
      builder.appendRange(old.storage, old.start, old.start + held.lowerBound(from, true))
    } else if (inc) {
      builder.alignTo(inc.start + incoming.lowerBound(from, true))
    }
    if (inc) builder.appendRange(inc.storage, inc.start + incoming.lowerBound(from, true),
      inc.start + incoming.lowerBound(through, false))
    if (old) builder.appendRange(old.storage, old.start + held.lowerBound(through, false), old.end)
  } else if (mode === 'authoritative' || held.length === 0) {
    if (inc) {
      builder.alignTo(inc.start)
      builder.appendRange(inc.storage, inc.start, inc.end)
    }
    if (old && mode === 'authoritative' && incoming.length > 0) {
      const lastTime = incoming.time(incoming.length - 1)
      builder.appendRange(old.storage, old.start + held.lowerBound(lastTime, false), old.end)
    } else if (old && incoming.length === 0 && mode === 'authoritative') {
      builder.alignTo(old.start)
      builder.appendRange(old.storage, old.start, old.end)
    }
  } else if (mode === 'prefix') {
    const firstHeld = held.time(0)
    if (inc) {
      builder.alignTo(inc.start)
      builder.appendRange(inc.storage, inc.start, inc.start + incoming.lowerBound(firstHeld, true))
    }
    if (old) builder.appendRange(old.storage, old.start, old.end)
  } else if (inc && old) {
    let i = 0, j = 0
    while (i < incoming.length || j < held.length) {
      const ti = i < incoming.length ? incoming.time(i) : Infinity
      const tj = j < held.length ? held.time(j) : Infinity
      if (tj < ti) {
        // Contiguous runs of held-only rows copy column-wise.
        let k = j + 1
        while (k < held.length && held.time(k) < ti) k++
        if (builder.length === 0) builder.alignTo(old.start + j)
        builder.appendRange(old.storage, old.start + j, old.start + k)
        j = k
      } else if (ti < tj) {
        let k = i + 1
        while (k < incoming.length && incoming.time(k) < tj) k++
        if (builder.length === 0) builder.alignTo(inc.start + i)
        builder.appendRange(inc.storage, inc.start + i, inc.start + k)
        i = k
      } else {
        builder.appendMerged(old.storage, old.start + j, inc.storage, inc.start + i, ti)
        i++; j++
      }
    }
  }
  let built = builder.build()
  if (built.length - built.head > maxRows) built.head = built.length - maxRows
  if (built.head >= CHUNK_ROWS) built = built.slice(built.head, built.length, true)
  table.replaceStorage(built)
}

// ── V6 columnar history payload ─────────────────────────────────────────────
// Produced by TnrdV6Archive::columnarHistory (protocol_parser_library
// src/tnrd/TNRD_V6.cpp, V6_HISTORY_MAGIC), carried in the seek flush's binary
// buffer. One block per V6 type; the blocks of a family are joined here by
// time exactly as the old path merged the equivalent JSON patches.

const V6_HISTORY_MAGIC = 0x31483656

const FAMILY_BIT: Record<HistoryFamily, number> = {
  telemetry: 1 << 1, status: 1 << 2, damage: 1 << 3, lap: 1 << 4, motion: 1 << 11, motion_ex: 1 << 12,
}

function v6FieldFamily(type: number, field: string): HistoryFamily | null {
  if (type === 7) return field === 'drs_allowed' ? 'status' : 'telemetry'
  if (type === 13) return field === 'sets' || field === 'fitted_idx' ? null : 'status'
  if (type >= 1 && type <= 11) return 'telemetry'
  if (type === 12 || type === 14) return 'damage'
  if (type >= 15 && type <= 20) return 'status'
  if (type === 21) return 'motion'
  if (type === 22) return 'motion_ex'
  if (type === 24) return 'lap'
  return null
}

interface HistoryField { name: string; bool: boolean; values: Float64Array }
interface HistoryBlock { type: number; time: Float64Array; fields: HistoryField[]; available: Float64Array | null }

function nanArray(length: number): Float64Array {
  const out = new Float64Array(length)
  out.fill(NaN)
  return out
}

export function isV6HistoryPayload(bytes: Uint8Array | null | undefined): boolean {
  if (!bytes || bytes.byteLength < 12) return false
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  return view.getUint32(0, true) === V6_HISTORY_MAGIC
}

// Called between slices of decode work. Resolves false to abandon the decode.
export type DecodePause = () => Promise<boolean>

class DecodeCancelled extends Error {}

function pacer(pause: DecodePause | undefined): (work: number) => Promise<void> {
  let budget = 0
  return async (work: number) => {
    budget += work
    if (!pause || budget < 60_000) return
    budget = 0
    if (!(await pause())) throw new DecodeCancelled()
  }
}

async function parseV6History(bytes: Uint8Array, step: (work: number) => Promise<void>): Promise<{ mask: number; blocks: HistoryBlock[] }> {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const decoder = new TextDecoder()
  let at = 0
  const need = (n: number) => { if (at + n > bytes.byteLength) throw new Error('truncated V6 history payload') }
  need(12)
  if (view.getUint32(0, true) !== V6_HISTORY_MAGIC) throw new Error('not a V6 history payload')
  const mask = view.getUint32(4, true) >>> 0
  const blockCount = view.getUint32(8, true)
  at = 12
  const blocks: HistoryBlock[] = []
  for (let b = 0; b < blockCount; b++) {
    need(8)
    const type = view.getUint8(at)
    const fieldCount = view.getUint16(at + 2, true)
    const rows = view.getUint32(at + 4, true)
    at += 8
    need(rows * 4)
    const time = new Float64Array(rows)
    for (let r = 0; r < rows; r++, at += 4) time[r] = view.getFloat32(at, true)
    const fields: HistoryField[] = []
    let available: Float64Array | null = null
    for (let f = 0; f < fieldCount; f++) {
      need(1)
      const nameLength = view.getUint8(at++)
      need(nameLength + 2)
      const name = decoder.decode(bytes.subarray(at, at + nameLength))
      at += nameLength
      const kind = view.getUint8(at++)
      const dense = view.getUint8(at++) !== 0
      let present: Uint8Array | null = null
      if (!dense) {
        const bitmapBytes = (rows + 7) >> 3
        need(bitmapBytes)
        present = bytes.subarray(at, at + bitmapBytes)
        at += bitmapBytes
      }
      const width = kind === 1 ? 8 : kind === 3 ? 1 : 4
      const values = nanArray(rows)
      for (let r = 0; r < rows; r++) {
        if (present && !((present[r >> 3] >> (r & 7)) & 1)) continue
        need(width)
        values[r] = kind === 0 ? view.getFloat32(at, true)
          : kind === 1 ? view.getFloat64(at, true)
          : kind === 2 ? view.getInt32(at, true)
          : view.getUint8(at)
        at += width
      }
      if (name === 'available') available = values
      else fields.push({ name, bool: kind === 3, values })
      await step(rows)
    }
    blocks.push({ type, time, fields, available })
  }
  return { mask, blocks }
}

function mergeSortedUnique(a: Float64Array, b: Float64Array): Float64Array {
  const out = new Float64Array(a.length + b.length)
  let i = 0, j = 0, n = 0
  let last = NaN
  while (i < a.length || j < b.length) {
    const v = j >= b.length || (i < a.length && a[i] <= b[j]) ? a[i++] : b[j++]
    if (v !== last) { out[n++] = v; last = v }
  }
  return out.subarray(0, n)
}

export interface V6HistoryTables {
  mask: number
  tables: Partial<Record<HistoryFamily, ColumnTable>>
  typeCounts: Record<string, number>
}

/** A decoded payload as plain chunk arrays, which can cross to another thread. */
export interface DecodedV6History {
  mask: number
  typeCounts: Record<string, number>
  families: Array<{
    family: HistoryFamily
    rows: number
    time: Array<NumChunk | undefined>
    nums: Array<{ name: string; kind: NumericKind; chunks: Array<NumChunk | undefined> }>
  }>
}

/**
 * Decodes a payload to chunks, for a decoder on another thread. Every chunk is
 * its own ArrayBuffer, so the result can be transferred without copying.
 */
export async function decodeV6HistoryChunks(bytes: Uint8Array): Promise<DecodedV6History> {
  const decoded = await decodeV6HistoryInner(bytes, async () => {})
  const families: DecodedV6History['families'] = []
  for (const [family, table] of Object.entries(decoded.tables) as [HistoryFamily, ColumnTable][]) {
    const s = table.storage
    families.push({
      family,
      rows: s.length,
      time: s.time.chunks,
      nums: [...s.nums].map(([name, column]) => ({ name, kind: column.kind, chunks: column.chunks })),
    })
  }
  return { mask: decoded.mask, typeCounts: decoded.typeCounts, families }
}

/** The ArrayBuffers of a decodeV6HistoryChunks result, to transfer it. */
export function decodedV6HistoryBuffers(decoded: DecodedV6History): ArrayBuffer[] {
  const buffers: ArrayBuffer[] = []
  const add = (chunk: NumChunk | undefined) => { if (chunk) buffers.push(chunk.buffer as ArrayBuffer) }
  for (const family of decoded.families) {
    family.time.forEach(add)
    for (const column of family.nums) column.chunks.forEach(add)
  }
  return buffers
}

function tablesOfDecoded(decoded: DecodedV6History): V6HistoryTables {
  const tables: Partial<Record<HistoryFamily, ColumnTable>> = {}
  for (const family of decoded.families) {
    const storage = new Storage(family.rows)
    storage.length = family.rows
    storage.time.chunks = family.time
    storage.time.owned = family.time.map(() => true)
    for (const { name, kind, chunks } of family.nums) {
      const column = storage.numColumn(name, kind)
      column.chunks = chunks
      column.owned = chunks.map(() => true)
    }
    tables[family.family] = new ColumnTable(family.family, storage)
  }
  return { mask: decoded.mask, tables, typeCounts: decoded.typeCounts }
}

/** Decodes payloads elsewhere (a worker); throws to fall back to this thread. */
export type HistoryDecoder = (bytes: Uint8Array) => Promise<DecodedV6History>
let historyDecoder: HistoryDecoder | null = null

export function setHistoryDecoder(decoder: HistoryDecoder | null): void {
  historyDecoder = decoder
}

/**
 * Decodes a V6H1 payload into one table per history family it covers. With
 * `pause`, the work is split into slices; null means the pause abandoned it.
 * With a decoder installed, the work runs there and `pause` is asked once,
 * when the result arrives.
 */
export async function decodeV6History(bytes: Uint8Array, pause?: DecodePause): Promise<V6HistoryTables | null> {
  if (historyDecoder) {
    let decoded: DecodedV6History | null = null
    try {
      decoded = await historyDecoder(bytes)
    } catch (error) {
      // A decoder failure (or a bad payload) falls back to the in-thread
      // decode below, which reports a bad payload the way it always did.
      console.warn('[history-decode] worker decode failed; decoding in-thread:', error)
    }
    if (decoded) {
      if (pause && !(await pause())) return null
      return tablesOfDecoded(decoded)
    }
  }
  try {
    return await decodeV6HistoryInner(bytes, pacer(pause))
  } catch (error) {
    if (error instanceof DecodeCancelled) return null
    throw error
  }
}

async function decodeV6HistoryInner(bytes: Uint8Array, step: (work: number) => Promise<void>): Promise<V6HistoryTables> {
  const { mask, blocks } = await parseV6History(bytes, step)
  const typeCounts: Record<string, number> = {}
  for (const block of blocks) typeCounts[String(block.type)] = block.time.length
  const tables: Partial<Record<HistoryFamily, ColumnTable>> = {}
  for (const family of Object.keys(FAMILY_BIT) as HistoryFamily[]) {
    if (!(mask & FAMILY_BIT[family])) continue
    const tracks = blocks.flatMap(block => {
      const fields = block.fields.filter(field => v6FieldFamily(block.type, field.name) === family)
      if (fields.length === 0 && !(block.available && v6FieldFamily(block.type, V6_PATCH_FIELDS[block.type]?.[0] ?? '') === family)) return []
      const drops = (V6_PATCH_FIELDS[block.type] ?? []).filter(name => v6FieldFamily(block.type, name) === family)
      return [{ block, fields, drops }]
    })
    if (tracks.length === 0) continue

    let times: Float64Array = new Float64Array(0)
    for (const track of tracks) times = mergeSortedUnique(times, track.block.time)
    const rows = times.length
    const storage = new Storage(rows)
    for (let r = 0; r < rows; r++) storage.time.set(r, times[r], rows)
    storage.length = rows
    const fieldNames: string[] = []
    const fieldIndex = new Map<string, number>()
    const addField = (name: string, bool: boolean) => {
      if (fieldIndex.has(name)) return
      fieldIndex.set(name, fieldNames.length)
      fieldNames.push(name)
      storage.numColumn(name, bool ? BOOL : NUM)
    }
    for (const track of tracks) {
      for (const field of track.fields) addField(field.name, field.bool)
      for (const name of track.drops) if (!fieldIndex.has(name)) fieldIndex.set(name, -1)
    }
    const columns = fieldNames.map(name => storage.nums.get(name)!)
    const state = new Float64Array(fieldNames.length).fill(NaN)
    const cursors = new Int32Array(tracks.length)
    const trackColumns = tracks.map(track => track.fields.map(field => fieldIndex.get(field.name)!))
    const trackDrops = tracks.map(track => track.drops.map(name => fieldIndex.get(name)!).filter(index => index >= 0))

    for (let r = 0; r < rows; r++) {
      const t = times[r]
      for (let k = 0; k < tracks.length; k++) {
        const { block } = tracks[k]
        let p = cursors[k]
        while (p < block.time.length && block.time[p] <= t) {
          if (block.available && block.available[p] === 0) {
            for (const index of trackDrops[k]) state[index] = NaN
          }
          const fields = tracks[k].fields
          const indices = trackColumns[k]
          for (let f = 0; f < fields.length; f++) {
            const value = fields[f].values[p]
            if (value === value) state[indices[f]] = value
          }
          p++
        }
        cursors[k] = p
      }
      // Absent values stay unwritten: a column only gets the chunks it uses.
      for (let c = 0; c < columns.length; c++) {
        const value = state[c]
        if (value === value) columns[c].set(r, value, rows)
      }
      if ((r & 4095) === 4095) await step(4096 * (columns.length + tracks.length))
    }
    tables[family] = new ColumnTable(family, storage)
  }
  return { mask, tables, typeCounts }
}

// ── Chart helpers ────────────────────────────────────────────────────────────

/** The X accessor of time-axis charts. */
export function sessionTimeAt(rows: ColumnView, i: number): number {
  return rows.time(i)
}

/**
 * Columns for a chart's table view: session time followed by one column per
 * accessor, evaluated for every row of `rows`.
 */
export function alignedFromView<T extends { session_time: number }>(
  rows: ColumnView<T>,
  accessors: readonly ((rows: ColumnView<T>, i: number) => number)[],
): Float64Array[] {
  const n = rows.length
  const ts = new Float64Array(n)
  const values = accessors.map(() => new Float64Array(n))
  for (let i = 0; i < n; i++) {
    ts[i] = rows.time(i)
    for (let k = 0; k < accessors.length; k++) values[k][i] = accessors[k](rows, i)
  }
  return [ts, ...values]
}
