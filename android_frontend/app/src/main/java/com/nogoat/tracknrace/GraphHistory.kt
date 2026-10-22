package com.nogoat.tracknrace

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** A row family the desktop sends, by its name in rows and in `request_lap_data`. */
internal enum class RowFamily(val id: String) {
    STATUS("status"),
    TELEMETRY("telemetry"),
    DAMAGE("damage"),
    MOTION("motion"),
    MOTION_EX("motion_ex"),
}

/**
 * A set of fields one graph tab keeps a history of. Each is kept on its own,
 * timed by its [row] family, so a tab never holds or downloads fields it
 * does not draw.
 */
internal enum class GraphFamily(val row: RowFamily) {
    /** Power: status. */
    STATUS(RowFamily.STATUS),

    /** Inputs: throttle, brake, steering, gear. */
    TELEMETRY(RowFamily.TELEMETRY),

    /** Speed: speed and RPM, with the ERS store held between status rows. */
    DRIVE(RowFamily.TELEMETRY),

    /** Tyres: surface, inner and brake temperatures. */
    TYRE_TEMPS(RowFamily.TELEMETRY),

    /** Tyres: wear. */
    TYRE_WEAR(RowFamily.DAMAGE),

    /** Misc: lateral and longitudinal G. */
    G_FORCE(RowFamily.MOTION),

    /** Misc: front and rear ride height. */
    RIDE_HEIGHT(RowFamily.MOTION_EX),

    /** Trends: the per-lap ERS deployed and harvested counters. */
    TREND_ENERGY(RowFamily.STATUS),
    ;

    /** This family's fields, in column order. */
    val fields: List<GraphField> by lazy { GraphField.entries.filter { it.family == this } }
}

/**
 * The fields the graphs keep, as columns of a [SampleHistory] of their
 * [family]: what the graph pages plot, and the fitted tyre that tells where
 * the current stint began. A field read from another row family than its
 * family's ([source]) holds its newest value at each sample.
 */
internal enum class GraphField(val family: GraphFamily, val key: String, val source: RowFamily = family.row) {
    ICE_KW(GraphFamily.STATUS, "engine_power_ice_kw"),
    MGUK_KW(GraphFamily.STATUS, "engine_power_mguk_kw"),
    HARVEST_MGUK_J(GraphFamily.STATUS, "ers_harvested_mguk_j"),
    HARVEST_MGUH_J(GraphFamily.STATUS, "ers_harvested_mguh_j"),
    ERS_PCT(GraphFamily.STATUS, "ers_pct"),
    FUEL_KG(GraphFamily.STATUS, "fuel_kg"),
    TYRE_COMPOUND(GraphFamily.STATUS, "tyre_compound"),
    VISUAL_COMPOUND(GraphFamily.STATUS, "visual_compound"),
    TYRE_AGE_LAPS(GraphFamily.STATUS, "tyre_age_laps"),
    THROTTLE(GraphFamily.TELEMETRY, "throttle"),
    BRAKE(GraphFamily.TELEMETRY, "brake"),
    STEERING(GraphFamily.TELEMETRY, "steering"),
    GEAR(GraphFamily.TELEMETRY, "gear"),
    SPEED(GraphFamily.DRIVE, "speed_kph"),
    RPM(GraphFamily.DRIVE, "rpm"),
    DRIVE_ERS_PCT(GraphFamily.DRIVE, "ers_pct", RowFamily.STATUS),
    SURFACE_FL(GraphFamily.TYRE_TEMPS, "tyre_temp_surface_fl"),
    SURFACE_FR(GraphFamily.TYRE_TEMPS, "tyre_temp_surface_fr"),
    SURFACE_RL(GraphFamily.TYRE_TEMPS, "tyre_temp_surface_rl"),
    SURFACE_RR(GraphFamily.TYRE_TEMPS, "tyre_temp_surface_rr"),
    INNER_FL(GraphFamily.TYRE_TEMPS, "tyre_temp_inner_fl"),
    INNER_FR(GraphFamily.TYRE_TEMPS, "tyre_temp_inner_fr"),
    INNER_RL(GraphFamily.TYRE_TEMPS, "tyre_temp_inner_rl"),
    INNER_RR(GraphFamily.TYRE_TEMPS, "tyre_temp_inner_rr"),
    BRAKE_TEMP_FL(GraphFamily.TYRE_TEMPS, "brake_temp_fl"),
    BRAKE_TEMP_FR(GraphFamily.TYRE_TEMPS, "brake_temp_fr"),
    BRAKE_TEMP_RL(GraphFamily.TYRE_TEMPS, "brake_temp_rl"),
    BRAKE_TEMP_RR(GraphFamily.TYRE_TEMPS, "brake_temp_rr"),
    WEAR_FL(GraphFamily.TYRE_WEAR, "tyre_wear_fl"),
    WEAR_FR(GraphFamily.TYRE_WEAR, "tyre_wear_fr"),
    WEAR_RL(GraphFamily.TYRE_WEAR, "tyre_wear_rl"),
    WEAR_RR(GraphFamily.TYRE_WEAR, "tyre_wear_rr"),
    G_LAT(GraphFamily.G_FORCE, "g_lat"),
    G_LONG(GraphFamily.G_FORCE, "g_long"),
    FRONT_HEIGHT(GraphFamily.RIDE_HEIGHT, "front_aero_height_mm"),
    REAR_HEIGHT(GraphFamily.RIDE_HEIGHT, "rear_aero_height_mm"),
    ERS_DEPLOYED_J(GraphFamily.TREND_ENERGY, "ers_deployed_j"),
    TREND_HARVEST_MGUK_J(GraphFamily.TREND_ENERGY, "ers_harvested_mguk_j"),
    TREND_HARVEST_MGUH_J(GraphFamily.TREND_ENERGY, "ers_harvested_mguh_j"),
    ;

    /** This field's column within its family. */
    val slot: Int by lazy { family.fields.indexOf(this) }

    /** The `request_lap_data` channel for this field. */
    val channel: String get() = "${source.id}.$key"
}

/** One family's samples in session-time order, as the time and lap-span charts read them. */
internal interface SampleSeries {
    val size: Int

    fun time(index: Int): Double

    fun value(field: GraphField, index: Int): Float

    /** Every sample's time, indexed as [time]; for a renderer's tight loops. */
    fun timeColumn(): DoubleArray

    /** Every sample of [field], indexed as [value]. */
    fun column(field: GraphField): FloatArray

    /** Index of the last sample at or before [time], or -1. */
    fun indexAtOrBefore(time: Double): Int {
        var lo = 0
        var hi = size
        while (lo < hi) {
            val mid = (lo + hi) ushr 1
            if (time(mid) <= time) lo = mid + 1 else hi = mid
        }
        return lo - 1
    }
}

/** A copied run of samples, for drawing off the main thread; columns by field slot. */
internal class SliceSeries(private val times: DoubleArray, private val columns: Array<FloatArray>) : SampleSeries {
    override val size: Int get() = times.size

    override fun time(index: Int): Double = times[index]

    override fun value(field: GraphField, index: Int): Float = columns[field.slot][index]

    override fun timeColumn(): DoubleArray = times

    override fun column(field: GraphField): FloatArray = columns[field.slot]
}

/**
 * Where the current tyre stint began in status [series]: the last sample a new
 * set was fitted, by the desktop's rule, else the first sample.
 */
internal fun stintStartTime(series: SampleSeries): Double {
    if (series.size == 0) return Double.NaN
    var start = series.time(0)
    for (index in 1 until series.size) {
        if (isNewTyreStint(series, index - 1, index)) start = series.time(index)
    }
    return start
}

internal fun isNewTyreStint(series: SampleSeries, previous: Int, current: Int): Boolean = isNewTyreStint(
    series.value(GraphField.TYRE_COMPOUND, previous), series.value(GraphField.VISUAL_COMPOUND, previous),
    series.value(GraphField.TYRE_AGE_LAPS, previous), series.time(previous),
    series.value(GraphField.TYRE_COMPOUND, current), series.value(GraphField.VISUAL_COMPOUND, current),
    series.value(GraphField.TYRE_AGE_LAPS, current), series.time(current),
)

// Tyre wear that falls by more than this, on average over the four, is a new set.
private const val NEW_SET_WEAR_DROP = 1f

/** Average wear of the four corners at [index], NaN when any is unknown. */
private fun meanWear(series: SampleSeries, index: Int): Float =
    (series.value(GraphField.WEAR_FL, index) + series.value(GraphField.WEAR_FR, index) +
        series.value(GraphField.WEAR_RL, index) + series.value(GraphField.WEAR_RR, index)) / 4f

/** Whether wear sample [current] is a new set after [previous]: wear only falls when the tyres are changed. */
internal fun isNewTyreSet(series: SampleSeries, previous: Int, current: Int): Boolean =
    meanWear(series, current) < meanWear(series, previous) - NEW_SET_WEAR_DROP

/** Where the current set went on in tyre wear [series], else its first sample. */
internal fun wearStintStartTime(series: SampleSeries): Double {
    if (series.size == 0) return Double.NaN
    var start = series.time(0)
    for (index in 1 until series.size) if (isNewTyreSet(series, index - 1, index)) start = series.time(index)
    return start
}

/**
 * The desktop's rule for a new set between two tyre samples: another
 * compound, the age going down, or a used set fitted within half a minute.
 */
internal fun isNewTyreStint(
    compoundBefore: Float, visualBefore: Float, ageBefore: Float, timeBefore: Double,
    compoundNow: Float, visualNow: Float, ageNow: Float, timeNow: Double,
): Boolean {
    val compoundChanged = compoundBefore > 0f && compoundNow > 0f &&
        (compoundNow != compoundBefore || visualNow != visualBefore)
    val ageDelta = ageNow - ageBefore
    val usedSetFitted = ageDelta > 1f && timeNow - timeBefore < 30.0
    return compoundChanged || ageDelta < 0f || usedSetFitted
}

/**
 * Where the selected driver's current tyre stint began, from the status rows'
 * fitted tyre whichever page subscribed them; for charts of other families.
 * Main thread only.
 */
internal class StintTracker {
    private var compound = Float.NaN
    private var visual = Float.NaN
    private var age = Float.NaN
    private var time = Double.NaN

    /** NaN until a fitting has been seen. */
    var start = Double.NaN
        private set

    fun accept(sessionTime: Double, newCompound: Int?, newVisual: Int?, newAge: Int?) {
        if (!sessionTime.isFinite()) return
        val compoundNow = newCompound?.toFloat() ?: compound
        val visualNow = newVisual?.toFloat() ?: visual
        val ageNow = newAge?.toFloat() ?: age
        if (time.isFinite() && sessionTime < time) reset()
        if (time.isFinite() && isNewTyreStint(compound, visual, age, time, compoundNow, visualNow, ageNow, sessionTime)) {
            start = sessionTime
        }
        compound = compoundNow
        visual = visualNow
        age = ageNow
        time = sessionTime
    }

    fun reset() {
        compound = Float.NaN
        visual = Float.NaN
        age = Float.NaN
        time = Double.NaN
        start = Double.NaN
    }
}

// A stream row this far behind the newest sample is a seek back; a smaller
// step back is a row that crossed a backfill and is dropped.
private const val SEEK_BACK_SECONDS = 1.0

// Live laps whose position is kept for the distance modes.
private const val MAX_LAP_TRACKS = 120

// A forward jump past this is a seek or a stall: start over so the window is
// backfilled rather than drawn as a straight line across the gap.
private const val SEEK_FORWARD_SECONDS = 5.0

/**
 * The selected driver's samples of one [family], kept on the phone while a
 * graph page of that family is subscribed: the desktop streams latest state
 * only, so the history is built from what arrives, and backfilled from
 * recorded laps in playback. Rows are patches, so every sample carries the
 * last known value of each field. Every sample is kept: All Laps draws the
 * whole session. Main thread only, except [enabled].
 */
internal class SampleHistory(val family: GraphFamily) : SampleSeries {
    private val width = family.fields.size
    private var times = DoubleArray(1024)
    private var columns = Array(width) { FloatArray(1024) }

    /** Number of samples kept. */
    override var size = 0
        private set

    // The newest value of each field, merged from patches.
    private val latest = FloatArray(width) { Float.NaN }

    /** Collecting at all: on only while a graph page of this family is subscribed. Read off the main thread. */
    @Volatile var enabled = false
        private set

    /** Bumped on every change, so a chart reading it in draw redraws. */
    var version by mutableIntStateOf(0)
        private set

    /** Bumped only when the samples change, not for lap rows; read after [version]. */
    var sampleVersion = 0
        private set

    /** Bumped when the samples are dropped, so a backfill knows to run again. */
    var epoch = 0
        private set

    /** The highest fuel load seen for this driver, across seeks; NaN until one arrives. */
    var fuelPeak = Float.NaN
        private set

    /** The selected driver's private telemetry is withheld. */
    var unavailable by mutableStateOf(false)
        private set

    // Live lap rows, by lap number, while a distance mode is subscribed.
    private val lapTracks = LinkedHashMap<Int, LiveLapTrack>()

    /** The newest lap number the lap rows named; 0 before any. */
    var currentLap = 0
        private set

    /** When the newest sample arrived (System.nanoTime, the frame clock's base), for following a live clock between rows. */
    var lastAppendNanos = 0L
        private set

    /** Where the current tyre stint began within the samples held: by the fitted tyre, or the wear starting over; NaN otherwise. */
    var stintStart = Double.NaN
        private set

    private val tracksStint = family == GraphFamily.STATUS
    private val tracksWear = family == GraphFamily.TYRE_WEAR

    override fun time(index: Int): Double = times[index]

    override fun value(field: GraphField, index: Int): Float = columns[field.slot][index]

    // Main thread only, like the rest of the history; a renderer takes [stableSlice].
    override fun timeColumn(): DoubleArray = times.copyOf(size)

    override fun column(field: GraphField): FloatArray = columns[field.slot].copyOf(size)

    /**
     * Samples [first]..[last] of [fields] copied for another thread: samples
     * keep arriving on this one. Other fields are left empty.
     */
    fun stableSlice(first: Int, last: Int, fields: Collection<GraphField>): SampleSeries {
        val from = first.coerceIn(0, size)
        val count = (last - from + 1).coerceIn(0, size - from)
        return SliceSeries(
            times.copyOfRange(from, from + count),
            Array(width) { slot ->
                if (family.fields[slot] in fields) columns[slot].copyOfRange(from, from + count) else FloatArray(0)
            },
        )
    }

    val firstTime: Double get() = if (size > 0) times[0] else Double.NaN
    val lastTime: Double get() = if (size > 0) times[size - 1] else Double.NaN

    override fun indexAtOrBefore(time: Double): Int {
        var lo = 0
        var hi = size
        while (lo < hi) {
            val mid = (lo + hi) ushr 1
            if (times[mid] <= time) lo = mid + 1 else hi = mid
        }
        return lo - 1
    }

    fun setEnabled(value: Boolean) {
        if (enabled == value) return
        enabled = value
        clear()
        latest.fill(Float.NaN)
    }

    /** Drops every sample: a seek, a new driver or session. */
    fun clear() {
        if (lapTracks.isNotEmpty()) {
            lapTracks.clear()
            currentLap = 0
            version++
        }
        stintStart = Double.NaN
        if (size == 0) return
        size = 0
        epoch++
        version++
        sampleVersion++
    }

    /** A new driver or session: also forget the merged values and fuel peak. */
    fun reset() {
        clear()
        latest.fill(Float.NaN)
        fuelPeak = Float.NaN
        unavailable = false
    }

    fun markUnavailable() {
        unavailable = true
        latest.fill(Float.NaN)
        clear()
    }

    /**
     * A newer value of a field this family holds from another row family:
     * kept for the samples that follow, with no sample of its own.
     */
    fun hold(field: GraphField, value: Float) {
        if (!enabled || value.isNaN()) return
        latest[field.slot] = value
    }

    /** One row: [patch] holds each field's new value by slot, NaN where it was absent. */
    fun accept(sessionTime: Double, patch: FloatArray) {
        if (!enabled) return
        unavailable = false
        for (index in patch.indices) if (!patch[index].isNaN()) latest[index] = patch[index]
        if (family == GraphFamily.STATUS) noteFuel(latest[GraphField.FUEL_KG.slot])
        if (!sessionTime.isFinite()) return
        if (size > 0) {
            val last = lastTime
            when {
                sessionTime < last - SEEK_BACK_SECONDS || sessionTime > last + SEEK_FORWARD_SECONDS -> clear()
                sessionTime < last -> return
                sessionTime == last -> {
                    // Another field group of the same instant.
                    val at = size - 1
                    for (index in columns.indices) columns[index][at] = latest[index]
                    if (tracksStint && size > 1 && isNewTyreStint(this, size - 2, size - 1)) stintStart = sessionTime
                    changed()
                    return
                }
            }
        }
        append(sessionTime)
        changed()
    }

    /**
     * Whole samples decoded from a binary batch, oldest first: [values] by
     * slot, each [count] long. Same rules as [accept], one change for the batch.
     */
    fun acceptBatch(sessionTimes: DoubleArray, values: Array<FloatArray>, count: Int) {
        if (!enabled || count == 0) return
        unavailable = false
        var added = false
        for (sample in 0 until count) {
            val sessionTime = sessionTimes[sample]
            if (!sessionTime.isFinite()) continue
            for (slot in 0 until width) {
                val value = values[slot][sample]
                if (!value.isNaN()) latest[slot] = value
            }
            if (size > 0) {
                val last = lastTime
                if (sessionTime < last - SEEK_BACK_SECONDS || sessionTime > last + SEEK_FORWARD_SECONDS) {
                    clear()
                } else if (sessionTime <= last) {
                    continue
                }
            }
            append(sessionTime)
            added = true
        }
        if (added) changed()
    }

    private fun append(sessionTime: Double) {
        ensureRoomAtEnd()
        times[size] = sessionTime
        for (index in columns.indices) columns[index][size] = latest[index]
        size++
        if (tracksStint) {
            if (size == 1) stintStart = sessionTime
            else if (isNewTyreStint(this, size - 2, size - 1)) stintStart = sessionTime
        }
        if (tracksWear) {
            if (size == 1) stintStart = sessionTime
            else if (isNewTyreSet(this, size - 2, size - 1)) stintStart = sessionTime
        }
    }

    private fun changed() {
        lastAppendNanos = System.nanoTime()
        version++
        sampleVersion++
    }

    /**
     * Inserts recorded samples older than the first one kept, or up to
     * [upTo] when nothing is kept yet. [times] ascend; [values] are by slot,
     * and a column's NaN carries the previous value forward, as patches do.
     */
    fun backfill(times: DoubleArray, values: Array<FloatArray>, upTo: Double) {
        if (!enabled) return
        val limit = if (size > 0) firstTime else upTo
        var count = 0
        while (count < times.size && times[count] < limit) count++
        if (count == 0) return
        val filled = Array(width) { column ->
            val source = values[column]
            var carried = Float.NaN
            FloatArray(count) { index ->
                val value = source[index]
                if (!value.isNaN()) carried = value
                carried
            }
        }
        if (family == GraphFamily.STATUS) for (value in filled[GraphField.FUEL_KG.slot]) noteFuel(value)
        // Prepend: rebuild once, as a backfill happens a few times per seek.
        val total = size + count
        val newTimes = DoubleArray(maxOf(total, 1024))
        val newColumns = Array(width) { FloatArray(newTimes.size) }
        System.arraycopy(times, 0, newTimes, 0, count)
        System.arraycopy(this.times, 0, newTimes, count, size)
        for (column in 0 until width) {
            System.arraycopy(filled[column], 0, newColumns[column], 0, count)
            System.arraycopy(columns[column], 0, newColumns[column], count, size)
        }
        this.times = newTimes
        columns = newColumns
        size = total
        if (tracksStint) stintStart = stintStartTime(this)
        if (tracksWear) stintStart = wearStintStartTime(this)
        if (lastAppendNanos == 0L) lastAppendNanos = System.nanoTime()
        version++
        sampleVersion++
    }

    /**
     * One merged lap row: where the car is in lap [lapNumber]. Starts that
     * lap's track, and closes the previous one with [lastLapMs].
     */
    fun acceptLap(sessionTime: Double, lapNumber: Int, currentLapMs: Int, metres: Double, invalid: Boolean, lastLapMs: Int) {
        if (!enabled || lapNumber <= 0 || !sessionTime.isFinite() || !metres.isFinite()) return
        // A lower lap number is a new session; history already restarts on its time.
        if (lapNumber < currentLap) {
            lapTracks.clear()
            currentLap = 0
        }
        var track = lapTracks[lapNumber]
        if (track == null) {
            lapTracks[lapNumber - 1]?.let { if (lastLapMs > 0) it.lapMs = lastLapMs }
            val start = if (currentLapMs >= 0) sessionTime - currentLapMs / 1000.0 else sessionTime
            track = LiveLapTrack(lapNumber, start)
            lapTracks[lapNumber] = track
            while (lapTracks.size > MAX_LAP_TRACKS) lapTracks.remove(lapTracks.keys.first())
        }
        if (invalid) track.invalid = true
        currentLap = maxOf(currentLap, lapNumber)
        track.append(sessionTime, metres.toFloat())
        version++
    }

    /** The live laps whose position is held, in order. */
    fun liveLaps(): List<LiveLapTrack> = lapTracks.values.toList()

    /** The live laps' numbers and start times, in order, for lap-span axes. */
    fun lapStarts(): List<Pair<Int, Double>> = lapTracks.values.map { it.lapNumber to it.startTime }

    /** The fastest completed valid live lap whose samples are held, or 0. */
    fun fastestLiveLap(): Int =
        lapTracks.values.filter { it.lapMs > 0 && !it.invalid }.minByOrNull { it.lapMs }?.lapNumber ?: 0

    /** Live lap [lapNumber]'s samples placed by distance, or null without its position. */
    fun liveLapTrace(lapNumber: Int): LapTrace? {
        val track = lapTracks[lapNumber] ?: return null
        if (track.size == 0 || size == 0) return null
        val end = lapTracks[lapNumber + 1]?.startTime ?: Double.POSITIVE_INFINITY
        val from = indexAtOrBefore(track.startTime) + 1
        val until = indexAtOrBefore(end - 1e-6)
        if (until < from) return null
        val count = until - from + 1
        val traceTimes = FloatArray(count) { (time(from + it) - track.startTime).toFloat() }
        val distance = FloatArray(count) { track.distanceAt(time(from + it)) }
        val values = Array(width) { column -> columns[column].copyOfRange(from, from + count) }
        return LapTrace(family, lapNumber, track.startTime, traceTimes, distance, values)
    }

    /** Raises the fuel axis to [fuel] if it is the highest load seen. */
    fun noteFuel(fuel: Float) {
        if (fuel.isFinite() && fuel >= 0f && (fuelPeak.isNaN() || fuel > fuelPeak)) fuelPeak = fuel
    }

    private fun ensureRoomAtEnd() {
        if (size < times.size) return
        val capacity = times.size * 2
        times = times.copyOf(capacity)
        columns = Array(width) { columns[it].copyOf(capacity) }
    }
}
