package com.nogoat.tracknrace

import org.json.JSONArray
import org.json.JSONObject

/** Laps this close through a braking zone count as level, as on the desktop. */
internal const val BRAKING_LEVEL_S = 0.01

/**
 * Something libtnrp found worth pointing out about a braking zone, as facts
 * to word (tnrp/AnalysisMap.h BrakingObservation). [lap] is 0 for the current
 * lap and 1 for the comparison lap.
 */
internal class BrakingObservation(val kind: String, val lap: Int, val value: Double, val other: Double)

/** One corner inside a braking zone, flat-out ones included. */
internal class BrakingApex(
    val distanceM: Double,
    val speedKph: Double,
    /** -1 left, 1 right, 0 when found from speed alone. */
    val side: Int,
    val hasLine: Boolean,
    /** Metres further inside the bend than the other lap; negative when wider. */
    val lineGapM: Double,
)

/** One braking zone of one lap. -1 marks a figure that never happened or is unknown. */
internal class BrakingPoint(
    val liftOnly: Boolean,
    val distanceM: Double,
    val speedKph: Double,
    val coastM: Double,
    val coastS: Double,
    val brakingDistanceM: Double,
    val minSpeedKph: Double,
    val minSpeedM: Double,
    val apexes: List<BrakingApex>,
    val endM: Double,
    val throttleOnM: Double,
    val throttleOnS: Double,
    val fullThrottleM: Double,
    val fullThrottleS: Double,
    val throttleToFullM: Double,
    val wheelspin: Boolean,
    val wheelspinPct: Double,
    val wheelspinS: Double,
)

/** Two laps' braking zones matched up: indices into [BrakingZones.current] and [BrakingZones.comparison]. */
internal class BrakingPair(
    val current: Int,
    val comparison: Int,
    val sameBrakingPoint: Boolean,
    val fromM: Double,
    val toM: Double,
    val currentTimeS: Double,
    val comparisonTimeS: Double,
    val currentHarvestKj: Double,
    val comparisonHarvestKj: Double,
    val observations: List<BrakingObservation>,
)

/**
 * The braking zones of the laps on show, as libtnrp found them
 * (tnrp/AnalysisMap.h BrakingPointsMap): each lap's zones in lap order and,
 * with two laps, how they pair up.
 */
internal class BrakingZones(
    val current: List<BrakingPoint>,
    val comparison: List<BrakingPoint>,
    val pairs: List<BrakingPair>,
) {
    val paired: Boolean get() = comparison.isNotEmpty()

    /** One row per zone: the pairs with two laps, else the current lap's zones. */
    val zones: List<BrakingZone> by lazy {
        if (paired) {
            pairs.map { pair ->
                val timed = pair.currentTimeS >= 0 && pair.comparisonTimeS >= 0
                BrakingZone(
                    number = pair.current + 1,
                    fromM = pair.fromM,
                    toM = pair.toM,
                    // Positive: the current lap was slower through the zone, as the delta reads.
                    gapS = if (timed) pair.currentTimeS - pair.comparisonTimeS else null,
                    current = current.getOrNull(pair.current),
                    comparison = comparison.getOrNull(pair.comparison),
                    pair = pair,
                )
            }
        } else {
            current.mapIndexed { index, point ->
                BrakingZone(
                    number = index + 1,
                    fromM = point.distanceM,
                    toM = maxOf(point.distanceM, point.endM),
                    gapS = null,
                    current = point,
                    comparison = null,
                    pair = null,
                )
            }
        }
    }

    companion object {
        fun parse(data: JSONObject?): BrakingZones? {
            data ?: return null
            return BrakingZones(
                current = points(data.optJSONArray("current")),
                comparison = points(data.optJSONArray("comparison")),
                pairs = objects(data.optJSONArray("pairs")).map { pair ->
                    BrakingPair(
                        current = pair.optInt("current"),
                        comparison = pair.optInt("comparison"),
                        sameBrakingPoint = pair.optBoolean("sameBrakingPoint", true),
                        fromM = pair.optDouble("fromM", 0.0),
                        toM = pair.optDouble("toM", 0.0),
                        currentTimeS = pair.optDouble("currentTimeS", -1.0),
                        comparisonTimeS = pair.optDouble("comparisonTimeS", -1.0),
                        currentHarvestKj = pair.optDouble("currentHarvestKj", -1.0),
                        comparisonHarvestKj = pair.optDouble("comparisonHarvestKj", -1.0),
                        observations = objects(pair.optJSONArray("observations")).map {
                            BrakingObservation(
                                it.optString("kind"),
                                it.optInt("lap"),
                                it.optDouble("value", 0.0),
                                it.optDouble("other", 0.0),
                            )
                        },
                    )
                },
            )
        }

        private fun points(array: JSONArray?): List<BrakingPoint> = objects(array).map { point ->
            BrakingPoint(
                liftOnly = point.optBoolean("liftOnly"),
                distanceM = point.optDouble("distanceM", 0.0),
                speedKph = point.optDouble("speedKph", 0.0),
                coastM = point.optDouble("coastM", 0.0),
                coastS = point.optDouble("coastS", 0.0),
                brakingDistanceM = point.optDouble("brakingDistanceM", -1.0),
                minSpeedKph = point.optDouble("minSpeedKph", 0.0),
                minSpeedM = point.optDouble("minSpeedM", 0.0),
                apexes = objects(point.optJSONArray("apexes")).map {
                    BrakingApex(
                        distanceM = it.optDouble("distanceM", 0.0),
                        speedKph = it.optDouble("speedKph", 0.0),
                        side = it.optInt("side"),
                        hasLine = it.optBoolean("hasLine"),
                        lineGapM = it.optDouble("lineGapM", 0.0),
                    )
                },
                endM = point.optDouble("endM", 0.0),
                throttleOnM = point.optDouble("throttleOnM", -1.0),
                throttleOnS = point.optDouble("throttleOnS", -1.0),
                fullThrottleM = point.optDouble("fullThrottleM", -1.0),
                fullThrottleS = point.optDouble("fullThrottleS", -1.0),
                throttleToFullM = point.optDouble("throttleToFullM", -1.0),
                wheelspin = point.optBoolean("wheelspin"),
                wheelspinPct = point.optDouble("wheelspinPct", 0.0),
                wheelspinS = point.optDouble("wheelspinS", 0.0),
            )
        }

        private fun objects(array: JSONArray?): List<JSONObject> = buildList {
            if (array != null) repeat(array.length()) { index -> array.optJSONObject(index)?.let(::add) }
        }
    }
}

/**
 * One row of the zone list: its number, where it starts and ends, the gap
 * through it (null with one lap, or when a lap does not cover it), and each
 * lap's zone.
 */
internal class BrakingZone(
    val number: Int,
    val fromM: Double,
    val toM: Double,
    val gapS: Double?,
    val current: BrakingPoint?,
    val comparison: BrakingPoint?,
    val pair: BrakingPair?,
) {
    /** 0 when the current lap was quicker through it, 1 the comparison lap, null when level or untimed. */
    val quicker: Int?
        get() = gapS?.takeIf { kotlin.math.abs(it) >= BRAKING_LEVEL_S }?.let { if (it > 0) 1 else 0 }
}
