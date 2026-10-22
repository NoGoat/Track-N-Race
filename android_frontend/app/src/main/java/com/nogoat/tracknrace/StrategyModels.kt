package com.nogoat.tracknrace

import org.json.JSONArray
import org.json.JSONObject

/**
 * libtnrp's derived `strategy` row for the selected car, as the desktop
 * Strategy page reads it. Only the fields the dashboard's Strategy tab shows
 * are kept. Every row is a complete snapshot, never a patch.
 */
internal data class StrategySnapshot(
    /** "non_race", "waiting" or "ready". */
    val state: String,
    val lapNumber: Int,
    val totalLaps: Int,
    val actualCompound: Int,
    val visualCompound: Int,
    val compoundName: String,
    val tyreAgeLaps: Int,
    val averageWear: Float,
    val wearPerLap: Float,
    val limitingCorner: String,
    val cliffLap: Int,
    val lapsUntilCliff: Int,
    val defensive: StrategyPlan,
    val attacking: StrategyPlan,
    val call: StrategyCall?,
    val neutralisation: StrategyNeutralisation?,
    val weather: StrategyWeather?,
    val rivals: List<StrategyRival>,
    /** The selected car's race position, 0 while unknown. */
    val playerPosition: Int,
    /** FL, FR, RL, RR wear in percent. */
    val cornerWear: List<Float>,
    val warnings: List<StrategyWarning>,
) {
    val ready get() = state == "ready"
}

internal data class StrategyPlan(
    val stops: Int,
    val targetIndex: Int,
    val targetName: String,
    val confidence: Float,
    val legal: Boolean,
    val requiresCompoundChange: Boolean,
    val stints: List<StrategyStint>,
) {
    /** The lap this plan next pits on, or null when it runs to the flag. */
    fun nextStopLap(currentLap: Int): Int? =
        stints.firstOrNull { !it.isLast && it.endLap >= currentLap }?.endLap
}

internal data class StrategyStint(
    val compoundName: String,
    val actualCompound: Int,
    val visualCompound: Int,
    val number: Int,
    val startLap: Int,
    val endLap: Int,
    val expectedLaps: Int,
    val actualLaps: Int,
    val isLast: Boolean,
    val rows: List<StrategyLapTarget>,
)

internal data class StrategyLapTarget(
    val lapNumber: Int,
    val requiredMs: Int,
    val actualMs: Int,
    val deltaLapMs: Int,
    val deltaStintMs: Int,
    val deltaTotalMs: Int,
    val hasActual: Boolean,
)

internal data class StrategyCall(
    /** "cover", "undercut" or "overcut". */
    val kind: String,
    val targetName: String,
    val gapMs: Int,
    val crossoverLaps: Int?,
    val reason: String,
)

internal data class StrategyNeutralisation(
    val safetyCar: Boolean,
    val box: Boolean,
    val reason: String,
    val boxNowCostMs: Int,
    val boxLaterCostMs: Int,
    val boxLaterLap: Int,
    val currentPosition: Int,
    val projectedBoxPosition: Int,
    val projectedStayPosition: Int,
)

internal data class StrategyWeather(
    val recommendation: String,
    val targetCompound: String,
    val rainPercentage: Int,
    val crossoverLap: Int,
    val minutesUntilChange: Int,
    val reason: String,
    val lapDeltaMs: Int?,
    val setWear: Int?,
)

internal data class StrategyRival(
    val index: Int,
    val name: String,
    val ahead: Boolean,
    val position: Int,
    val gapMs: Int,
    val paceDeltaMs: Int,
    val tyreAgeLaps: Int,
    val actualCompound: Int,
    val visualCompound: Int,
    val lastPitLap: Int,
)

internal data class StrategyWarning(
    val text: String,
    /** "danger", "warning" or "caution". */
    val severity: String,
)

internal fun parseStrategySnapshot(row: JSONObject): StrategySnapshot = StrategySnapshot(
    state = row.optString("state", "waiting"),
    lapNumber = row.optInt("lap_num"),
    totalLaps = row.optInt("total_laps"),
    actualCompound = row.optInt("current_actual_compound"),
    visualCompound = row.optInt("current_visual_compound"),
    compoundName = row.optString("current_compound_name"),
    tyreAgeLaps = row.optInt("current_tyre_age_laps"),
    averageWear = row.optDouble("average_wear", 0.0).toFloat(),
    wearPerLap = row.optDouble("wear_per_lap", 0.0).toFloat(),
    limitingCorner = row.optString("limiting_corner"),
    cliffLap = row.optInt("cliff_lap"),
    lapsUntilCliff = row.optInt("laps_until_cliff"),
    defensive = parsePlan(row.optJSONObject("conservative")),
    attacking = parsePlan(row.optJSONObject("aggressive")),
    call = row.optJSONObject("call")?.let { call ->
        StrategyCall(
            kind = call.optString("kind"),
            targetName = call.optString("target_name"),
            gapMs = call.optInt("gap_ms"),
            crossoverLaps = call.optionalInt("crossover_laps"),
            reason = call.optString("reason"),
        )
    },
    neutralisation = row.optJSONObject("neutralisation")?.let { n ->
        StrategyNeutralisation(
            safetyCar = n.optString("kind") == "safety_car",
            box = n.optString("recommendation") == "box",
            reason = n.optString("reason"),
            boxNowCostMs = n.optInt("box_now_cost_ms"),
            boxLaterCostMs = n.optInt("box_later_cost_ms"),
            boxLaterLap = n.optInt("box_later_lap"),
            currentPosition = n.optInt("current_position"),
            projectedBoxPosition = n.optInt("projected_box_position"),
            projectedStayPosition = n.optInt("projected_stay_position"),
        )
    },
    weather = row.optJSONObject("weather_strategy")?.let { w ->
        StrategyWeather(
            recommendation = w.optString("recommendation"),
            targetCompound = w.optString("target_compound"),
            rainPercentage = w.optInt("rain_percentage"),
            crossoverLap = w.optInt("crossover_lap"),
            minutesUntilChange = w.optInt("minutes_until_change"),
            reason = w.optString("reason"),
            lapDeltaMs = w.optionalInt("lap_delta_ms"),
            setWear = w.optionalInt("set_wear"),
        )
    },
    rivals = row.optJSONArray("rivals").objects().map { r ->
        StrategyRival(
            index = r.optInt("idx"),
            name = r.optString("name"),
            ahead = r.optString("direction") == "ahead",
            position = r.optInt("position"),
            gapMs = r.optInt("gap_ms"),
            paceDeltaMs = r.optInt("pace_delta_ms"),
            tyreAgeLaps = r.optInt("tyre_age_laps"),
            actualCompound = r.optInt("actual_compound"),
            visualCompound = r.optInt("visual_compound"),
            lastPitLap = r.optInt("last_pit_lap"),
        )
    },
    playerPosition = row.optJSONArray("positions").objects()
        .firstOrNull { it.optString("role") == "player" }?.optInt("position") ?: 0,
    cornerWear = listOf("wear_fl", "wear_fr", "wear_rl", "wear_rr").map { row.optDouble(it, 0.0).toFloat() },
    warnings = row.optJSONArray("wear_warnings").objects().map { w ->
        StrategyWarning(w.optString("text"), w.optString("severity", "caution"))
    },
)

private fun parsePlan(plan: JSONObject?): StrategyPlan = StrategyPlan(
    stops = plan?.optInt("stops") ?: 0,
    targetIndex = plan?.optInt("target_idx", -1) ?: -1,
    targetName = plan?.optString("target_name").orEmpty(),
    confidence = plan?.optDouble("confidence", 0.0)?.toFloat() ?: 0f,
    legal = plan?.optBoolean("legal", true) ?: true,
    requiresCompoundChange = plan?.optBoolean("requires_compound_change") ?: false,
    stints = plan?.optJSONArray("stints").objects().map { s ->
        StrategyStint(
            compoundName = s.optString("compound_name"),
            actualCompound = s.optInt("actual_compound"),
            visualCompound = s.optInt("visual_compound"),
            number = s.optInt("stint_number"),
            startLap = s.optInt("start_lap"),
            endLap = s.optInt("end_lap"),
            expectedLaps = s.optInt("expected_laps"),
            actualLaps = s.optInt("actual_laps"),
            isLast = s.optBoolean("is_last"),
            rows = s.optJSONArray("rows").objects().map { r ->
                StrategyLapTarget(
                    lapNumber = r.optInt("lap_num"),
                    requiredMs = r.optDouble("required_ms", 0.0).toInt(),
                    actualMs = r.optDouble("actual_ms", 0.0).toInt(),
                    deltaLapMs = r.optDouble("delta_lap_ms", 0.0).toInt(),
                    deltaStintMs = r.optDouble("delta_stint_ms", 0.0).toInt(),
                    deltaTotalMs = r.optDouble("delta_total_ms", 0.0).toInt(),
                    hasActual = r.optBoolean("has_actual"),
                )
            },
        )
    },
)

private fun JSONArray?.objects(): List<JSONObject> =
    if (this == null) emptyList() else List(length()) { optJSONObject(it) ?: JSONObject() }
