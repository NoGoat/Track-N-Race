package com.tracknrace.android

/** One completed lap of the Trends page: its time, and the ERS spent and recovered on it. */
internal data class TrendLap(
    val lap: Int,
    /** Session time the lap began and ended; the combined chart plots it at its end. */
    val startTime: Double,
    val endTime: Double,
    val lapTimeMs: Int,
    /** Null where the recording does not say. */
    val valid: Boolean?,
    val deployedMj: Double?,
    val harvestedMj: Double?,
)

/** The Trends page's laps, the lap the current stint began on, and the fastest lap. */
internal data class TrendData(
    val laps: List<TrendLap> = emptyList(),
    val stintStartLap: Int = 1,
    val fastestLap: Int = 0,
) {
    /** The laps a Stint Laps or All Laps chart shows. */
    fun lapsFor(window: GraphWindow): List<TrendLap> =
        if (window == GraphWindow.STINT) laps.filter { it.lap >= stintStartLap } else laps
}

/** A lap's total of [field] from its recorded [trace]; see [lapTotal]. */
internal fun lapTotal(trace: LapTrace, field: GraphField): Double? {
    val times = DoubleArray(trace.size) { trace.times[it].toDouble() }
    val end = times.lastOrNull() ?: return null
    return lapTotal(times, trace.values[field.slot], 0, trace.size, 0.0, end)
}

/**
 * A lap's total of a counter that the game resets at the line: its highest
 * value in the second half of the lap. Just after the line the counter still
 * holds the previous lap's total, so the first half is not read.
 */
internal fun lapTotal(times: DoubleArray, values: FloatArray, from: Int, until: Int, start: Double, end: Double): Double? {
    val half = start + (end - start) / 2
    var best = Double.NaN
    for (index in from until until) {
        if (times[index] < half) continue
        val value = values[index].toDouble()
        if (value.isFinite() && (best.isNaN() || value > best)) best = value
    }
    return best.takeIf { it.isFinite() }
}
