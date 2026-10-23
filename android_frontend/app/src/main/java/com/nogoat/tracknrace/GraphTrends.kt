package com.nogoat.tracknrace

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

/** One completed lap's ERS and fuel use as the engine measured it; null where it was not. */
internal data class LapMeasure(
    val deployedMj: Double?,
    val harvestedMj: Double?,
    val fuelUsedKg: Double?,
)
