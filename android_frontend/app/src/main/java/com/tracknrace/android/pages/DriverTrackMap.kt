package com.tracknrace.android.pages

import android.content.Context
import androidx.compose.foundation.Canvas
import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.tracknrace.android.MapPositions
import com.tracknrace.android.TimingDriver
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import kotlin.math.PI
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

private val MapUnderlay = Color(0xff10151b)
private val SectorColors = listOf(
    Color(0xffe8002d),
    Color(0xff0090d0),
    Color(0xffffd700),
)
private val OtherDriverFallback = Color(0xffa8b3be)
private const val FollowZoom = 8f
private const val FitPadding = 0.9f
private const val SegmentsPerPathChunk = 96

private data class MapTransform(
    val minX: Double,
    val minZ: Double,
    val scale: Double,
    val offsetX: Double,
    val offsetZ: Double,
)

private data class MapSector(
    val index: Int,
    val points: List<Offset>,
)

private data class TrackMapGeometry(
    val transform: MapTransform,
    val sectors: List<MapSector>,
    val centerline: List<Offset>,
) {
    fun mapPosition(x: Double, z: Double): Offset = Offset(
        ((x - transform.minX) * transform.scale + transform.offsetX).toFloat(),
        ((z - transform.minZ) * transform.scale + transform.offsetZ).toFloat(),
    )
}

private data class PreparedTrackMap(
    val geometry: TrackMapGeometry,
    val pathChunks: List<MapPathChunk>,
    val heading: TrackHeadingTracker,
    val boundsWidth: Float,
    val boundsHeight: Float,
)

private data class MapPathChunk(
    val sectorIndex: Int,
    val path: Path,
    val minX: Float,
    val maxX: Float,
    val minY: Float,
    val maxY: Float,
) {
    fun intersects(center: Offset, radius: Float): Boolean =
        maxX >= center.x - radius && minX <= center.x + radius &&
            maxY >= center.y - radius && minY <= center.y + radius
}

/**
 * Close-follow map for the dashboard. The player remains fixed at the centre,
 * while the circuit and the other cars translate and rotate underneath it.
 */
@Composable
internal fun DriverTrackMap(
    trackId: Int,
    positions: State<MapPositions>,
    drivers: Map<Int, TimingDriver>,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current.applicationContext
    val density = LocalDensity.current
    val geometry by produceState<TrackMapGeometry?>(null, context, trackId) {
        value = null
        if (trackId >= 0) {
            value = withContext(Dispatchers.IO) {
                loadTrackMap(context, trackId)
            }
        }
    }
    val prepared = remember(geometry) { geometry?.prepare() }
    val driverColors = remember(drivers) {
        drivers.mapValues { (_, driver) -> parseColor(driver.teamColor) ?: OtherDriverFallback }
    }
    val playerArrow = remember(density) {
        with(density) {
            Path().apply {
                moveTo(0f, -13.dp.toPx())
                lineTo(8.dp.toPx(), 8.dp.toPx())
                lineTo(2.5.dp.toPx(), 5.dp.toPx())
                lineTo(0f, 10.dp.toPx())
                lineTo(-2.5.dp.toPx(), 5.dp.toPx())
                lineTo(-8.dp.toPx(), 8.dp.toPx())
                close()
            }
        }
    }

    Canvas(
        modifier.clipToBounds().semantics {
            contentDescription = "Rotating track map with all drivers"
        },
    ) {
        val map = prepared ?: return@Canvas
        // Reading positions in the draw phase invalidates only this Canvas.
        // New packets remain discrete; no data is synthesized between them.
        val currentPositions = positions.value
        val playerIndex = currentPositions.playerIndex
        if (!currentPositions.hasPosition(playerIndex)) return@Canvas

        val playerPoint = map.geometry.mapPosition(
            currentPositions.xAt(playerIndex),
            currentPositions.zAt(playerIndex),
        )
        val headingRadians = map.heading.headingAt(playerPoint)
        val fitScale = min(
            size.width * FitPadding / map.boundsWidth,
            size.height * FitPadding / map.boundsHeight,
        )
        val cameraScale = fitScale * FollowZoom
        val rotationDegrees = -90f - headingRadians * 180f / PI.toFloat()

        withTransform({
            translate(size.width / 2f, size.height / 2f)
            rotate(rotationDegrees, Offset.Zero)
            scale(cameraScale, cameraScale, Offset.Zero)
            translate(-playerPoint.x, -playerPoint.y)
        }) {
            // Use an enclosing circle for the rotated viewport. Culling whole
            // cached chunks avoids submitting the rest of the circuit to the
            // renderer while retaining every source point in visible chunks.
            val visibleRadius = hypot(size.width, size.height) / (2f * cameraScale) +
                12.dp.toPx() / cameraScale
            drawCircuit(map, cameraScale, playerPoint, visibleRadius)

            val markerRadius = 4.dp.toPx() / cameraScale
            val markerOutline = 1.25.dp.toPx() / cameraScale
            for (carIndex in 0 until currentPositions.carCount) {
                if (carIndex == playerIndex || !currentPositions.hasPosition(carIndex)) continue
                val point = map.geometry.mapPosition(
                    currentPositions.xAt(carIndex),
                    currentPositions.zAt(carIndex),
                )
                drawCircle(MapUnderlay, markerRadius + markerOutline, point)
                drawCircle(driverColors[carIndex] ?: OtherDriverFallback, markerRadius, point)
            }
        }

        val playerColor = driverColors[playerIndex] ?: OtherDriverFallback
        withTransform({ translate(size.width / 2f, size.height / 2f) }) {
            drawPath(
                playerArrow,
                MapUnderlay,
                style = Stroke(
                    width = 3.dp.toPx(),
                    cap = StrokeCap.Round,
                    join = StrokeJoin.Round,
                ),
            )
            drawPath(playerArrow, playerColor)
        }
    }
}

private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawCircuit(
    map: PreparedTrackMap,
    cameraScale: Float,
    center: Offset,
    visibleRadius: Float,
) {
    val underlay = Stroke(
        width = 10.dp.toPx() / cameraScale,
        cap = StrokeCap.Round,
        join = StrokeJoin.Round,
    )
    val track = Stroke(
        width = 6.dp.toPx() / cameraScale,
        cap = StrokeCap.Round,
        join = StrokeJoin.Round,
    )
    for (chunk in map.pathChunks) {
        if (chunk.intersects(center, visibleRadius)) {
            drawPath(chunk.path, MapUnderlay, style = underlay)
        }
    }
    for (chunk in map.pathChunks) {
        if (chunk.intersects(center, visibleRadius)) {
            drawPath(
                chunk.path,
                SectorColors.getOrElse(chunk.sectorIndex - 1) { Color.White },
                style = track,
            )
        }
    }
}

private fun TrackMapGeometry.prepare(): PreparedTrackMap {
    val pathChunks = buildList {
        for (sector in sectors) {
            var start = 0
            while (start < sector.points.lastIndex) {
                val end = min(start + SegmentsPerPathChunk, sector.points.lastIndex)
                val first = sector.points[start]
                var minX = first.x
                var maxX = first.x
                var minY = first.y
                var maxY = first.y
                val path = Path().apply { moveTo(first.x, first.y) }
                for (index in start + 1..end) {
                    val point = sector.points[index]
                    path.lineTo(point.x, point.y)
                    minX = min(minX, point.x)
                    maxX = max(maxX, point.x)
                    minY = min(minY, point.y)
                    maxY = max(maxY, point.y)
                }
                add(MapPathChunk(sector.index, path, minX, maxX, minY, maxY))
                // Reuse the last vertex so adjacent chunks join without a gap.
                start = end
            }
        }
    }
    val minX = centerline.minOfOrNull { it.x } ?: 0f
    val maxX = centerline.maxOfOrNull { it.x } ?: 1f
    val minY = centerline.minOfOrNull { it.y } ?: 0f
    val maxY = centerline.maxOfOrNull { it.y } ?: 1f
    return PreparedTrackMap(
        geometry = this,
        pathChunks = pathChunks,
        heading = TrackHeadingTracker(centerline, transform.scale.toFloat()),
        boundsWidth = (maxX - minX).coerceAtLeast(1f),
        boundsHeight = (maxY - minY).coerceAtLeast(1f),
    )
}

private class TrackHeadingTracker(
    private val points: List<Offset>,
    mapUnitsPerMetre: Float,
) {
    private val movementDistanceSquared = (mapUnitsPerMetre * 0.25f).let { it * it }
    private val teleportDistanceSquared = (mapUnitsPerMetre * 80f).let { it * it }
    private val relocationDistanceSquared = (mapUnitsPerMetre * 120f).let { it * it }
    private var lastIndex = -1
    private var smoothedHeading = Float.NaN
    private var previousPosition: Offset? = null

    fun headingAt(position: Offset): Float {
        if (points.size < 2) return 0f
        val previous = previousPosition
        previousPosition = position
        var teleported = false
        if (previous != null) {
            val movement = distanceSquared(previous, position)
            if (movement in movementDistanceSquared..teleportDistanceSquared) {
                return smooth(
                    atan2(position.y - previous.y, position.x - previous.x),
                    reset = false,
                    response = 0.34f,
                )
            }
            teleported = movement > teleportDistanceSquared
        }

        var relocated = lastIndex < 0 || teleported
        var nearest = if (relocated) findNearest(position, 0, points.size) else {
            findNearestAround(position, lastIndex, 180)
        }
        val localDistance = distanceSquared(position, points[nearest])
        if (lastIndex >= 0 && localDistance > relocationDistanceSquared) {
            nearest = findNearest(position, 0, points.size)
            relocated = true
        }
        lastIndex = nearest

        val span = min(12, max(1, points.size / 8))
        val before = points[wrap(nearest - span)]
        val after = points[wrap(nearest + span)]
        val raw = atan2(after.y - before.y, after.x - before.x)
        return smooth(raw, relocated, 0.24f)
    }

    private fun smooth(raw: Float, reset: Boolean, response: Float): Float {
        if (!raw.isFinite()) return if (smoothedHeading.isFinite()) smoothedHeading else 0f
        if (reset || !smoothedHeading.isFinite()) {
            smoothedHeading = raw
        } else {
            var delta = raw - smoothedHeading
            val halfTurn = PI.toFloat()
            val fullTurn = halfTurn * 2f
            while (delta > halfTurn) delta -= fullTurn
            while (delta < -halfTurn) delta += fullTurn
            smoothedHeading += delta * response
        }
        return smoothedHeading
    }

    private fun findNearestAround(position: Offset, center: Int, radius: Int): Int {
        var best = center
        var bestDistance = Float.POSITIVE_INFINITY
        for (step in -radius..radius) {
            val index = wrap(center + step)
            val distance = distanceSquared(position, points[index])
            if (distance < bestDistance) {
                bestDistance = distance
                best = index
            }
        }
        return best
    }

    private fun findNearest(position: Offset, start: Int, end: Int): Int {
        var best = start
        var bestDistance = Float.POSITIVE_INFINITY
        for (index in start until end) {
            val distance = distanceSquared(position, points[index])
            if (distance < bestDistance) {
                bestDistance = distance
                best = index
            }
        }
        return best
    }

    private fun wrap(index: Int): Int {
        val remainder = index % points.size
        return if (remainder < 0) remainder + points.size else remainder
    }
}

private fun distanceSquared(a: Offset, b: Offset): Float {
    val dx = a.x - b.x
    val dy = a.y - b.y
    return dx * dx + dy * dy
}

private fun loadTrackMap(context: Context, trackId: Int): TrackMapGeometry? = runCatching {
    val json = context.assets.open("maps/track_$trackId.json").bufferedReader().use { it.readText() }
    val root = JSONObject(json)
    val rawTransform = root.getJSONObject("transform")
    val transform = MapTransform(
        minX = rawTransform.getDouble("min_x"),
        minZ = rawTransform.getDouble("min_z"),
        scale = rawTransform.getDouble("scale"),
        offsetX = rawTransform.getDouble("off_x"),
        offsetZ = rawTransform.getDouble("off_z"),
    )
    val rawSectors = root.getJSONArray("sectors")
    val sectors = ArrayList<MapSector>(rawSectors.length())
    repeat(rawSectors.length()) { sectorIndex ->
        val rawSector = rawSectors.getJSONObject(sectorIndex)
        val rawPoints = rawSector.getJSONArray("points")
        val points = ArrayList<Offset>(rawPoints.length())
        repeat(rawPoints.length()) { pointIndex ->
            val point = rawPoints.getJSONArray(pointIndex)
            points += Offset(point.getDouble(0).toFloat(), point.getDouble(1).toFloat())
        }
        sectors += MapSector(rawSector.getInt("index"), points)
    }
    sectors.sortBy(MapSector::index)

    val centerline = ArrayList<Offset>()
    for (sector in sectors) {
        for (point in sector.points) {
            if (centerline.lastOrNull() != point) centerline += point
        }
    }
    if (centerline.size > 1 && centerline.first() == centerline.last()) {
        centerline.removeAt(centerline.lastIndex)
    }

    TrackMapGeometry(transform, sectors, centerline)
}.getOrNull()

private fun parseColor(value: String): Color? = runCatching {
    Color(android.graphics.Color.parseColor(value))
}.getOrNull()
