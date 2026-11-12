package com.nogoat.tracknrace

import androidx.annotation.DrawableRes
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.floor

/**
 * The Analysis map's layers, as on the desktop. [native] names the libtnrp
 * calculation (tnrp/AnalysisMap.h) that paints it; None and Racing Line need
 * none. [perLap] layers paint each lap on its own, so the map shows one lap,
 * both laps stacked, or both as two bands ([MapLapView]).
 */
internal enum class MapLayer(
    val id: String,
    val label: String,
    val native: String?,
    val perLap: Boolean,
    @param:DrawableRes val preview: Int,
) {
    NONE("none", "None", null, false, R.drawable.map_layer_none),
    DOMINANCE("dominance", "Track dominance", "dominance", false, R.drawable.map_layer_dominance),
    RACING_LINE("racing", "Racing line", null, false, R.drawable.map_layer_racing),
    INPUTS("inputs", "Driver inputs", "inputs", true, R.drawable.map_layer_inputs),
    SPEED("speed", "Speed", "speed", true, R.drawable.map_layer_speed),
    ERS("ers", "ERS usage", "ers", true, R.drawable.map_layer_ers),
    HARVEST("harvest", "ERS harvest", "harvest", true, R.drawable.map_layer_harvest);

    companion object {
        fun fromId(id: String?): MapLayer = entries.firstOrNull { it.id == id } ?: NONE
    }
}

/**
 * How a per-lap layer shows the two laps: the current lap alone, the
 * comparison lap alone, both on maps of their own, or both on one map as two
 * bands side by side.
 */
internal enum class MapLapView(val id: String) {
    CURRENT("current"),
    COMPARISON("comparison"),
    BOTH("both"),
    COMBINED("combined");

    companion object {
        fun fromId(id: String?): MapLapView = entries.firstOrNull { it.id == id } ?: CURRENT
    }
}

/** A corner's slowest speed on the Speed layer. */
internal class MapApex(val distanceM: Double, val speedKph: Double, val side: Int)

/**
 * One lap's layer, painted by libtnrp: a colour (ARGB) per [binM] metres from
 * the timing line, or spread evenly over the lap when [binM] is 0.
 */
internal class MapLapPaint(
    val binM: Double,
    val trackLengthM: Double,
    val colors: IntArray,
    /** Every ERS field read zero: the driver's telemetry is Restricted. */
    val restricted: Boolean,
    val apexes: List<MapApex>,
) {
    /** The colour at [fraction] (0-1) of the lap from the timing line, or null when unpainted. */
    fun colorAt(fraction: Double): Int? {
        if (colors.isEmpty()) return null
        val n = colors.size
        val bin = if (binM > 0.0 && trackLengthM > 0.0) floor(fraction * trackLengthM / binM) else floor(fraction * n)
        return colors[bin.toInt().coerceIn(0, n - 1)]
    }

    companion object {
        fun parse(json: JSONObject?): MapLapPaint? {
            json ?: return null
            val colors = json.optJSONArray("colors") ?: JSONArray()
            val apexes = json.optJSONArray("apexes") ?: JSONArray()
            return MapLapPaint(
                binM = json.optDouble("binM", 0.0),
                trackLengthM = json.optDouble("trackLengthM", 0.0),
                colors = IntArray(colors.length()) { parseColor(colors.optString(it)) ?: 0 },
                restricted = json.optBoolean("restricted"),
                apexes = List(apexes.length()) { index ->
                    val apex = apexes.optJSONObject(index)
                    MapApex(apex?.optDouble("distanceM") ?: 0.0, apex?.optDouble("speedKph") ?: 0.0, apex?.optInt("side") ?: 0)
                },
            )
        }
    }
}

/** An `analysis_map` reply: one layer painted for each lap, and its legend's scales. */
internal class MapLayerPaint(
    val layer: MapLayer,
    val current: MapLapPaint?,
    val comparison: MapLapPaint?,
    val speedMinKph: Double,
    val speedMaxKph: Double,
    val ersPeakKw: Double,
    val harvestPeakKw: Double,
) {
    companion object {
        fun parse(json: JSONObject?): MapLayerPaint? {
            json ?: return null
            return MapLayerPaint(
                layer = MapLayer.fromId(json.optString("layer")),
                current = MapLapPaint.parse(json.optJSONObject("current")),
                comparison = MapLapPaint.parse(json.optJSONObject("comparison")),
                speedMinKph = json.optDouble("speedMinKph", 0.0),
                speedMaxKph = json.optDouble("speedMaxKph", 0.0),
                ersPeakKw = json.optDouble("ersPeakKw", 0.0),
                harvestPeakKw = json.optDouble("harvestPeakKw", 0.0),
            )
        }
    }
}

/** One theme's layer colours from libtnrp (tnrp/AnalysisMapPaint.h), for the legends. */
internal class MapLayerPalette(
    val ers: List<Int>,
    val harvest: List<Int>,
    val speed: List<Int>,
    val superclip: Int,
    /** Coasting, partial throttle, full throttle, braking. */
    val inputs: List<Int>,
    val inputLabels: List<String>,
    val level: Int,
) {
    companion object {
        private var cached: Pair<MapLayerPalette, MapLayerPalette>? = null

        /** The dark or light palette; read from the native library once. */
        fun of(dark: Boolean): MapLayerPalette? {
            val both = cached ?: runCatching {
                val root = JSONObject(NativeTelemetry.nativeAnalysisMapPaletteJson())
                (parse(root.getJSONObject("dark")) to parse(root.getJSONObject("light"))).also { cached = it }
            }.getOrNull() ?: return null
            return if (dark) both.first else both.second
        }

        private fun colors(array: JSONArray?): List<Int> =
            if (array == null) emptyList() else List(array.length()) { parseColor(array.optString(it)) ?: 0 }

        private fun parse(json: JSONObject): MapLayerPalette {
            val labels = json.optJSONArray("inputLabels")
            return MapLayerPalette(
                ers = colors(json.optJSONObject("ers")?.optJSONArray("colors")),
                harvest = colors(json.optJSONObject("harvest")?.optJSONArray("colors")),
                speed = colors(json.optJSONObject("speed")?.optJSONArray("colors")),
                superclip = parseColor(json.optString("superclip")) ?: 0,
                inputs = colors(json.optJSONArray("inputs")),
                inputLabels = if (labels == null) emptyList() else List(labels.length()) { labels.optString(it) },
                level = parseColor(json.optString("level")) ?: 0,
            )
        }
    }
}
