package com.nogoat.tracknrace

import androidx.compose.runtime.Immutable
import org.json.JSONObject

/** One completed lap: its time and sectors in ms (0 = none), and which of them count. */
@Immutable
internal data class DriverLap(
    val lapNumber: Int,
    val timeMs: Int,
    val s1Ms: Int,
    val s2Ms: Int,
    val s3Ms: Int,
    val valid: Boolean,
    val s1Valid: Boolean,
    val s2Valid: Boolean,
    val s3Valid: Boolean,
)

/**
 * One car's completed laps, as the desktop's Driver laps dialog shows them:
 * live from its session history, in playback up to the cursor. The engine's
 * driver_lap_history row.
 */
@Immutable
internal data class DriverLapHistory(
    val carIndex: Int,
    val bestLapNumber: Int?,
    val laps: List<DriverLap>,
    /** First lap of the tyre stint in use; 0 when unknown. */
    val stintStartLap: Int,
    /** The session's fastest valid sectors across every car, 0 when none yet. */
    val overallBestS1Ms: Int,
    val overallBestS2Ms: Int,
    val overallBestS3Ms: Int,
)

internal fun parseDriverLapHistory(row: JSONObject): DriverLapHistory? {
    val car = row.optionalInt("car_idx") ?: return null
    val laps = row.optJSONArray("laps")?.let { array ->
        List(array.length()) { index -> array.optJSONObject(index) }
            .filterNotNull()
            .map { lap ->
                DriverLap(
                    lapNumber = lap.optInt("lap_num"),
                    timeMs = lap.optInt("lap_time_ms"),
                    s1Ms = lap.optInt("s1_ms"),
                    s2Ms = lap.optInt("s2_ms"),
                    s3Ms = lap.optInt("s3_ms"),
                    valid = lap.optBoolean("lap_valid", true),
                    s1Valid = lap.optBoolean("s1_valid", true),
                    s2Valid = lap.optBoolean("s2_valid", true),
                    s3Valid = lap.optBoolean("s3_valid", true),
                )
            }
            .filter { it.lapNumber > 0 && it.timeMs > 0 }
            .sortedBy { it.lapNumber }
    } ?: emptyList()
    return DriverLapHistory(
        carIndex = car,
        bestLapNumber = row.optionalInt("best_lap_num")?.takeIf { it > 0 },
        laps = laps,
        stintStartLap = row.optInt("stint_start_lap"),
        overallBestS1Ms = row.optInt("overall_best_s1_ms"),
        overallBestS2Ms = row.optInt("overall_best_s2_ms"),
        overallBestS3Ms = row.optInt("overall_best_s3_ms"),
    )
}

/** 75204 → "1:15.204". */
internal fun formatLapMs(ms: Int): String {
    if (ms <= 0) return "—"
    val minutes = ms / 60_000
    val rest = ms - minutes * 60_000
    return "%d:%02d.%03d".format(java.util.Locale.ROOT, minutes, rest / 1000, rest % 1000)
}

/** 18469 → "18.469"; a sector over a minute keeps its minutes. */
internal fun formatSectorMs(ms: Int): String {
    if (ms <= 0) return "—"
    return if (ms >= 60_000) formatLapMs(ms) else "%d.%03d".format(java.util.Locale.ROOT, ms / 1000, ms % 1000)
}
