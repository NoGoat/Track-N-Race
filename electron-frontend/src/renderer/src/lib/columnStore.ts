// ─────────────────────────────────────────────────────────────────────────────
// Columnar history store.
//
// Every renderer history family (telemetry, motion, motion_ex, status, damage,
// lap) is held as typed column arrays rather than an array of row objects: one
// Float64Array of session times plus one Float64Array per numeric/boolean field
// (NaN = the row does not carry that field). Charts, tables and lap analysis
// read samples by index through ColumnView; row objects are only materialised
// for the few consumers that want one sample (cards, tooltips).
//
// Storage is append-only. Anything that would rewrite existing rows (a rewind,
// a history merge, a copy) builds a new Storage instead, so a frozen view taken
// earlier keeps reading exactly the rows it was published with. The one
// exception is the V6 patch merge into the newest row at the same timestamp,
// which the old row-array path also made visible to already-published arrays.
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
    'drs_fault', 'ers_fault', 'blisters_fl', 'blisters_fr', 'blisters_rl', 'blisters_rr'],
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

function nanArray(capacity: number): Float64Array {
  const out = new Float64Array(capacity)
  out.fill(NaN)
  return out
}

class Storage {
  head = 0
  length = 0
  readonly time: Float64Array
  readonly nums = new Map<string, Float64Array>()
  readonly kinds = new Map<string, NumericKind>()
  readonly others = new Map<string, unknown[]>()

  constructor(readonly capacity: number) {
    this.time = new Float64Array(capacity)
  }

  numColumn(field: string, kind: NumericKind): Float64Array {
    let column = this.nums.get(field)
    if (!column) {
      column = nanArray(this.capacity)
      this.nums.set(field, column)
      this.kinds.set(field, kind)
    }
    return column
  }

  otherColumn(field: string): unknown[] {
    let column = this.others.get(field)
    if (!column) {
      column = []
      this.others.set(field, column)
    }
    return column
  }

  write(p: number, field: string, value: unknown): void {
    if (value === undefined || value === null || META_FIELDS.has(field)) return
    if (typeof value === 'number') this.numColumn(field, NUM)[p] = value
    else if (typeof value === 'boolean') this.numColumn(field, BOOL)[p] = value ? 1 : 0
    else this.otherColumn(field)[p] = value
  }

  clearField(p: number, field: string): void {
    const column = this.nums.get(field)
    if (column) column[p] = NaN
    const other = this.others.get(field)
    if (other && p < other.length) other[p] = undefined
  }

  copyRow(from: number, to: number): void {
    for (const column of this.nums.values()) column[to] = column[from]
    for (const column of this.others.values()) if (from < column.length) column[to] = column[from]
  }

  lowerBound(start: number, end: number, sessionTime: number, inclusive: boolean): number {
    let lo = start, hi = end
    while (lo < hi) {
      const mid = (lo + hi) >> 1
      const t = this.time[mid]
      if (inclusive ? t < sessionTime : t <= sessionTime) lo = mid + 1
      else hi = mid
    }
    return lo
  }

  materialize(rowType: string, p: number): Record<string, unknown> {
    const out: Record<string, unknown> = { type: rowType, session_time: this.time[p] }
    for (const [field, column] of this.nums) {
      const value = column[p]
      if (value === value) out[field] = this.kinds.get(field) === BOOL ? value !== 0 : value
    }
    for (const [field, column] of this.others) {
      const value = p < column.length ? column[p] : undefined
      if (value !== undefined) out[field] = value
    }
    return out
  }
}

// Copies rows into a fresh Storage, taking the union of the sources' columns.
class StorageBuilder {
  private storage: Storage
  constructor(capacity: number) {
    this.storage = new Storage(Math.max(16, capacity))
  }

  get length(): number { return this.storage.length }

  lastTime(): number {
    return this.storage.length > 0 ? this.storage.time[this.storage.length - 1] : -Infinity
  }

  private ensure(extra: number): Storage {
    const s = this.storage
    if (s.length + extra <= s.capacity) return s
    const grown = new Storage(Math.max(s.capacity * 2, s.length + extra))
    copyRange(s, 0, s.length, grown)
    this.storage = grown
    return grown
  }

  appendRange(source: Storage, start: number, end: number): void {
    if (end <= start) return
    copyRange(source, start, end, this.ensure(end - start))
  }

  // Appends one row: `base` fields, then `overlay` fields where present.
  appendMerged(base: Storage | null, bp: number, overlay: Storage | null, op: number, time: number): void {
    const s = this.ensure(1)
    const p = s.length
    s.time[p] = time
    for (const [source, sp] of [[base, bp], [overlay, op]] as const) {
      if (!source) continue
      for (const [field, column] of source.nums) {
        const value = column[sp]
        if (value === value) s.numColumn(field, source.kinds.get(field) ?? NUM)[p] = value
      }
      for (const [field, column] of source.others) {
        const value = sp < column.length ? column[sp] : undefined
        if (value !== undefined) s.otherColumn(field)[p] = value
      }
    }
    s.length = p + 1
  }

  build(): Storage { return this.storage }
}

function copyRange(source: Storage, start: number, end: number, target: Storage): void {
  const count = end - start
  const at = target.length
  target.time.set(source.time.subarray(start, end), at)
  for (const [field, column] of source.nums) {
    target.numColumn(field, source.kinds.get(field) ?? NUM).set(column.subarray(start, end), at)
  }
  for (const [field, column] of source.others) {
    const out = target.otherColumn(field)
    for (let i = 0; i < count; i++) {
      const value = start + i < column.length ? column[start + i] : undefined
      if (value !== undefined) out[at + i] = value
    }
  }
  target.length = at + count
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

class FrozenView<T extends { session_time: number }> implements ColumnView<T> {
  constructor(
    private readonly storage: Storage,
    private readonly start: number,
    private readonly end: number,
    readonly rowType: string,
  ) {}
  get length(): number { return this.end - this.start }
  time(i: number): number { return this.storage.time[this.start + i] }
  num(field: string, i: number): number {
    const column = this.storage.nums.get(field)
    return column ? column[this.start + i] : NaN
  }
  value(field: string, i: number): unknown {
    const p = this.start + i
    const column = this.storage.nums.get(field)
    if (column) {
      const value = column[p]
      if (value !== value) return undefined
      return this.storage.kinds.get(field) === BOOL ? value !== 0 : value
    }
    const other = this.storage.others.get(field)
    return other && p < other.length ? other[p] : undefined
  }
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
  time(i: number): number { const s = this.table.storage; return s.time[s.head + i] }
  num(field: string, i: number): number {
    const s = this.table.storage
    const column = s.nums.get(field)
    return column ? column[s.head + i] : NaN
  }
  value(field: string, i: number): unknown { return this.table.frozen().value(field, i) }
  row(i: number): T { const s = this.table.storage; return s.materialize(this.table.rowType, s.head + i) as unknown as T }
  lowerBound(sessionTime: number, inclusive: boolean): number {
    const s = this.table.storage
    return s.lowerBound(s.head, s.length, sessionTime, inclusive) - s.head
  }
  slice(start: number, end?: number): ColumnView<T> { return this.table.frozen().slice(start, end) }
}

const EMPTY_STORAGE = new Storage(0)
const emptyViews = new Map<string, ColumnView<any>>()
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
    this.storage = storage ?? new Storage(256)
  }

  get length(): number { return this.storage.length - this.storage.head }

  firstTime(): number | undefined {
    return this.length > 0 ? this.storage.time[this.storage.head] : undefined
  }

  lastTime(): number | undefined {
    return this.length > 0 ? this.storage.time[this.storage.length - 1] : undefined
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
    return column ? column[this.storage.length - 1] : NaN
  }

  private replace(storage: Storage): void {
    this.storage = storage
    this.live = null
  }

  private reserve(): Storage {
    const s = this.storage
    if (s.length < s.capacity) return s
    const size = s.length - s.head
    const grown = new Storage(Math.max(256, size * 2))
    copyRange(s, s.head, s.length, grown)
    this.storage = grown // same rows, same logical indices: the live view stays
    return grown
  }

  /** Appends a complete row. Fields the row lacks read as absent. */
  append(row: Record<string, unknown> & { session_time: number }, maxRows = Infinity): void {
    const s = this.reserve()
    const p = s.length
    s.time[p] = row.session_time
    for (const key in row) s.write(p, key, row[key])
    s.length = p + 1
    this.capRows(maxRows)
  }

  /**
   * Appends a V6 patch: fields it carries overwrite the previous row's, every
   * other field carries forward, and a sample at the newest row's timestamp
   * updates that row. `available:false` clears that type's fields.
   */
  appendPatch(patch: Record<string, unknown> & { session_time: number }, maxRows = Infinity): void {
    const v6Type = Number(patch._v6_type)
    const s0 = this.storage
    const lastIndex = s0.length - 1
    if (this.length > 0 && s0.time[lastIndex] === patch.session_time) {
      this.applyPatch(s0, lastIndex, patch, v6Type)
      return
    }
    const s = this.reserve()
    const p = s.length
    s.time[p] = patch.session_time
    if (this.length > 0) s.copyRow(p - 1, p)
    s.length = p + 1
    this.applyPatch(s, p, patch, v6Type)
    this.capRows(maxRows)
  }

  private applyPatch(s: Storage, p: number, patch: Record<string, unknown>, v6Type: number): void {
    if (patch.available === false) {
      for (const field of V6_PATCH_FIELDS[v6Type] ?? []) s.clearField(p, field)
    }
    for (const key in patch) s.write(p, key, patch[key])
  }

  private capRows(maxRows: number): void {
    // Front trim only advances the head; the tail storage is compacted on the
    // next growth. Views taken earlier keep their own physical range.
    if (this.length > maxRows + 4096) this.storage.head = this.storage.length - maxRows
  }

  /** Drops rows before `cutoff` (keeping one predecessor when asked). */
  trimBefore(cutoff: number, preservePredecessor = false): void {
    let start = this.lowerBound(cutoff, true)
    if (preservePredecessor && start > 0) start--
    if (start > 0) this.storage.head += start
  }

  /** Keeps rows in [from, to) as a new storage; used for rewinds. */
  retainRange(from: number, to: number): void {
    const s = this.storage
    const start = s.lowerBound(s.head, s.length, from, true)
    const end = s.lowerBound(s.head, s.length, to, true)
    const next = new Storage(Math.max(256, (end - start) * 2))
    copyRange(s, start, Math.max(start, end), next)
    this.replace(next)
  }

  /** Keeps rows with time <= target. */
  truncateAt(target: number): void {
    const s = this.storage
    const end = s.lowerBound(s.head, s.length, target, false)
    if (end === s.length) return
    const next = new Storage(Math.max(256, (end - s.head) * 2))
    copyRange(s, s.head, end, next)
    this.replace(next)
  }

  /** Drops every row but the newest (a head advance; nothing is copied). */
  keepLastOnly(): void {
    const s = this.storage
    if (s.length - s.head > 1) s.head = s.length - 1
  }

  clear(): void {
    this.replace(new Storage(256))
  }

  /** Replaces the contents with a copy of `view`'s rows. */
  replaceWithView(view: ColumnView<any>): void {
    this.replace(storageOfView(view))
  }

  /** @internal */ replaceStorage(storage: Storage): void {
    this.replace(storage)
  }
}

function viewParts(view: ColumnView<any>): { storage: Storage; start: number; end: number } | null {
  const frozen = view instanceof LiveView ? view.slice(0) : view
  if (!(frozen instanceof FrozenView)) return null
  const internals = frozen as unknown as { storage: Storage; start: number; end: number }
  return { storage: internals.storage, start: internals.start, end: internals.end }
}

function storageOfView(view: ColumnView<any>): Storage {
  const parts = viewParts(view)
  const builder = new StorageBuilder(Math.max(256, view.length * 2))
  if (parts) builder.appendRange(parts.storage, parts.start, parts.end)
  return builder.build()
}

export function tableFromView<T extends { session_time: number }>(view: ColumnView<T>): ColumnTable<T> {
  return new ColumnTable<T>(view.rowType, storageOfView(view))
}

/** Builds a table from row objects, merging V6 patches as they stream. */
export function tableFromRows<T extends { session_time: number }>(
  rowType: string,
  rows: readonly Record<string, any>[],
  mergePatches: boolean,
): ColumnTable<T> {
  const table = new ColumnTable<T>(rowType)
  for (const row of rows) {
    if (mergePatches && Number.isInteger(Number(row._v6_type))) table.appendPatch(row as T & Record<string, unknown>)
    else table.append(row as T & Record<string, unknown>)
  }
  return table
}

export function viewOfRows<T extends { session_time: number }>(
  rowType: string,
  rows: readonly Record<string, any>[],
  mergePatches = true,
): ColumnView<T> {
  return tableFromRows<T>(rowType, rows, mergePatches).frozen()
}

/** `prefix` followed by the rows of `rest` after prefix's last time. */
export function concatAfter<T extends { session_time: number }>(prefix: ColumnView<T>, rest: ColumnView<T>): ColumnView<T> {
  const a = viewParts(prefix), b = viewParts(rest)
  const builder = new StorageBuilder(prefix.length + rest.length)
  if (a) builder.appendRange(a.storage, a.start, a.end)
  const lastTime = prefix.length ? prefix.time(prefix.length - 1) : -Infinity
  if (b) builder.appendRange(b.storage, b.start + rest.lowerBound(lastTime, false), b.end)
  return new FrozenView<T>(builder.build(), 0, builder.length, prefix.rowType || rest.rowType)
}

export type InstallMode = 'authoritative' | 'prefix' | 'overlay'

/**
 * Installs a decoded history range next to already-held rows.
 *   authoritative: the incoming range, then held rows newer than it.
 *   overlay: time-ordered union; at a shared timestamp the held row is kept
 *            and the incoming fields are laid over it (V6 patch history).
 *   prefix: incoming rows older than the held rows, then the held rows.
 * The result keeps at most `maxRows` newest rows.
 */
export function installHistory<T extends { session_time: number }>(
  table: ColumnTable<T>,
  incoming: ColumnView<T>,
  mode: InstallMode,
  maxRows: number,
): void {
  const held = table.frozen()
  const inc = viewParts(incoming)
  const old = viewParts(held)
  const builder = new StorageBuilder(incoming.length + held.length)
  if (mode === 'authoritative' || held.length === 0) {
    if (inc) builder.appendRange(inc.storage, inc.start, inc.end)
    if (old && mode === 'authoritative' && incoming.length > 0) {
      const lastTime = incoming.time(incoming.length - 1)
      builder.appendRange(old.storage, old.start + held.lowerBound(lastTime, false), old.end)
    } else if (old && incoming.length === 0 && mode === 'authoritative') {
      builder.appendRange(old.storage, old.start, old.end)
    }
  } else if (mode === 'prefix') {
    const firstHeld = held.time(0)
    if (inc) builder.appendRange(inc.storage, inc.start, inc.start + incoming.lowerBound(firstHeld, true))
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
        builder.appendRange(old.storage, old.start + j, old.start + k)
        j = k
      } else if (ti < tj) {
        let k = i + 1
        while (k < incoming.length && incoming.time(k) < tj) k++
        builder.appendRange(inc.storage, inc.start + i, inc.start + k)
        i = k
      } else {
        builder.appendMerged(old.storage, old.start + j, inc.storage, inc.start + i, ti)
        i++; j++
      }
    }
  }
  const built = builder.build()
  if (built.length - built.head > maxRows) built.head = built.length - maxRows
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
  tables: Partial<Record<HistoryFamily, ColumnTable<any>>>
  typeCounts: Record<string, number>
}

/**
 * Decodes a V6H1 payload into one table per history family it covers. With
 * `pause`, the work is split into slices; null means the pause abandoned it.
 */
export async function decodeV6History(bytes: Uint8Array, pause?: DecodePause): Promise<V6HistoryTables | null> {
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
  const tables: Partial<Record<HistoryFamily, ColumnTable<any>>> = {}
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
    const storage = new Storage(Math.max(16, rows))
    storage.time.set(times)
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
      for (let c = 0; c < columns.length; c++) columns[c][r] = state[c]
      if ((r & 4095) === 4095) await step(4096 * (columns.length + tracks.length))
    }
    tables[family] = new ColumnTable(family, storage)
  }
  return { mask, tables, typeCounts }
}

// ── Chart helpers ────────────────────────────────────────────────────────────

/** The X accessor of time-axis charts. */
export function sessionTimeAt(rows: ColumnView<any>, i: number): number {
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
