package com.tracknrace.android.pages

import android.content.Context
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.CanvasDrawScope
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import com.tracknrace.android.AnalysisLap
import com.tracknrace.android.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.acos
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

private val PlaybackSpeeds = listOf(0.5f, 1f, 2f, 4f)

// The desktop map's palette (TrackMap.tsx), dark and light variants. The
// sector colours are the broadcast S1/S2/S3 colours; the rest mark the aids.
private class MapPalette(
    val sectors: List<Color>,
    val drs: Color,
    val slm: Color,
    val speedTrap: Color,
    val overtake: Color,
    val startFinish: Color,
)

private val DarkMapPalette = MapPalette(
    sectors = listOf(Color(0xffE8002D), Color(0xff0090D0), Color(0xffFFD700)),
    drs = Color(0xff39B54A),
    slm = Color(0xff46E396),
    speedTrap = Color(0xff8881DE),
    overtake = Color(0xff95E7F0),
    startFinish = Color(0xffE8002D),
)

private val LightMapPalette = MapPalette(
    sectors = listOf(Color(0xffB3132B), Color(0xff0D47A1), Color(0xff765900)),
    drs = Color(0xff237A32),
    slm = Color(0xff16804A),
    speedTrap = Color(0xff5B54B4),
    overtake = Color(0xff397F88),
    startFinish = Color(0xffE8002D),
)

private typealias Point = Offset

/** A mark across the track: a point, the track's normal there, and its sector colour. */
private class TrackTick(val point: Point, val normal: Point, val sector: Int = 0)

/** A zone drawn alongside the track, on its outside. */
private class OffsetZone(val points: List<Point>, val outwardSign: Float)

/**
 * A circuit in map units, already rotated as the desktop draws it. Sector
 * polylines come from the map; DRS and SLM zones are rebuilt as slices of the
 * centreline between their start and end points, exactly as the desktop does.
 */
private class TrackOutline(
    private val transform: MapTransform,
    private val rotation: MapRotation,
    val sectors: List<Pair<Int, List<Point>>>,
    val junctions: List<TrackTick>,
    val startFinish: TrackTick?,
    val drsZones: List<OffsetZone>,
    val slmZones: List<OffsetZone>,
    val speedTraps: List<Point>,
    val overtakeDetection: Point?,
    val overtakeActivation: TrackTick?,
) {
    val boundsMinX = sectors.minOf { (_, points) -> points.minOf { it.x } }
    val boundsMaxX = sectors.maxOf { (_, points) -> points.maxOf { it.x } }
    val boundsMinY = sectors.minOf { (_, points) -> points.minOf { it.y } }
    val boundsMaxY = sectors.maxOf { (_, points) -> points.maxOf { it.y } }

    /** Car world coordinates to rotated map units. */
    fun mapPosition(x: Double, z: Double): Point = rotation.apply(transform.apply(x, z))
}

private class MapTransform(
    private val minX: Double,
    private val minZ: Double,
    private val scale: Double,
    private val offsetX: Double,
    private val offsetZ: Double,
) {
    fun apply(x: Double, z: Double) = Offset(
        ((x - minX) * scale + offsetX).toFloat(),
        ((z - minZ) * scale + offsetZ).toFloat(),
    )
}

/** The map's `rotation_deg` about its view box centre, as the desktop draws it. */
private class MapRotation(degrees: Double, private val centerX: Float, private val centerY: Float) {
    private val cos = kotlin.math.cos(Math.toRadians(degrees)).toFloat()
    private val sin = kotlin.math.sin(Math.toRadians(degrees)).toFloat()

    fun apply(point: Point): Point {
        val dx = point.x - centerX
        val dy = point.y - centerY
        return Offset(cos * dx - sin * dy + centerX, sin * dx + cos * dy + centerY)
    }
}

/**
 * The map clock of a fixed comparison, which plays both laps side by side on
 * the phone. Following the desktop, the map shows the desktop's cursor instead.
 */
@Stable
internal class AnalysisMapClock {
    var playing by mutableStateOf(false)
        private set
    var speed by mutableFloatStateOf(1f)
        private set
    private var base = 0.0
    private var startedAt = 0L
    private var total = 0.0

    /** Seconds into the laps at [nowNanos]. */
    fun read(nowNanos: Long): Double {
        if (!playing) return base
        val elapsed = base + (nowNanos - startedAt) / 1e9 * speed
        if (elapsed >= total) {
            base = total
            playing = false
            return total
        }
        return elapsed
    }

    fun setTotal(seconds: Double) {
        total = max(0.0, seconds)
        base = base.coerceIn(0.0, total)
    }

    fun seek(seconds: Double) {
        base = seconds.coerceIn(0.0, total)
        startedAt = System.nanoTime()
    }

    fun toggle() {
        base = read(System.nanoTime())
        if (!playing && base >= total) base = 0.0
        playing = !playing && total > 0
        startedAt = System.nanoTime()
    }

    fun cycleSpeed() {
        base = read(System.nanoTime())
        startedAt = System.nanoTime()
        speed = PlaybackSpeeds[(PlaybackSpeeds.indexOf(speed) + 1) % PlaybackSpeeds.size]
    }

    fun reset(seconds: Double = 0.0) {
        playing = false
        base = seconds.coerceIn(0.0, max(total, seconds))
        startedAt = System.nanoTime()
    }
}

internal class MapLap(val lap: AnalysisLap, val color: Color, val label: String)

private val TrackWidth = 4.dp
private val ZoneWidth = 4.dp
private val ZoneOffset = 9.dp
private val ZoneDash = 3.dp
private val TickHalf = 7.dp
private val StartFinishHalf = 9.dp
private val PointRadius = 5.dp

/**
 * The circuit with each compared car where it was [elapsed] seconds into its
 * lap, drawn like the desktop map: sector boundaries, start/finish, speed
 * traps, and the overtaking aids of the car's era (DRS zones, or the 2026 SLM
 * zones with the overtake detection and activation points).
 */
@Composable
internal fun AnalysisMap(
    trackId: Int,
    laps: List<MapLap>,
    elapsed: State<Double>,
    aeroMode: String,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current.applicationContext
    val outline by produceState<TrackOutline?>(null, context, trackId) {
        value = null
        if (trackId >= 0) value = withContext(Dispatchers.IO) { loadTrackOutline(context, trackId) }
    }
    val colors = MaterialTheme.colorScheme
    val palette = if (colors.surface.luminance() < 0.5f) DarkMapPalette else LightMapPalette
    val trackColor = colors.onSurface
    val pillColor = colors.inverseSurface
    val pillText = colors.inverseOnSurface
    val markerOutline = colors.surface
    val labelStyle = MaterialTheme.typography.labelMedium.copy(fontWeight = FontWeight.SemiBold)
    val messageStyle = MaterialTheme.typography.bodyMedium.copy(color = colors.onSurfaceVariant)
    val measurer = rememberTextMeasurer()
    val slm = aeroMode == "slm"

    BoxWithConstraints(modifier.clipToBounds()) {
        val density = LocalDensity.current
        val width = constraints.maxWidth.toFloat()
        val height = constraints.maxHeight.toFloat()
        val fit = remember(outline, width, height, density) {
            outline?.let { track ->
                // Room outside the centreline for the offset zones and the labels.
                val pad = with(density) { (ZoneOffset + ZoneWidth + 16.dp).toPx() }
                fitTrack(track, width, height, pad)
            }
        }
        // Marker positions as whole pixels: the map clock moves every frame,
        // a marker moves a pixel only every few frames.
        val markers = remember(fit, laps, outline) {
            derivedStateOf {
                val track = outline
                val seconds = elapsed.value
                if (fit == null || track == null) return@derivedStateOf emptyList()
                laps.map { entry ->
                    val (x, z) = entry.lap.positionAt(seconds.coerceIn(0.0, entry.lap.duration))
                        ?: return@map null
                    val point = fit.toScreen(track.mapPosition(x, z))
                    IntOffset(point.x.roundToInt(), point.y.roundToInt())
                }
            }
        }
        Spacer(
            Modifier
                .fillMaxSize()
                .semantics { contentDescription = "Track map of the compared laps" }
                .drawWithCache {
                    val track = outline
                    if (fit == null || track == null) {
                        val text = measurer.measure(
                            if (trackId < 0) "Waiting for the track" else "No map for this circuit",
                            messageStyle,
                        )
                        return@drawWithCache onDrawBehind {
                            drawText(text, topLeft = Offset((size.width - text.size.width) / 2f, (size.height - text.size.height) / 2f))
                        }
                    }
                    // The circuit never moves: draw it once per size into a
                    // bitmap covering its bounding box, then each frame only
                    // blits it and places the cars.
                    val pad = (ZoneOffset + ZoneWidth + 6.dp).toPx()
                    val originX = floor(track.boundsMinX * fit.scale + fit.offsetX - pad).coerceAtLeast(0f)
                    val originY = floor(track.boundsMinY * fit.scale + fit.offsetY - pad).coerceAtLeast(0f)
                    val boxWidth = (ceil(track.boundsMaxX * fit.scale + fit.offsetX + pad) - originX).coerceIn(1f, size.width - originX)
                    val boxHeight = (ceil(track.boundsMaxY * fit.scale + fit.offsetY + pad) - originY).coerceIn(1f, size.height - originY)
                    val circuit = ImageBitmap(boxWidth.toInt().coerceAtLeast(1), boxHeight.toInt().coerceAtLeast(1))
                    val origin = Offset(originX, originY)
                    CanvasDrawScope().draw(this, layoutDirection, androidx.compose.ui.graphics.Canvas(circuit), Size(boxWidth, boxHeight)) {
                        drawCircuit(track, fit, origin, palette, trackColor, slm)
                    }
                    val circuitTexture = circuit.asAndroidBitmap().copy(android.graphics.Bitmap.Config.HARDWARE, false)
                        ?.asImageBitmap() ?: circuit

                    // Label pills, measured once: a lap-colour edge, then the name.
                    val labels = laps.map { measurer.measure(it.label, labelStyle.copy(color = pillText)) }
                    val dotRadius = 6.dp.toPx()
                    val dotOutline = 1.5.dp.toPx()
                    val accent = 3.dp.toPx()
                    val padX = 6.dp.toPx()
                    val padY = 2.dp.toPx()
                    val gap = 4.dp.toPx()
                    val corner = CornerRadius(4.dp.toPx())

                    onDrawBehind {
                        drawImage(circuitTexture, topLeft = origin)
                        val positions = markers.value
                        // The comparison car first, so the current car sits on top.
                        for (index in positions.indices.reversed()) {
                            val position = positions[index] ?: continue
                            val point = Offset(position.x.toFloat(), position.y.toFloat())
                            drawCircle(markerOutline, dotRadius + dotOutline, point)
                            drawCircle(laps[index].color, dotRadius, point)
                        }
                        // Pills centred over the current car and under the
                        // comparison car, so two cars side by side stay readable.
                        for (index in positions.indices) {
                            val position = positions[index] ?: continue
                            val text = labels[index]
                            val pillWidth = accent + padX * 2 + text.size.width
                            val pillHeight = text.size.height + padY * 2
                            val left = (position.x - pillWidth / 2f).coerceIn(0f, max(0f, size.width - pillWidth))
                            val top = if (index == 0) {
                                position.y - dotRadius - gap - pillHeight
                            } else {
                                position.y + dotRadius + gap
                            }.coerceIn(0f, max(0f, size.height - pillHeight))
                            drawRoundRect(pillColor, Offset(left, top), Size(pillWidth, pillHeight), corner)
                            drawRoundRect(laps[index].color, Offset(left, top), Size(accent * 2, pillHeight), corner)
                            drawRect(pillColor, Offset(left + accent, top), Size(accent, pillHeight))
                            drawText(text, topLeft = Offset(left + accent + padX, top + padY))
                        }
                    }
                },
        )
    }
}

private fun DrawScope.drawCircuit(
    track: TrackOutline,
    fit: TrackFit,
    origin: Offset,
    palette: MapPalette,
    trackColor: Color,
    slm: Boolean,
) {
    fun screen(point: Point) = fit.toScreen(point) - origin
    val trackWidth = TrackWidth.toPx()
    val trackStroke = Stroke(trackWidth, cap = StrokeCap.Round, join = StrokeJoin.Round)
    for ((_, points) in track.sectors) drawPath(polyline(points.map(::screen)), trackColor, style = trackStroke)

    fun tick(tick: TrackTick, half: Float, color: Color, width: Float) {
        val centre = screen(tick.point)
        drawLine(color, centre - tick.normal * half, centre + tick.normal * half, width, StrokeCap.Round)
    }
    // Where S1 ends a tick in the S2 colour, and where S2 ends one in the S3 colour.
    val tickHalf = max(TickHalf.toPx(), trackWidth / 2 + 4.dp.toPx())
    for (junction in track.junctions) {
        tick(junction, tickHalf, palette.sectors.getOrElse(junction.sector) { trackColor }, 3.dp.toPx())
    }

    // Overtaking aids: DRS zones, or on the 2026 cars the SLM zones and the
    // overtake detection and activation points that belong to them.
    val zoneOffset = max(ZoneOffset.toPx(), trackWidth / 2 + 6.dp.toPx())
    val dash = PathEffect.dashPathEffect(floatArrayOf(ZoneDash.toPx(), ZoneDash.toPx()))
    val zoneStroke = Stroke(ZoneWidth.toPx(), cap = StrokeCap.Butt, join = StrokeJoin.Miter, pathEffect = dash)
    val zones = if (slm) track.slmZones else track.drsZones
    val zoneColor = if (slm) palette.slm else palette.drs
    for (zone in zones) {
        val points = zone.points.mapIndexed { index, point ->
            screen(point) + perpendicular(zone.points, index) * (zoneOffset * zone.outwardSign)
        }
        drawPath(polyline(points), zoneColor, style = zoneStroke)
    }
    if (slm) {
        track.overtakeDetection?.let { drawCircle(palette.overtake, PointRadius.toPx(), screen(it)) }
        track.overtakeActivation?.let { tick(it, tickHalf, palette.overtake, 3.dp.toPx()) }
    }

    for (trap in track.speedTraps) drawCircle(palette.speedTrap, PointRadius.toPx(), screen(trap))
    track.startFinish?.let {
        tick(it, max(StartFinishHalf.toPx(), trackWidth / 2 + 4.dp.toPx()), palette.startFinish, 2.5.dp.toPx())
    }
}

private fun polyline(points: List<Point>) = Path().apply {
    points.firstOrNull()?.let { moveTo(it.x, it.y) }
    for (index in 1 until points.size) lineTo(points[index].x, points[index].y)
}

/** Scale and offset that fit the circuit into an area, centred. */
private class TrackFit(val scale: Float, val offsetX: Float, val offsetY: Float) {
    fun toScreen(point: Point) = Offset(point.x * scale + offsetX, point.y * scale + offsetY)
}

private fun fitTrack(track: TrackOutline, width: Float, height: Float, pad: Float): TrackFit {
    val boundsWidth = (track.boundsMaxX - track.boundsMinX).coerceAtLeast(1f)
    val boundsHeight = (track.boundsMaxY - track.boundsMinY).coerceAtLeast(1f)
    val scale = min((width - 2 * pad) / boundsWidth, (height - 2 * pad) / boundsHeight).coerceAtLeast(1e-3f)
    return TrackFit(
        scale,
        (width - boundsWidth * scale) / 2f - track.boundsMinX * scale,
        (height - boundsHeight * scale) / 2f - track.boundsMinY * scale,
    )
}

/** Play, scrub and speed for a fixed comparison's own map clock. */
@Composable
internal fun AnalysisPlaybackBar(
    clock: AnalysisMapClock,
    elapsed: State<Double>,
    total: Double,
    modifier: Modifier = Modifier,
) {
    Surface(modifier = modifier.fillMaxWidth(), color = MaterialTheme.colorScheme.surfaceContainer) {
        Row(
            Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FilledTonalIconButton(onClick = clock::toggle) {
                Icon(
                    painterResource(if (clock.playing) R.drawable.ic_pause else R.drawable.ic_play),
                    contentDescription = if (clock.playing) "Pause" else "Play",
                )
            }
            IconButton(onClick = { clock.seek(elapsed.value - 5) }) {
                Icon(painterResource(R.drawable.ic_replay), contentDescription = "Back 5 seconds")
            }
            ScrubSlider(clock, elapsed, total, Modifier.weight(1f))
            IconButton(onClick = { clock.seek(elapsed.value + 5) }) {
                Icon(painterResource(R.drawable.ic_forward), contentDescription = "Forward 5 seconds")
            }
            TextButton(onClick = clock::cycleSpeed, modifier = Modifier.widthIn(min = 56.dp)) {
                Text("${if (clock.speed < 1f) clock.speed.toString() else clock.speed.toInt().toString()}×")
            }
        }
    }
}

@Composable
private fun ScrubSlider(clock: AnalysisMapClock, elapsed: State<Double>, total: Double, modifier: Modifier) {
    // Tenths of a second, as the label shows them: the clock moves every
    // frame, but this bar needs to recompose only ten times a second.
    val tenths by remember { derivedStateOf { (elapsed.value * 10).roundToInt() } }
    val seconds = tenths / 10.0
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        Slider(
            value = if (total > 0) (seconds / total).toFloat().coerceIn(0f, 1f) else 0f,
            onValueChange = { clock.seek(it * total) },
            modifier = Modifier.fillMaxWidth().height(32.dp),
        )
        Text(
            "${formatElapsed(seconds, 0.1)} / ${formatElapsed(total, 0.1)}",
            style = MaterialTheme.typography.labelSmall.copy(fontFeatureSettings = "tnum"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

// ── Map file ────────────────────────────────────────────────────────────────

private fun loadTrackOutline(context: Context, trackId: Int): TrackOutline? = runCatching {
    val json = context.assets.open("maps/track_$trackId.json").bufferedReader().use { it.readText() }
    val root = JSONObject(json)
    val rawTransform = root.getJSONObject("transform")
    val transform = MapTransform(
        rawTransform.getDouble("min_x"),
        rawTransform.getDouble("min_z"),
        rawTransform.getDouble("scale"),
        rawTransform.getDouble("off_x"),
        rawTransform.getDouble("off_z"),
    )
    val viewBox = root.optJSONObject("view_box")
    val rotation = MapRotation(
        root.optDouble("rotation_deg", 0.0).takeIf { it.isFinite() } ?: 0.0,
        (viewBox?.optDouble("width", 0.0) ?: 0.0).toFloat() / 2f,
        (viewBox?.optDouble("height", 0.0) ?: 0.0).toFloat() / 2f,
    )
    fun point(array: JSONArray?): Point? =
        if (array == null || array.length() < 2) null else Offset(array.getDouble(0).toFloat(), array.getDouble(1).toFloat())

    val rawSectors = root.getJSONArray("sectors")
    val sectorsRaw = List(rawSectors.length()) { sectorIndex ->
        val sector = rawSectors.getJSONObject(sectorIndex)
        val points = sector.getJSONArray("points")
        sector.getInt("index") to List(points.length()) { point(points.getJSONArray(it))!! }
    }.filter { it.second.isNotEmpty() }.sortedBy { it.first }
    if (sectorsRaw.isEmpty()) return null
    val sectors = sectorsRaw.map { (index, points) -> index to points.map(rotation::apply) }

    // One closed loop through every sector, for zone slices and outside tests.
    val centerline = ArrayList<Point>()
    for ((_, points) in sectorsRaw) for (p in points) if (centerline.lastOrNull() != p) centerline += p
    if (centerline.size > 1 && centerline.first() == centerline.last()) centerline.removeAt(centerline.lastIndex)
    val rotatedCenterline = centerline.map(rotation::apply)

    fun zones(key: String): List<OffsetZone> {
        val array = root.optJSONArray(key) ?: return emptyList()
        return List(array.length()) { array.optJSONObject(it) }.mapNotNull { zone ->
            val start = point(zone?.optJSONArray("start")) ?: return@mapNotNull null
            val end = point(zone.optJSONArray("end")) ?: return@mapNotNull null
            val points = smoothSeam(sliceZone(centerline, start, end)).map(rotation::apply)
            if (points.size < 2) null else OffsetZone(points, outwardSign(points, rotatedCenterline))
        }
    }

    val junctions = sectors.dropLast(1).mapIndexedNotNull { index, (_, points) ->
        if (points.size < 2) null else TrackTick(points.last(), perpendicular(points, points.lastIndex), sector = index + 1)
    }
    val startFinish = point(root.optJSONArray("start_finish"))?.let(rotation::apply)?.let { finish ->
        val first = sectors.first().second
        val nearest = first.indices.minByOrNull { distanceSquared(first[it], finish) } ?: 0
        TrackTick(finish, perpendicular(first, nearest))
    }
    val activation = point(root.optJSONArray("overtake_activation_point"))?.let(rotation::apply)?.let { at ->
        val nearest = rotatedCenterline.indices.minByOrNull { distanceSquared(rotatedCenterline[it], at) } ?: 0
        TrackTick(at, perpendicular(rotatedCenterline, nearest))
    }
    val traps = root.optJSONArray("speed_traps")
    TrackOutline(
        transform = transform,
        rotation = rotation,
        sectors = sectors,
        junctions = junctions,
        startFinish = startFinish,
        drsZones = zones("drs_zones"),
        slmZones = zones("slm_dry"),
        speedTraps = if (traps == null) emptyList() else List(traps.length()) { point(traps.optJSONArray(it)) }
            .mapNotNull { it?.let(rotation::apply) },
        overtakeDetection = point(root.optJSONArray("overtake_detection_point"))?.let(rotation::apply),
        overtakeActivation = activation,
    )
}.getOrNull()

// ── Geometry, ported from the desktop map ───────────────────────────────────

private fun distanceSquared(a: Point, b: Point): Float {
    val dx = a.x - b.x
    val dy = a.y - b.y
    return dx * dx + dy * dy
}

/** Unit normal of a polyline at [index], from its neighbours. */
private fun perpendicular(points: List<Point>, index: Int): Point {
    val low = max(0, index - 1)
    val high = min(points.size - 1, index + 1)
    val dx = points[high].x - points[low].x
    val dy = points[high].y - points[low].y
    val length = hypot(dx, dy).takeIf { it > 0f } ?: 1f
    return Offset(-dy / length, dx / length)
}

/** Centreline points from nearest([start]) to nearest([end]), forward with wraparound. */
private fun sliceZone(centerline: List<Point>, start: Point, end: Point): List<Point> {
    val count = centerline.size
    if (count == 0) return emptyList()
    val first = centerline.indices.minBy { distanceSquared(centerline[it], start) }
    val last = centerline.indices.minBy { distanceSquared(centerline[it], end) }
    val out = ArrayList<Point>()
    var index = first
    repeat(count) {
        out += centerline[index]
        if (index == last) return out
        index = (index + 1) % count
    }
    return out
}

private fun pointInPolygon(point: Point, polygon: List<Point>): Boolean {
    var inside = false
    var j = polygon.size - 1
    for (i in polygon.indices) {
        val a = polygon[i]
        val b = polygon[j]
        if ((a.y > point.y) != (b.y > point.y) &&
            point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x
        ) inside = !inside
        j = i
    }
    return inside
}

/**
 * Which normal points outside the circuit, voted along the zone so a
 * figure-eight's two lobes can differ; falls back to the loop's winding.
 */
private fun outwardSign(zone: List<Point>, centerline: List<Point>): Float {
    val step = max(1, zone.size / 128)
    var positive = 0
    var negative = 0
    for (index in zone.indices step step) {
        val normal = perpendicular(zone, index)
        val point = zone[index]
        val plusInside = pointInPolygon(point + normal * 2f, centerline)
        val minusInside = pointInPolygon(point - normal * 2f, centerline)
        if (plusInside == minusInside) continue
        if (plusInside) negative++ else positive++
    }
    if (positive != negative) return if (positive > negative) 1f else -1f
    var twiceArea = 0f
    var j = centerline.size - 1
    for (i in centerline.indices) {
        twiceArea += centerline[j].x * centerline[i].y - centerline[i].x * centerline[j].y
        j = i
    }
    return if (twiceArea >= 0) -1f else 1f
}

private const val SEAM_ANGLE_DEG = 6.0
private const val SEAM_SMOOTH_SPAN = 6
private const val SEAM_SMOOTH_PASSES = 20

private fun turnAngle(a: Point, b: Point, c: Point): Double {
    val d1x = (b.x - a.x).toDouble()
    val d1y = (b.y - a.y).toDouble()
    val d2x = (c.x - b.x).toDouble()
    val d2y = (c.y - b.y).toDouble()
    val l1 = hypot(d1x, d1y).takeIf { it > 0 } ?: 1.0
    val l2 = hypot(d2x, d2y).takeIf { it > 0 } ?: 1.0
    val dot = (d1x * d2x + d1y * d2y) / (l1 * l2)
    return acos(dot.coerceIn(-1.0, 1.0)) * 180 / PI
}

/**
 * Relaxes the tangent kink where a zone crosses the start/finish seam, which
 * would otherwise show as a chevron in the offset line. Real corners never
 * turn this sharply between the densely sampled vertices.
 */
private fun smoothSeam(points: List<Point>): List<Point> {
    val count = points.size
    if (count < 5) return points
    val weight = FloatArray(count)
    var any = false
    for (i in 1 until count - 1) {
        if (turnAngle(points[i - 1], points[i], points[i + 1]) <= SEAM_ANGLE_DEG) continue
        for (j in i - SEAM_SMOOTH_SPAN..i + SEAM_SMOOTH_SPAN) {
            if (j <= 0 || j >= count - 1) continue
            val taper = (0.5 * (1 + cos(PI * abs(j - i) / (SEAM_SMOOTH_SPAN + 1)))).toFloat()
            if (taper > weight[j]) {
                weight[j] = taper
                any = true
            }
        }
    }
    if (!any) return points
    var out = points.toMutableList()
    repeat(SEAM_SMOOTH_PASSES) {
        val next = out.toMutableList()
        for (i in 1 until count - 1) {
            val w = weight[i]
            if (w <= 0f) continue
            val mid = Offset((out[i - 1].x + out[i + 1].x) / 2, (out[i - 1].y + out[i + 1].y) / 2)
            next[i] = out[i] + (mid - out[i]) * w
        }
        out = next
    }
    return out
}
