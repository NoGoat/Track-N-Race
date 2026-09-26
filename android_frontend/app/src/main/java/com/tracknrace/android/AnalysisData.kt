package com.tracknrace.android

import android.util.JsonReader
import android.util.JsonToken
import org.json.JSONObject
import java.io.StringReader
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/** One row family's samples: times in seconds from the lap start, one array per field. */
internal class AnalysisFamilyData(
    val times: FloatArray,
    val fields: Map<String, FloatArray>,
)

/** A plotted metric: x in seconds or metres, y normalised to the metric's [min, max]. */
internal class AnalysisPlot(val x: FloatArray, val y: FloatArray) {
    val size: Int get() = x.size

    /** Index of the sample nearest to [value] on the x axis, or -1 when empty. */
    fun nearestIndex(value: Double): Int {
        if (x.isEmpty()) return -1
        var lo = 0
        var hi = x.size
        while (lo < hi) {
            val mid = (lo + hi) ushr 1
            if (x[mid] < value) lo = mid + 1 else hi = mid
        }
        if (lo == 0) return 0
        if (lo == x.size) return x.size - 1
        return if (value - x[lo - 1] <= x[lo] - value) lo - 1 else lo
    }
}

internal data class AnalysisSectorSplit(
    /** Completed sector number; the next sector starts here. */
    val afterSector: Int,
    val distance: Double,
    val elapsedSeconds: Double,
)

/**
 * One recorded lap of the desktop's selected driver, as sent by
 * `request_lap_data`. Every time is seconds from [startSessionTime].
 */
internal class AnalysisLap(
    val lapNumber: Int,
    val startSessionTime: Double,
    val endSessionTime: Double,
    val progressTime: FloatArray,
    val progressDistance: FloatArray,
    val progressElapsedMs: FloatArray,
    val progressSector: FloatArray,
    val progressS1Ms: FloatArray,
    val progressS2Ms: FloatArray,
    val positionTime: FloatArray,
    val positionX: FloatArray,
    val positionZ: FloatArray,
    val families: Map<String, AnalysisFamilyData>,
    val channels: Set<String>,
) {
    val duration: Double get() = max(0.0, endSessionTime - startSessionTime)

    val progress: AnalysisProgress? by lazy { AnalysisProgress.build(this) }

    val sectorSplits: List<AnalysisSectorSplit> by lazy { findSectorSplits(this) }

    val lastDistance: Double
        get() = progressDistance.lastOrNull()?.toDouble()?.takeIf { it.isFinite() } ?: 0.0

    // Built on the raster thread and read by the tooltip on the main thread.
    private val plots = java.util.concurrent.ConcurrentHashMap<String, AnalysisPlot>()

    /** The metric's samples on the chosen x axis; built once and kept with the lap. */
    fun plot(metric: AnalysisMetric, distanceMode: Boolean): AnalysisPlot? {
        val key = "${metric.id}:$distanceMode"
        plots[key]?.let { return it }
        val family = families[metric.family] ?: return null
        val columns = metric.fields.map { family.fields[it] ?: return null }
        val progress = if (distanceMode) progress ?: return null else null
        val xs = FloatArray(family.times.size)
        val ys = FloatArray(family.times.size)
        val scratch = DoubleArray(columns.size)
        val span = metric.max - metric.min
        var count = 0
        for (index in family.times.indices) {
            val time = family.times[index].toDouble()
            val x = if (progress != null) {
                if (time > progress.maxTime) break
                progress.distanceAt(time)
            } else {
                time
            }
            if (!x.isFinite()) continue
            for (column in columns.indices) scratch[column] = columns[column][index].toDouble()
            val value = metric.compute(scratch)
            val y = if (value.isFinite()) ((value - metric.min) / span).toFloat() else Float.NaN
            // Distance can stall while time advances; keep the last sample there.
            if (count > 0 && x.toFloat() == xs[count - 1]) {
                ys[count - 1] = y
                continue
            }
            if (count > 0 && x.toFloat() < xs[count - 1]) continue
            xs[count] = x.toFloat()
            ys[count] = y
            count++
        }
        return AnalysisPlot(xs.copyOf(count), ys.copyOf(count)).also { plots[key] = it }
    }

    /** The latest sample of one field at or before [time] (seconds from lap start). */
    fun fieldAt(family: String, field: String, time: Double): Double {
        val data = families[family] ?: return Double.NaN
        val column = data.fields[field] ?: return Double.NaN
        val index = lastIndexAtOrBefore(data.times, time)
        return if (index >= 0) column[index].toDouble() else Double.NaN
    }

    /** The car's world position at [time], interpolated, or null without positions. */
    fun positionAt(time: Double): Pair<Double, Double>? {
        if (positionTime.isEmpty()) return null
        if (positionTime.size == 1) return positionX[0].toDouble() to positionZ[0].toDouble()
        val target = time.coerceIn(positionTime.first().toDouble(), positionTime.last().toDouble())
        val after = firstIndexAfter(positionTime, target).coerceIn(1, positionTime.size - 1)
        val before = after - 1
        val span = positionTime[after] - positionTime[before]
        val ratio = if (span > 0) ((target - positionTime[before]) / span).coerceIn(0.0, 1.0) else 0.0
        return (positionX[before] + (positionX[after] - positionX[before]) * ratio) to
            (positionZ[before] + (positionZ[after] - positionZ[before]) * ratio)
    }

    /** Whether this lap already carries every channel in [needed]. */
    fun covers(needed: Collection<String>): Boolean = channels.containsAll(needed)
}

private fun lastIndexAtOrBefore(times: FloatArray, time: Double): Int = firstIndexAfter(times, time) - 1

private fun firstIndexAfter(times: FloatArray, time: Double): Int {
    var lo = 0
    var hi = times.size
    while (lo < hi) {
        val mid = (lo + hi) ushr 1
        if (times[mid] <= time) lo = mid + 1 else hi = mid
    }
    return lo
}

/**
 * Monotonic time -> distance -> elapsed mapping for one lap, built exactly as
 * the desktop's `buildLapProgressMapFromPoints`: a synthetic lap-start point,
 * then every sample that moves forward in both time and distance.
 */
internal class AnalysisProgress private constructor(
    private val time: DoubleArray,
    private val distance: DoubleArray,
    private val elapsedMs: DoubleArray,
) {
    val maxTime: Double get() = time.last()
    val maxDistance: Double get() = distance.last()
    val minDistance: Double get() = distance.first()

    /** Lap distance at [sessionOffset] seconds into the lap, NaN outside it. */
    fun distanceAt(sessionOffset: Double): Double =
        interpolate(time, distance, sessionOffset)

    /** Lap elapsed seconds at [metres], NaN outside the recorded distance. */
    fun elapsedAtDistance(metres: Double): Double =
        interpolate(distance, elapsedMs, metres) / 1000.0

    /** Session offset (seconds from lap start) at [metres]. */
    fun timeAtDistance(metres: Double): Double = interpolate(distance, time, metres)

    fun distanceAtElapsedMs(milliseconds: Double): Double =
        interpolate(elapsedMs, distance, milliseconds)

    private fun interpolate(keys: DoubleArray, values: DoubleArray, key: Double): Double {
        if (!key.isFinite() || key < keys.first() || key > keys.last()) return Double.NaN
        var lo = 1
        var hi = keys.size
        while (lo < hi) {
            val mid = (lo + hi) ushr 1
            if (keys[mid] < key) lo = mid + 1 else hi = mid
        }
        if (lo >= keys.size) return values.last()
        val span = keys[lo] - keys[lo - 1]
        val ratio = if (span > 0) (key - keys[lo - 1]) / span else 1.0
        return values[lo - 1] + (values[lo] - values[lo - 1]) * ratio
    }

    companion object {
        fun build(lap: AnalysisLap): AnalysisProgress? {
            val count = lap.progressTime.size
            if (count == 0) return null
            val end = lap.duration
            // Race Lap 1 starts behind the line; keep the earliest distance
            // recorded at the lap-start boundary instead of inventing zero.
            var origin = Double.POSITIVE_INFINITY
            for (index in 0 until count) {
                val time = lap.progressTime[index].toDouble()
                if (time > end) break
                val distance = lap.progressDistance[index].toDouble()
                if (time < 0 || lap.progressElapsedMs[index] != 0f ||
                    !distance.isFinite() || distance < 0
                ) continue
                origin = min(origin, distance)
            }
            if (!origin.isFinite()) origin = 0.0
            val times = ArrayList<Double>(count + 1).apply { add(0.0) }
            val distances = ArrayList<Double>(count + 1).apply { add(origin) }
            val elapsed = ArrayList<Double>(count + 1).apply { add(0.0) }
            var lastTime = 0.0
            var lastDistance = origin
            for (index in 0 until count) {
                val time = lap.progressTime[index].toDouble()
                val distance = lap.progressDistance[index].toDouble()
                val elapsedMs = lap.progressElapsedMs[index].toDouble()
                if (time > end) break
                if (!time.isFinite() || !distance.isFinite() || !elapsedMs.isFinite() ||
                    time < lastTime || distance < lastDistance || elapsedMs < 0
                ) continue
                if (time == 0.0) continue
                if (times.size == 1) {
                    if (distance == origin) continue
                    times += time; distances += distance; elapsed += elapsedMs
                } else if (distance == lastDistance) {
                    continue
                } else if (time == lastTime) {
                    times[times.lastIndex] = time
                    distances[distances.lastIndex] = distance
                    elapsed[elapsed.lastIndex] = elapsedMs
                } else {
                    times += time; distances += distance; elapsed += elapsedMs
                }
                lastTime = time
                lastDistance = distance
            }
            if (times.size < 2) return null
            return AnalysisProgress(times.toDoubleArray(), distances.toDoubleArray(), elapsed.toDoubleArray())
        }
    }
}

private fun findSectorSplits(lap: AnalysisLap): List<AnalysisSectorSplit> {
    val progress = lap.progress ?: return emptyList()
    val splits = mutableListOf<AnalysisSectorSplit>()
    var enteredFirstSector = false
    for (index in lap.progressTime.indices) {
        val sector = lap.progressSector[index]
        if (!sector.isFinite()) continue
        val distance = lap.progressDistance[index]
        // Race Lap 1 starts behind the line, reported as sector 2 with a
        // negative distance; that is the previous lap's state.
        if (!enteredFirstSector) {
            if (sector == 0f && distance >= 0f) enteredFirstSector = true
            continue
        }
        val afterSector = when {
            splits.isEmpty() && sector >= 1f -> 1
            splits.size == 1 && sector >= 2f -> 2
            else -> continue
        }
        val s1 = lap.progressS1Ms[index].toDouble()
        val s2 = lap.progressS2Ms[index].toDouble()
        val exactMs = if (afterSector == 1) {
            s1
        } else if (s1.isFinite() && s2.isFinite() && s1 > 0 && s2 > 0) {
            s1 + s2
        } else {
            Double.NaN
        }
        val elapsedMs = if (exactMs.isFinite() && exactMs > 0) exactMs else lap.progressElapsedMs[index].toDouble()
        val splitDistance = progress.distanceAtElapsedMs(elapsedMs)
        if (!splitDistance.isFinite()) continue
        splits += AnalysisSectorSplit(afterSector, splitDistance, elapsedMs / 1000)
        if (splits.size == 2) break
    }
    return splits
}

/** Both laps' S1/S2 splits, the primary lap's taking precedence. */
internal fun resolvedSectorSplits(primary: AnalysisLap?, comparison: AnalysisLap?): List<AnalysisSectorSplit> {
    val bySector = HashMap<Int, AnalysisSectorSplit>()
    comparison?.sectorSplits?.forEach { bySector[it.afterSector] = it }
    primary?.sectorSplits?.forEach { bySector[it.afterSector] = it }
    return listOfNotNull(bySector[1], bySector[2])
}

/** The desktop's lap-delta curve between two laps, by lap distance. */
internal class AnalysisDeltaCurve(
    val currentLap: Int,
    val comparisonLap: Int,
    val sectorDelta: Boolean,
    val maxAbsDeltaSeconds: Double,
    val distance: DoubleArray,
    val delta: DoubleArray,
    val valid: BooleanArray,
) {
    val size: Int get() = distance.size

    /** The plotted half-range in seconds, as the desktop rounds it. */
    val range: Double get() = max(0.5, kotlin.math.ceil(maxAbsDeltaSeconds * 10) / 10)

    companion object {
        fun parse(data: JSONObject?): AnalysisDeltaCurve? {
            data ?: return null
            val samples = data.optJSONArray("samples") ?: return null
            val distance = ArrayList<Double>(samples.length())
            val delta = ArrayList<Double>(samples.length())
            val valid = ArrayList<Boolean>(samples.length())
            repeat(samples.length()) { index ->
                val sample = samples.optJSONObject(index) ?: return@repeat
                val metres = sample.optDouble("lap_distance_m", Double.NaN)
                val seconds = sample.optDouble("delta_seconds", Double.NaN)
                if (!metres.isFinite() || !seconds.isFinite()) return@repeat
                distance += metres
                delta += seconds
                valid += sample.optBoolean("valid", true)
            }
            return AnalysisDeltaCurve(
                currentLap = data.optInt("currentLapNum"),
                comparisonLap = data.optInt("comparisonLapNum"),
                sectorDelta = data.optBoolean("sectorDelta"),
                maxAbsDeltaSeconds = data.optDouble("maxAbsDeltaSeconds", 0.0).takeIf { it.isFinite() } ?: 0.0,
                distance = distance.toDoubleArray(),
                delta = delta.toDoubleArray(),
                valid = valid.toBooleanArray(),
            )
        }
    }
}

/** Sector and lap deltas, null where not reached or unknown. */
internal data class AnalysisDeltaSummary(
    val sectors: List<Double?> = listOf(null, null, null),
    val lap: Double? = null,
)

private fun interpolateDelta(curve: AnalysisDeltaCurve, indices: List<Int>, distance: Double?): Double? {
    if (distance == null) return null
    var before = -1
    for (index in indices) {
        val sample = curve.distance[index]
        if (sample < distance) {
            before = index
            continue
        }
        if (sample == distance) return curve.delta[index]
        if (before < 0) return null
        val span = sample - curve.distance[before]
        if (span <= 0) return curve.delta[index]
        val ratio = (distance - curve.distance[before]) / span
        return curve.delta[before] + (curve.delta[index] - curve.delta[before]) * ratio
    }
    return if (before >= 0) curve.delta[before] else null
}

/**
 * Port of the desktop's `summarizeAnalyzeDelta`. [visibleDistance] null means
 * the whole lap; NaN means the cursor position is unknown.
 */
internal fun summarizeDelta(
    curve: AnalysisDeltaCurve?,
    current: AnalysisLap?,
    comparison: AnalysisLap?,
    visibleDistance: Double?,
): AnalysisDeltaSummary {
    if (curve == null || current == null || comparison == null ||
        curve.currentLap != current.lapNumber || curve.comparisonLap != comparison.lapNumber ||
        visibleDistance?.isNaN() == true
    ) return AnalysisDeltaSummary()

    if (curve.sectorDelta) {
        val sectors = mutableListOf(mutableListOf<Int>())
        for (index in 0 until curve.size) {
            if (!curve.valid[index]) {
                if (sectors.last().isNotEmpty()) sectors += mutableListOf<Int>()
                continue
            }
            sectors.last() += index
        }
        val bySector = sectors.filter { it.isNotEmpty() }
        val full = bySector.lastOrNull()?.lastOrNull()?.let { curve.distance[it] }
        val visible = visibleDistance ?: full ?: return AnalysisDeltaSummary()
        val values = (0..2).map { sector ->
            val indices = bySector.getOrNull(sector)
            if (indices.isNullOrEmpty() || visible < curve.distance[indices.first()]) {
                null
            } else {
                interpolateDelta(curve, indices, min(visible, curve.distance[indices.last()]))
            }
        }
        val reached = values.filterNotNull()
        return AnalysisDeltaSummary(values, if (reached.isEmpty()) null else reached.sum())
    }

    val indices = (0 until curve.size).filter { curve.valid[it] }
    val full = indices.lastOrNull()?.let { curve.distance[it] }
    val visible = visibleDistance ?: full ?: return AnalysisDeltaSummary()
    val splits = resolvedSectorSplits(current, comparison)
    val sector1End = splits.firstOrNull { it.afterSector == 1 }?.distance
    val sector2End = splits.firstOrNull { it.afterSector == 2 }?.distance
    val ends = listOf(sector1End, sector2End, full)
    val starts = listOf(indices.firstOrNull()?.let { curve.distance[it] } ?: 0.0, sector1End, sector2End)
    val baselines = listOf(0.0, interpolateDelta(curve, indices, sector1End), interpolateDelta(curve, indices, sector2End))
    val values = (0..2).map { sector ->
        val start = starts[sector]
        val end = ends[sector]
        if (end == null || start == null || visible < start) return@map null
        val cumulative = interpolateDelta(curve, indices, min(visible, end))
        val baseline = baselines[sector]
        if (cumulative == null || baseline == null) null else cumulative - baseline
    }
    return AnalysisDeltaSummary(values, interpolateDelta(curve, indices, min(visible, full ?: visible)))
}

internal data class AnalysisLapReply(
    val requestId: Long,
    val lapNumber: Int,
    val lap: AnalysisLap?,
)

/** Streams a `lap_data` frame into columns without building a JSON tree. */
internal object AnalysisLapParser {
    fun parse(json: String, channels: Set<String>): AnalysisLapReply? = try {
        JsonReader(StringReader(json)).use { reader -> readFrame(reader, channels) }
    } catch (_: Exception) {
        null
    }

    /** Reads only the envelope's request id, for routing before a full parse. */
    fun requestIdOf(json: String): Long? {
        val match = Regex("\"requestId\":(\\d+)").find(json.take(96)) ?: return null
        return match.groupValues[1].toLongOrNull()
    }

    private fun readFrame(reader: JsonReader, channels: Set<String>): AnalysisLapReply {
        var requestId = 0L
        var lapNumber = 0
        var lap: AnalysisLap? = null
        reader.beginObject()
        while (reader.hasNext()) {
            when (reader.nextName()) {
                "requestId" -> requestId = reader.nextLong()
                "lapNum" -> lapNumber = reader.nextInt()
                "data" -> lap = if (reader.peek() == JsonToken.NULL) {
                    reader.nextNull()
                    null
                } else {
                    readLap(reader, channels)
                }
                else -> reader.skipValue()
            }
        }
        reader.endObject()
        return AnalysisLapReply(requestId, lapNumber, lap)
    }

    private fun readLap(reader: JsonReader, channels: Set<String>): AnalysisLap {
        var lapNumber = 0
        var start = 0.0
        var end = 0.0
        var progress = emptyMap<String, FloatArray>()
        var positions = emptyMap<String, FloatArray>()
        val families = HashMap<String, AnalysisFamilyData>()
        reader.beginObject()
        while (reader.hasNext()) {
            when (reader.nextName()) {
                "lapNum" -> lapNumber = reader.nextInt()
                "startSessionTime" -> start = readNumber(reader).toDouble()
                "endSessionTime" -> end = readNumber(reader).toDouble()
                "progress" -> progress = readColumns(reader)
                "positions" -> positions = readColumns(reader)
                "families" -> {
                    reader.beginObject()
                    while (reader.hasNext()) {
                        val name = reader.nextName()
                        var times = FloatArray(0)
                        var fields = emptyMap<String, FloatArray>()
                        reader.beginObject()
                        while (reader.hasNext()) {
                            when (reader.nextName()) {
                                "t" -> times = readArray(reader)
                                "fields" -> fields = readColumns(reader)
                                else -> reader.skipValue()
                            }
                        }
                        reader.endObject()
                        families[name] = AnalysisFamilyData(times, fields)
                    }
                    reader.endObject()
                }
                else -> reader.skipValue()
            }
        }
        reader.endObject()
        fun column(map: Map<String, FloatArray>, key: String, size: Int) =
            map[key]?.takeIf { it.size == size } ?: FloatArray(size) { Float.NaN }
        val progressSize = progress["t"]?.size ?: 0
        val positionSize = positions["t"]?.size ?: 0
        return AnalysisLap(
            lapNumber = lapNumber,
            startSessionTime = start,
            endSessionTime = end,
            progressTime = column(progress, "t", progressSize),
            progressDistance = column(progress, "distance", progressSize),
            progressElapsedMs = column(progress, "elapsedMs", progressSize),
            progressSector = column(progress, "sector", progressSize),
            progressS1Ms = column(progress, "s1Ms", progressSize),
            progressS2Ms = column(progress, "s2Ms", progressSize),
            positionTime = column(positions, "t", positionSize),
            positionX = column(positions, "x", positionSize),
            positionZ = column(positions, "z", positionSize),
            families = families,
            channels = channels,
        )
    }

    private fun readColumns(reader: JsonReader): Map<String, FloatArray> {
        val columns = HashMap<String, FloatArray>()
        reader.beginObject()
        while (reader.hasNext()) {
            val name = reader.nextName()
            columns[name] = readArray(reader)
        }
        reader.endObject()
        return columns
    }

    private fun readArray(reader: JsonReader): FloatArray {
        var values = FloatArray(1024)
        var count = 0
        reader.beginArray()
        while (reader.hasNext()) {
            if (count == values.size) values = values.copyOf(values.size * 2)
            values[count++] = readNumber(reader)
        }
        reader.endArray()
        return values.copyOf(count)
    }

    private fun readNumber(reader: JsonReader): Float =
        if (reader.peek() == JsonToken.NULL) {
            reader.nextNull()
            Float.NaN
        } else {
            reader.nextDouble().toFloat()
        }
}

/** Lap time as m:ss.mmm, or null when not set. */
internal fun formatLapTimeMs(milliseconds: Int): String? {
    if (milliseconds <= 0) return null
    val minutes = milliseconds / 60_000
    val seconds = (milliseconds % 60_000) / 1000.0
    return String.format(java.util.Locale.US, "%d:%06.3f", minutes, seconds)
}

internal fun formatDelta(value: Double?): String {
    if (value == null || !value.isFinite()) return "—.---"
    val normalized = if (abs(value) < 0.0005) 0.0 else value
    return String.format(java.util.Locale.US, "%s%.3f", if (normalized > 0) "+" else "", normalized)
}
