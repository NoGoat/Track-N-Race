package com.nogoat.tracknrace

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** One family's samples from a history payload, oldest first: [columns] by field slot, NaN where a row left a field out. */
internal class HistorySamples(val times: DoubleArray, val columns: Array<FloatArray>)

/** A history payload's lap rows, oldest first, each field holding its last value as the live lap rows are merged. */
internal class HistoryLapRows(
    val times: DoubleArray,
    val lapNumbers: IntArray,
    val currentLapMs: IntArray,
    val metres: DoubleArray,
    val invalid: BooleanArray,
    val lastLapMs: IntArray,
)

/** What a history payload holds for the graph histories. */
internal class LiveHistory(val samples: Map<GraphFamily, HistorySamples>, val laps: HistoryLapRows?)

/**
 * Reads libtnrp's columnar history ("V6H1", TnrdV6Archive::columnarHistory):
 * what the engine returns for the live session it keeps, one block of rows
 * per V6 type. Little endian:
 *
 *   u32 magic, u32 row family mask, u32 block count; per block: u8 V6 type,
 *   u8 reserved, u16 field count, u32 row count, f32[row count] session
 *   times ascending; per field: u8 name length, name, u8 kind (0 f32, 1 f64,
 *   2 i32, 3 bool), u8 dense, a presence bitmap when not dense, then one
 *   value per present row.
 *
 * Keep this in lockstep with protocol_parser_library/src/tnrd/TNRD_V6.cpp.
 * A family's samples are timed by the rows of its row family, as a recorded
 * lap's are; a field from another row family takes its newest value at each.
 */
internal object V6HistoryDecoder {
    private const val MAGIC = 0x31483656 // "V6H1"
    private const val AERO_TYPE = 7
    private const val LAP_TYPE = 24
    // An aero row naming this is a status row; any other is a telemetry row.
    private const val STATUS_AERO_FIELD = "drs_allowed"

    /** [present] and [data] are payload offsets; [present] is -1 when every row has the field. */
    private class Field(val name: String, val kind: Int, val present: Int, val data: Int)

    private class Block(val type: Int, val rows: Int, val times: Int, val fields: List<Field>)

    /** The [families]' samples and the lap rows in [payload]; null when it is not a history payload. */
    fun decode(payload: ByteBuffer, families: Collection<GraphFamily>): LiveHistory? {
        val bytes = payload.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        return try {
            val blocks = parse(bytes) ?: return null
            val samples = HashMap<GraphFamily, HistorySamples>()
            for (family in families) samples(bytes, blocks, family)?.let { samples[family] = it }
            LiveHistory(samples, blocks.firstOrNull { it.type == LAP_TYPE }?.let { lapRows(bytes, it) })
        } catch (_: IndexOutOfBoundsException) {
            // A payload cut short.
            null
        }
    }

    private fun parse(bytes: ByteBuffer): List<Block>? {
        val length = bytes.limit().toLong()
        if (length < 12 || bytes.getInt(0) != MAGIC) return null
        val count = bytes.getInt(8)
        val blocks = ArrayList<Block>()
        var at = 12L
        for (index in 0 until count) {
            if (at + 8 > length) return null
            val head = at.toInt()
            val type = bytes.get(head).toInt() and 0xFF
            val fieldCount = bytes.getShort(head + 2).toInt() and 0xFFFF
            val rows = bytes.getInt(head + 4)
            if (rows < 0) return null
            val times = at + 8
            at = times + rows * 4L
            val fields = ArrayList<Field>(fieldCount)
            for (field in 0 until fieldCount) {
                if (at + 1 > length) return null
                val nameLength = bytes.get(at.toInt()).toInt() and 0xFF
                if (at + 3 + nameLength > length) return null
                val name = ByteArray(nameLength) { bytes.get(at.toInt() + 1 + it) }.toString(Charsets.UTF_8)
                at += 1 + nameLength
                val kind = bytes.get(at.toInt()).toInt() and 0xFF
                val dense = bytes.get(at.toInt() + 1).toInt() != 0
                at += 2
                var present = -1
                var values = rows.toLong()
                if (!dense) {
                    val bitmap = (rows + 7L) / 8
                    if (at + bitmap > length) return null
                    present = at.toInt()
                    values = 0
                    for (byte in 0 until bitmap.toInt()) values += Integer.bitCount(bytes.get(present + byte).toInt() and 0xFF)
                    at += bitmap
                }
                val width = valueWidth(kind) ?: return null
                fields += Field(name, kind, present, at.toInt())
                at += values * width
            }
            if (at > length) return null
            blocks += Block(type, rows, times.toInt(), fields)
        }
        return blocks
    }

    private fun valueWidth(kind: Int): Int? = when (kind) {
        0, 2 -> 4
        1 -> 8
        3 -> 1
        else -> null
    }

    /** The row family a V6 type's rows are sent in; aero rows naming [STATUS_AERO_FIELD] are status rows instead. */
    private fun rowFamily(type: Int): RowFamily? = when (type) {
        in 1..11 -> RowFamily.TELEMETRY
        12, 14 -> RowFamily.DAMAGE
        13, in 15..20 -> RowFamily.STATUS
        21 -> RowFamily.MOTION
        22 -> RowFamily.MOTION_EX
        else -> null
    }

    /**
     * Walks one block's rows in order, reading the fields named in [wanted]
     * (by name, to an index of the caller's row). With [counted], only the
     * rows that do ([counted] true) or do not name [STATUS_AERO_FIELD] count;
     * the others are passed over.
     */
    private class Cursor(
        private val bytes: ByteBuffer,
        private val block: Block,
        wanted: Map<String, Int>,
        private val counted: Boolean? = null,
    ) {
        private val fields = block.fields.filter { it.name in wanted || (counted != null && it.name == STATUS_AERO_FIELD) }
        private val targets = IntArray(fields.size) { wanted[fields[it].name] ?: -1 }
        private val gate = if (counted == null) -1 else fields.indexOfFirst { it.name == STATUS_AERO_FIELD }
        // Where each field's next value is.
        private val next = IntArray(fields.size) { fields[it].data }
        private var row = 0

        init {
            settle()
        }

        val rows: Int get() = block.rows
        val done: Boolean get() = row >= block.rows
        val time: Double get() = bytes.getFloat(block.times + row * 4).toDouble()

        /** Whether any wanted field is in this block. */
        val reads: Boolean get() = targets.any { it >= 0 }

        /** Reads this row's wanted fields [into] their indices, and moves to the next row that counts. */
        fun read(into: FloatArray) {
            step(into)
            settle()
        }

        private fun settle() {
            if (counted == null) return
            while (row < block.rows && has(gate) != counted) step(null)
        }

        private fun has(field: Int): Boolean {
            if (field < 0) return false
            val present = fields[field].present
            return present < 0 || (bytes.get(present + (row ushr 3)).toInt() ushr (row and 7)) and 1 != 0
        }

        private fun step(into: FloatArray?) {
            for (index in fields.indices) {
                if (!has(index)) continue
                val field = fields[index]
                val at = next[index]
                val target = targets[index]
                when (field.kind) {
                    0 -> {
                        if (into != null && target >= 0) into[target] = bytes.getFloat(at)
                        next[index] = at + 4
                    }
                    1 -> {
                        if (into != null && target >= 0) into[target] = bytes.getDouble(at).toFloat()
                        next[index] = at + 8
                    }
                    2 -> {
                        if (into != null && target >= 0) into[target] = bytes.getInt(at).toFloat()
                        next[index] = at + 4
                    }
                    else -> {
                        if (into != null && target >= 0) into[target] = if (bytes.get(at).toInt() != 0) 1f else 0f
                        next[index] = at + 1
                    }
                }
            }
            row++
        }
    }

    private fun samples(bytes: ByteBuffer, blocks: List<Block>, family: GraphFamily): HistorySamples? {
        val width = family.fields.size
        val own = HashMap<String, Int>()
        val heldBy = HashMap<RowFamily, HashMap<String, Int>>()
        for (field in family.fields) {
            if (field.source == family.row) own[field.key] = field.slot
            else heldBy.getOrPut(field.source) { HashMap() }[field.key] = field.slot
        }
        val timed = ArrayList<Cursor>()
        val held = ArrayList<Cursor>()
        for (block in blocks) {
            val row = rowFamily(block.type) ?: continue
            if (block.type == AERO_TYPE) {
                if (family.row == RowFamily.TELEMETRY) timed += Cursor(bytes, block, own, counted = false)
                if (family.row == RowFamily.STATUS) timed += Cursor(bytes, block, own, counted = true)
                continue
            }
            if (row == family.row) timed += Cursor(bytes, block, own)
            heldBy[row]?.let { wanted -> Cursor(bytes, block, wanted).takeIf { it.reads }?.let(held::add) }
        }
        // Rows of one packet share a time, so the longest block is about every sample.
        var capacity = timed.maxOfOrNull { it.rows } ?: 0
        if (capacity == 0) return null
        var times = DoubleArray(capacity)
        var columns = Array(width) { FloatArray(capacity) }
        val heldSlots = heldBy.values.flatMap { it.values }
        val heldValues = FloatArray(width) { Float.NaN }
        val sample = FloatArray(width)
        var count = 0
        while (true) {
            var time = Double.POSITIVE_INFINITY
            for (cursor in timed) if (!cursor.done && cursor.time < time) time = cursor.time
            if (time == Double.POSITIVE_INFINITY) break
            for (cursor in held) while (!cursor.done && cursor.time <= time) cursor.read(heldValues)
            sample.fill(Float.NaN)
            for (slot in heldSlots) sample[slot] = heldValues[slot]
            for (cursor in timed) while (!cursor.done && cursor.time == time) cursor.read(sample)
            if (count == capacity) {
                capacity *= 2
                times = times.copyOf(capacity)
                columns = Array(width) { columns[it].copyOf(capacity) }
            }
            times[count] = time
            for (slot in 0 until width) columns[slot][count] = sample[slot]
            count++
        }
        if (count == 0) return null
        return HistorySamples(times.copyOf(count), Array(width) { columns[it].copyOf(count) })
    }

    private fun lapRows(bytes: ByteBuffer, block: Block): HistoryLapRows? {
        if (block.rows == 0) return null
        val cursor = Cursor(
            bytes, block,
            mapOf("lap_num" to 0, "current_lap_ms" to 1, "lap_distance_m" to 2, "lap_invalid" to 3, "last_lap_ms" to 4),
        )
        // A row is a patch: what it leaves out keeps its last value.
        val state = floatArrayOf(0f, -1f, Float.NaN, 0f, 0f)
        val count = block.rows
        val times = DoubleArray(count)
        val lapNumbers = IntArray(count)
        val currentLapMs = IntArray(count)
        val metres = DoubleArray(count)
        val invalid = BooleanArray(count)
        val lastLapMs = IntArray(count)
        for (index in 0 until count) {
            times[index] = cursor.time
            cursor.read(state)
            lapNumbers[index] = state[0].toInt()
            currentLapMs[index] = state[1].toInt()
            metres[index] = state[2].toDouble()
            invalid[index] = state[3] != 0f
            lastLapMs[index] = state[4].toInt()
        }
        return HistoryLapRows(times, lapNumbers, currentLapMs, metres, invalid, lastLapMs)
    }
}
