package com.nogoat.tracknrace

/**
 * One lap of a [family]'s fields against lap distance, for the distance chart
 * modes. [times] are seconds from the lap start, [distance] metres, and
 * [values] holds each field's column by slot, with gaps carried forward.
 */
internal class LapTrace(
    val family: GraphFamily,
    val lapNumber: Int,
    /** Session time at the lap's start, which [times] count from. */
    val startSessionTime: Double,
    val times: FloatArray,
    val distance: FloatArray,
    val values: Array<FloatArray>,
) {
    val size: Int get() = times.size

    fun value(field: GraphField, index: Int): Float = values[field.slot][index]

    /** Index of the last sample at or before [metres] that is drawn up to [maxTime], or -1. */
    fun indexAtDistance(metres: Double, maxTime: Double = Double.POSITIVE_INFINITY): Int {
        var lo = 0
        var hi = size
        while (lo < hi) {
            val mid = (lo + hi) ushr 1
            if (distance[mid] <= metres) lo = mid + 1 else hi = mid
        }
        var index = lo - 1
        while (index >= 0 && times[index] > maxTime) index--
        return index
    }
}

/**
 * Where the car was through one live lap, from the lap rows: the session time
 * of each row and its lap distance. Lets the status samples of that lap be
 * placed by distance.
 */
internal class LiveLapTrack(val lapNumber: Int, val startTime: Double) {
    var times = DoubleArray(256)
        private set
    var distance = FloatArray(256)
        private set
    var size = 0
        private set

    /** The lap's time once the next one started, 0 until then. */
    var lapMs = 0

    var invalid = false

    fun append(sessionTime: Double, metres: Float) {
        if (size > 0 && sessionTime <= times[size - 1]) return
        if (size == times.size) {
            times = times.copyOf(size * 2)
            distance = distance.copyOf(size * 2)
        }
        times[size] = sessionTime
        distance[size] = metres
        size++
    }

    /** Lap distance at [sessionTime], interpolated; clamped to the samples held. */
    fun distanceAt(sessionTime: Double): Float {
        if (size == 0) return Float.NaN
        if (sessionTime <= times[0]) return distance[0]
        if (sessionTime >= times[size - 1]) return distance[size - 1]
        var lo = 0
        var hi = size
        while (lo < hi) {
            val mid = (lo + hi) ushr 1
            if (times[mid] <= sessionTime) lo = mid + 1 else hi = mid
        }
        val after = lo
        val before = after - 1
        val span = times[after] - times[before]
        val ratio = if (span > 0) ((sessionTime - times[before]) / span).toFloat() else 0f
        return distance[before] + (distance[after] - distance[before]) * ratio
    }
}

/**
 * A recorded lap's [family] fields by distance, from a `lap_data` reply,
 * timed by the family's own rows; a field from another row family takes its
 * newest value at or before each of those times.
 */
internal fun AnalysisLap.lapTrace(family: GraphFamily): LapTrace? {
    val data = families[family.row.id] ?: return null
    // Without the lap's progress it still serves the time charts, just not by distance.
    val progress = progress
    val count = data.times.size
    val distance = FloatArray(count) { progress?.distanceAt(data.times[it].toDouble())?.toFloat() ?: Float.NaN }
    val values = Array(family.fields.size) { slot ->
        val field = family.fields[slot]
        val source = if (field.source == family.row) {
            data.fields[field.key]?.takeIf { it.size == count }
        } else {
            families[field.source.id]?.let { other -> heldAt(other, field.key, data.times) }
        }
        var carried = Float.NaN
        FloatArray(count) { index ->
            val value = source?.get(index) ?: Float.NaN
            if (!value.isNaN()) carried = value
            carried
        }
    }
    return LapTrace(family, lapNumber, startSessionTime, data.times.copyOf(), distance, values)
}

/**
 * Recorded laps joined end to end in session time, for the lap-span charts
 * in playback. Shares the laps' columns rather than copying them.
 */
/** [key] of [other] at each of [times]: its newest sample at or before it, NaN before the first. */
private fun heldAt(other: AnalysisFamilyData, key: String, times: FloatArray): FloatArray? {
    val column = other.fields[key]?.takeIf { it.size == other.times.size } ?: return null
    var index = -1
    return FloatArray(times.size) { sample ->
        while (index + 1 < other.times.size && other.times[index + 1] <= times[sample]) index++
        if (index >= 0) column[index] else Float.NaN
    }
}

internal class RecordedSeries(private val laps: List<LapTrace>) : SampleSeries {
    // Index of each lap's first sample in the joined series.
    private val offsets = IntArray(laps.size + 1).also { offsets ->
        laps.forEachIndexed { index, lap -> offsets[index + 1] = offsets[index] + lap.size }
    }

    override val size: Int = offsets.last()

    private fun lapOf(index: Int): Int {
        var lo = 0
        var hi = laps.size - 1
        while (lo < hi) {
            val mid = (lo + hi + 1) ushr 1
            if (offsets[mid] <= index) lo = mid else hi = mid - 1
        }
        return lo
    }

    override fun time(index: Int): Double {
        val lap = lapOf(index)
        val trace = laps[lap]
        return trace.startSessionTime + trace.times[index - offsets[lap]]
    }

    override fun value(field: GraphField, index: Int): Float {
        val lap = lapOf(index)
        return laps[lap].value(field, index - offsets[lap])
    }

    /** Where the current stint began within these laps: by the fitted tyre, or the wear starting over; NaN for others. */
    val stintStart: Double by lazy {
        when (laps.firstOrNull()?.family) {
            GraphFamily.STATUS -> stintStartTime(this)
            GraphFamily.TYRE_WEAR -> wearStintStartTime(this)
            else -> Double.NaN
        }
    }

    // The laps joined into single columns on first use, for the renderers'
    // tight loops; the four charts may ask from four threads at once.
    private val times: DoubleArray by lazy {
        DoubleArray(size).also { joined ->
            laps.forEachIndexed { lap, trace ->
                for (index in 0 until trace.size) joined[offsets[lap] + index] = trace.startSessionTime + trace.times[index]
            }
        }
    }
    private val joinedColumns = java.util.concurrent.ConcurrentHashMap<GraphField, FloatArray>()

    override fun timeColumn(): DoubleArray = times

    override fun column(field: GraphField): FloatArray = joinedColumns.getOrPut(field) {
        FloatArray(size).also { joined ->
            laps.forEachIndexed { lap, trace -> System.arraycopy(trace.values[field.slot], 0, joined, offsets[lap], trace.size) }
        }
    }
}
