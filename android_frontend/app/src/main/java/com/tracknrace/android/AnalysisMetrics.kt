package com.tracknrace.android

import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale
import kotlin.math.abs
import kotlin.math.roundToInt

// Port of the desktop Analyze metric catalogue (electron-frontend
// lib/analyzeMetrics.ts): the same ids, scales, colours and formatting, so a
// graph reads the same on both. Values are plotted normalised to [min, max].

internal enum class AnalysisView { GRAPH, SPLIT, MAP }

/**
 * One plottable metric. [fields] are the row fields it reads from [family],
 * and [compute] turns their values at one sample into the plotted value.
 */
internal class AnalysisMetric(
    val id: String,
    val group: String,
    val label: String,
    val family: String,
    val fields: List<String>,
    val defaultColor: Int,
    val scaleKey: String,
    val min: Double,
    val max: Double,
    val unit: String,
    val step: Boolean = false,
    val compute: (DoubleArray) -> Double = { it[0] },
    val format: (Double) -> String,
    val axisFormat: (Double) -> String,
) {
    /** The "family.field" names the desktop is asked for. */
    val channels: List<String> = fields.map { "$family.$it" }
}

internal data class AnalysisCorner(val key: String, val label: String, val color: Int)

internal data class AnalysisTyreRow(
    val idPrefix: String,
    val label: String,
    val shortLabel: String,
    val combinedColor: Int,
)

/** A card that plots the picked corners of one tyre metric on a shared scale. */
internal data class AnalysisCombinedMetric(
    val id: String,
    val label: String,
    val unit: String,
    val idPrefix: String,
    val defaultColor: Int,
    val memberIds: List<String>,
)

private fun rgb(value: Int): Int = (0xff shl 24) or value

private fun fixed(value: Double, digits: Int): String = String.format(Locale.US, "%.${digits}f", value)

private fun withUnit(unit: String, digits: Int = 0): (Double) -> String = { "${fixed(it, digits)}$unit" }

private fun plain(digits: Int = 0): (Double) -> String = { fixed(it, digits) }

private fun percentOfOne(value: Double): String = "${(value * 100).roundToInt()}%"

private fun orZero(value: Double): Double = if (value.isNaN()) 0.0 else value

internal object AnalysisCatalog {
    const val DELTA_ID = "delta"

    val corners = listOf(
        AnalysisCorner("fl", "FL", rgb(0xe10600)),
        AnalysisCorner("fr", "FR", rgb(0x4488ff)),
        AnalysisCorner("rl", "RL", rgb(0x37872D)),
        AnalysisCorner("rr", "RR", rgb(0xffd700)),
    )

    val categories = listOf("Driving", "Motion", "Power", "Tyres")

    private val base = listOf(
        AnalysisMetric("speed", "Driving", "Speed", "telemetry", listOf("speed_kph"), rgb(0x37872D), "speed", 0.0, 380.0, "km/h", format = withUnit(" km/h"), axisFormat = plain()),
        AnalysisMetric("rpm", "Driving", "RPM", "telemetry", listOf("rpm"), rgb(0xC4162A), "rpm", 0.0, 16000.0, "rpm",
            format = { String.format(Locale.US, "%,d rpm", it.roundToInt()) },
            axisFormat = { if (it == 0.0) "0" else "${(it / 1000).roundToInt()}k" }),
        AnalysisMetric("gear", "Driving", "Gear", "telemetry", listOf("gear"), rgb(0x5794F2), "gear", 0.5, 8.5, "", step = true,
            format = { "Gear ${it.roundToInt()}" }, axisFormat = { it.roundToInt().toString() }),
        AnalysisMetric("throttle", "Driving", "Throttle", "telemetry", listOf("throttle"), rgb(0x37872D), "input-positive", 0.0, 1.0, "%", step = true,
            format = ::percentOfOne, axisFormat = ::percentOfOne),
        AnalysisMetric("brake", "Driving", "Brake", "telemetry", listOf("brake"), rgb(0xC4162A), "input-positive", 0.0, 1.0, "%", step = true,
            format = ::percentOfOne, axisFormat = ::percentOfOne),
        AnalysisMetric("steering", "Driving", "Steering", "telemetry", listOf("steering"), rgb(0xBF5FFF), "input-signed", -1.0, 1.0, "%",
            format = { "${if (it < 0) "L " else if (it > 0) "R " else ""}${(abs(it) * 100).roundToInt()}%" },
            axisFormat = ::percentOfOne),
        AnalysisMetric("ers", "Driving", "ERS", "status", listOf("ers_pct"), rgb(0xFADE2A), "percent", 0.0, 100.0, "%", format = withUnit("%", 1), axisFormat = withUnit("%")),
        AnalysisMetric("g-lateral", "Motion", "Lateral G", "motion", listOf("g_lat"), rgb(0xF0A500), "g-force", -6.0, 6.0, "g", format = withUnit(" g", 2), axisFormat = withUnit("g")),
        AnalysisMetric("g-longitudinal", "Motion", "Longitudinal G", "motion", listOf("g_long"), rgb(0x5794F2), "g-force", -6.0, 6.0, "g", format = withUnit(" g", 2), axisFormat = withUnit("g")),
        AnalysisMetric("ride-front", "Motion", "Front Ride Height", "motion_ex", listOf("front_aero_height_mm"), rgb(0x73BF69), "ride-height", -2.0, 20.0, "mm", format = withUnit(" mm", 1), axisFormat = withUnit("mm")),
        AnalysisMetric("ride-rear", "Motion", "Rear Ride Height", "motion_ex", listOf("rear_aero_height_mm"), rgb(0xB877DB), "ride-height", -2.0, 20.0, "mm", format = withUnit(" mm", 1), axisFormat = withUnit("mm")),
        AnalysisMetric("power-ice", "Power", "ICE Power", "status", listOf("engine_power_ice_kw"), rgb(0x5794F2), "power", 0.0, 1000.0, "kW",
            compute = { orZero(it[0]) }, format = withUnit(" kW", 1), axisFormat = withUnit("kW")),
        AnalysisMetric("power-mguk", "Power", "MGU-K Power", "status", listOf("engine_power_mguk_kw"), rgb(0xFADE2A), "power", 0.0, 1000.0, "kW",
            compute = { orZero(it[0]) }, format = withUnit(" kW", 1), axisFormat = withUnit("kW")),
        AnalysisMetric("harvest-mguk", "Power", "MGU-K Harvest", "status", listOf("ers_harvested_mguk_j"), rgb(0x37872D), "harvest", 0.0, 2000.0, "kJ",
            compute = { orZero(it[0]) / 1000 }, format = withUnit(" kJ", 1), axisFormat = withUnit("kJ")),
        AnalysisMetric("harvest-mguh", "Power", "MGU-H Harvest", "status", listOf("ers_harvested_mguh_j"), rgb(0xC4162A), "harvest", 0.0, 2000.0, "kJ",
            compute = { orZero(it[0]) / 1000 }, format = withUnit(" kJ", 1), axisFormat = withUnit("kJ")),
        AnalysisMetric("fuel", "Power", "Fuel", "status", listOf("fuel_kg"), rgb(0xF0A500), "fuel", 0.0, 110.0, "kg", format = withUnit(" kg", 2), axisFormat = withUnit("kg")),
    )

    private val tyreMetrics = corners.flatMap { corner ->
        listOf(
            AnalysisMetric("surface-${corner.key}", "Tyres", "Surface Temp ${corner.label}", "telemetry", listOf("tyre_temp_surface_${corner.key}"), corner.color, "tyre-temp", 0.0, 125.0, "°C", format = withUnit(" °C", 1), axisFormat = withUnit("°")),
            AnalysisMetric("inner-${corner.key}", "Tyres", "Inner Temp ${corner.label}", "telemetry", listOf("tyre_temp_inner_${corner.key}"), corner.color, "tyre-temp", 0.0, 125.0, "°C", format = withUnit(" °C", 1), axisFormat = withUnit("°")),
            AnalysisMetric("brake-temp-${corner.key}", "Tyres", "Brake Temp ${corner.label}", "telemetry", listOf("brake_temp_${corner.key}"), corner.color, "brake-temp", 0.0, 1250.0, "°C", format = withUnit(" °C", 1), axisFormat = withUnit("°")),
            AnalysisMetric("wear-${corner.key}", "Tyres", "Tyre Wear ${corner.label}", "damage", listOf("tyre_wear_${corner.key}"), corner.color, "percent", 0.0, 100.0, "%", format = withUnit("%", 1), axisFormat = withUnit("%")),
            AnalysisMetric("life-${corner.key}", "Tyres", "Tyre Life ${corner.label}", "damage", listOf("tyre_wear_${corner.key}"), corner.color, "percent", 0.0, 100.0, "%",
                compute = { 100 - it[0] }, format = withUnit("%", 1), axisFormat = withUnit("%")),
        )
    }

    // The baseline across all four corners: the mean of whichever corners report.
    private fun averageWear(values: DoubleArray): Double {
        var sum = 0.0
        var count = 0
        for (value in values) if (value.isFinite()) { sum += value; count++ }
        return if (count > 0) sum / count else Double.NaN
    }

    private val wearFields = corners.map { "tyre_wear_${it.key}" }

    private val averageMetrics = listOf(
        AnalysisMetric("wear-avg", "Tyres", "Average Tyre Wear", "damage", wearFields, rgb(0xFF780A), "percent", 0.0, 100.0, "%",
            compute = ::averageWear, format = withUnit("%", 1), axisFormat = withUnit("%")),
        AnalysisMetric("life-avg", "Tyres", "Average Tyre Life", "damage", wearFields, rgb(0x73BF69), "percent", 0.0, 100.0, "%",
            compute = { 100 - averageWear(it) }, format = withUnit("%", 1), axisFormat = withUnit("%")),
    )

    val metrics: List<AnalysisMetric> = base + tyreMetrics + averageMetrics
    val metricById: Map<String, AnalysisMetric> = metrics.associateBy { it.id }

    val tyreRows = listOf(
        AnalysisTyreRow("surface", "Surface Temp", "Surface", rgb(0xFF9830)),
        AnalysisTyreRow("inner", "Inner Temp", "Inner", rgb(0xB877DB)),
        AnalysisTyreRow("brake-temp", "Brake Temp", "Brake", rgb(0xF2495C)),
        AnalysisTyreRow("wear", "Tyre Wear", "T.Wear", rgb(0x8AB8FF)),
        AnalysisTyreRow("life", "Tyre Life", "T.Life", rgb(0x73BF69)),
    )

    val combined: List<AnalysisCombinedMetric> = tyreRows.map { row ->
        AnalysisCombinedMetric(
            id = "${row.idPrefix}-all",
            label = "${row.label} · Combined",
            unit = metricById.getValue("${row.idPrefix}-${corners[0].key}").unit,
            idPrefix = row.idPrefix,
            defaultColor = row.combinedColor,
            memberIds = corners.map { "${row.idPrefix}-${it.key}" },
        )
    }
    val combinedById: Map<String, AnalysisCombinedMetric> = combined.associateBy { it.id }

    /** Per-corner metric ids, which the picker shows as a matrix instead of chips. */
    val cornerMetricIds: Set<String> = tyreRows.flatMap { row -> corners.map { "${row.idPrefix}-${it.key}" } }.toSet()

    private val cornerKeys = corners.map { it.key }

    /** Valid corner keys in FL, FR, RL, RR order; null means all four. */
    fun sanitizeCorners(value: List<String>?): List<String> =
        if (value == null) cornerKeys else cornerKeys.filter { it in value }

    /** The metric definitions a series draws: itself, its chosen corners, or none (delta). */
    fun memberIds(series: AnalysisSeries): List<String> {
        val combined = combinedById[series.metricId]
        if (combined != null) return (series.corners ?: cornerKeys).map { "${combined.idPrefix}-$it" }
        return if (series.metricId in metricById) listOf(series.metricId) else emptyList()
    }

    fun cornerKeyOf(combinedId: String, memberId: String): String? {
        val combined = combinedById[combinedId] ?: return null
        return if (memberId.startsWith("${combined.idPrefix}-")) memberId.substring(combined.idPrefix.length + 1) else null
    }

    /** The colour a series draws one of its member lines in. */
    fun lineColor(series: AnalysisSeries, memberId: String): Int {
        val key = cornerKeyOf(series.metricId, memberId) ?: return series.color
        return series.cornerColors[key] ?: metricById[memberId]?.defaultColor ?: series.color
    }

    fun hasLines(series: AnalysisSeries): Boolean =
        series.metricId == DELTA_ID || memberIds(series).isNotEmpty()

    /** The definition whose scale and formatting a series' axis uses. */
    fun scaleMetric(metricId: String): AnalysisMetric? =
        metricById[combinedById[metricId]?.memberIds?.first() ?: metricId]

    /** Series ids that cannot be charted alongside [metricId]. */
    fun conflicts(metricId: String): List<String> {
        combinedById[metricId]?.let { return it.memberIds }
        val owner = combined.firstOrNull { metricId in it.memberIds }
        return if (owner != null) listOf(owner.id) else emptyList()
    }

    fun labelOf(series: AnalysisSeries): String {
        if (series.metricId == DELTA_ID) return "Delta"
        combinedById[series.metricId]?.let { combined ->
            return tyreRows.firstOrNull { it.idPrefix == combined.idPrefix }?.label ?: combined.label
        }
        return metricById[series.metricId]?.label ?: series.metricId
    }
}

internal const val DEFAULT_MAP_CURRENT_COLOR = (0xff shl 24) or 0x5794F2
internal const val DEFAULT_MAP_COMPARISON_COLOR = (0xff shl 24) or 0xC4162A
internal const val DEFAULT_DELTA_POSITIVE_COLOR = (0xff shl 24) or 0xC4162A
internal const val DEFAULT_DELTA_NEGATIVE_COLOR = (0xff shl 24) or 0x37872D

internal const val DEFAULT_CURRENT_LABEL = "Current"
internal const val DEFAULT_COMPARE_LABEL = "Compare"
internal const val DEFAULT_LAP_A_LABEL = "Lap A"
internal const val DEFAULT_LAP_B_LABEL = "Lap B"

internal data class AnalysisSeries(
    val metricId: String,
    val color: Int,
    val negativeColor: Int? = null,
    val visible: Boolean = true,
    val showYAxis: Boolean = true,
    /** Combined tyre cards only: the corner keys drawn on the card. */
    val corners: List<String>? = null,
    /** Combined tyre cards only: custom line colour per corner key. */
    val cornerColors: Map<String, Int> = emptyMap(),
)

/** The Analysis page's saved configuration, mirroring the desktop's. */
internal data class AnalysisConfig(
    val view: AnalysisView = AnalysisView.GRAPH,
    val individualGraphs: Boolean = false,
    val syncedTooltip: Boolean = false,
    val sectorBoundaries: Boolean = false,
    val sectorDelta: Boolean = false,
    val currentLabel: String = "",
    val compareLabel: String = "",
    val lapALabel: String = "",
    val lapBLabel: String = "",
    val mapCurrentColor: Int = DEFAULT_MAP_CURRENT_COLOR,
    val mapComparisonColor: Int = DEFAULT_MAP_COMPARISON_COLOR,
    val series: List<AnalysisSeries> = DEFAULT_SERIES,
) {
    val deltaSeries: AnalysisSeries? get() = series.firstOrNull { it.metricId == AnalysisCatalog.DELTA_ID }

    fun toJson(): String = JSONObject()
        .put("version", 1)
        .put("view", view.name)
        .put("individualGraphs", individualGraphs)
        .put("syncedTooltip", syncedTooltip)
        .put("sectorBoundaries", sectorBoundaries)
        .put("sectorDelta", sectorDelta)
        .put("currentLabel", currentLabel)
        .put("compareLabel", compareLabel)
        .put("lapALabel", lapALabel)
        .put("lapBLabel", lapBLabel)
        .put("mapCurrentColor", hexColor(mapCurrentColor))
        .put("mapComparisonColor", hexColor(mapComparisonColor))
        .put("series", JSONArray().apply {
            series.forEach { item ->
                put(JSONObject().apply {
                    put("metricId", item.metricId)
                    put("color", hexColor(item.color))
                    item.negativeColor?.let { put("negativeColor", hexColor(it)) }
                    put("visible", item.visible)
                    put("showYAxis", item.showYAxis)
                    item.corners?.let { put("corners", JSONArray(it)) }
                    if (item.cornerColors.isNotEmpty()) {
                        put("cornerColors", JSONObject().apply {
                            item.cornerColors.forEach { (key, color) -> put(key, hexColor(color)) }
                        })
                    }
                })
            }
        })
        .toString()

    companion object {
        val DEFAULT_SERIES = listOf("speed", "rpm", "ers").map {
            AnalysisSeries(it, AnalysisCatalog.metricById.getValue(it).defaultColor)
        } + AnalysisSeries(
            AnalysisCatalog.DELTA_ID,
            DEFAULT_DELTA_POSITIVE_COLOR,
            negativeColor = DEFAULT_DELTA_NEGATIVE_COLOR,
        )

        /** Reads a saved config, dropping anything unknown or conflicting, as the desktop does. */
        fun fromJson(json: String?): AnalysisConfig {
            val root = try {
                if (json.isNullOrEmpty()) return AnalysisConfig() else JSONObject(json)
            } catch (_: Exception) {
                return AnalysisConfig()
            }
            val seen = mutableSetOf<String>()
            val series = mutableListOf<AnalysisSeries>()
            val items = root.optJSONArray("series")
            if (items != null) repeat(items.length()) { index ->
                val item = items.optJSONObject(index) ?: return@repeat
                val id = item.optString("metricId")
                val visible = item.optBoolean("visible", true)
                val showYAxis = item.optBoolean("showYAxis", true)
                if (id == AnalysisCatalog.DELTA_ID) {
                    if (seen.add(id)) {
                        series += AnalysisSeries(
                            id,
                            parseColor(item.optString("color")) ?: DEFAULT_DELTA_POSITIVE_COLOR,
                            negativeColor = parseColor(item.optString("negativeColor")) ?: DEFAULT_DELTA_NEGATIVE_COLOR,
                            visible = visible,
                            showYAxis = showYAxis,
                        )
                    }
                    return@repeat
                }
                val combined = AnalysisCatalog.combinedById[id]
                val metric = AnalysisCatalog.metricById[id]
                if ((combined == null && metric == null) || id in seen ||
                    AnalysisCatalog.conflicts(id).any { it in seen }
                ) return@repeat
                seen += id
                val color = parseColor(item.optString("color"))
                    ?: combined?.defaultColor ?: metric!!.defaultColor
                series += if (combined != null) {
                    val corners = item.optJSONArray("corners")?.let { array ->
                        List(array.length()) { array.optString(it) }
                    }
                    val cornerColors = buildMap {
                        val colors = item.optJSONObject("cornerColors")
                        if (colors != null) {
                            for (corner in AnalysisCatalog.corners) {
                                parseColor(colors.optString(corner.key))?.let { put(corner.key, it) }
                            }
                        }
                    }
                    AnalysisSeries(
                        id, color, visible = visible, showYAxis = showYAxis,
                        corners = AnalysisCatalog.sanitizeCorners(corners),
                        cornerColors = cornerColors,
                    )
                } else {
                    AnalysisSeries(id, color, visible = visible, showYAxis = showYAxis)
                }
            }
            if (items == null) return AnalysisConfig().copy(view = readView(root))
            if (AnalysisCatalog.DELTA_ID !in seen) series += DEFAULT_SERIES.last()
            fun label(key: String) = root.optString(key).take(40)
            return AnalysisConfig(
                view = readView(root),
                individualGraphs = root.optBoolean("individualGraphs"),
                syncedTooltip = root.optBoolean("syncedTooltip"),
                sectorBoundaries = root.optBoolean("sectorBoundaries"),
                sectorDelta = root.optBoolean("sectorDelta"),
                currentLabel = label("currentLabel"),
                compareLabel = label("compareLabel"),
                lapALabel = label("lapALabel"),
                lapBLabel = label("lapBLabel"),
                mapCurrentColor = parseColor(root.optString("mapCurrentColor")) ?: DEFAULT_MAP_CURRENT_COLOR,
                mapComparisonColor = parseColor(root.optString("mapComparisonColor")) ?: DEFAULT_MAP_COMPARISON_COLOR,
                series = series,
            )
        }

        private fun readView(root: JSONObject): AnalysisView =
            AnalysisView.entries.firstOrNull { it.name == root.optString("view") } ?: AnalysisView.GRAPH
    }
}

internal fun hexColor(color: Int): String = String.format(Locale.US, "#%06X", color and 0xffffff)

internal fun parseColor(value: String?): Int? {
    if (value == null || !Regex("^#[0-9a-fA-F]{6}$").matches(value)) return null
    return (0xff shl 24) or value.substring(1).toInt(16)
}
