package com.tracknrace.android.pages

import android.content.Context
import com.tracknrace.android.MapPositions
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.RadioButton
import androidx.compose.material3.TextButton
import androidx.compose.ui.semantics.Role
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.setProgress
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.structuralEqualityPolicy
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
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
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
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.Density
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
import kotlin.math.sin

private val PlaybackSpeeds = listOf(0.5f, 1f, 2f, 4f)

// The map's palette, dark and light variants: each sector a tint of the app's
// accent, light to deep, and one colour for each overtaking aid.
private class MapPalette(
    val sectors: List<Color>,
    val drs: Color,
    val slmDry: Color,
    val slmWet: Color,
    val speedTrap: Color,
    val overtake: Color,
)

private val DarkMapPalette = MapPalette(
    sectors = listOf(Color(0xffE3E8F8), Color(0xffB8C6EA), Color(0xff7286BB)),
    drs = Color(0xff7ADCA7),
    slmDry = Color(0xffF4A3C4),
    slmWet = Color(0xff6FD3E6),
    speedTrap = Color(0xff8881DE),
    overtake = Color(0xffF1D25D),
)

private val LightMapPalette = MapPalette(
    sectors = listOf(Color(0xff7F92C4), Color(0xff4E63A3), Color(0xff2B3A6B)),
    drs = Color(0xff1F8A5A),
    slmDry = Color(0xffB8457A),
    slmWet = Color(0xff0F7D92),
    speedTrap = Color(0xff5B54B4),
    overtake = Color(0xff8A6D00),
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
    /** The whole lap as one loop, for keeping the sector names clear of the track. */
    val centerline: List<Point>,
    val startFinish: TrackTick?,
    val drsZones: List<OffsetZone>,
    val slmDryZones: List<OffsetZone>,
    val slmWetZones: List<OffsetZone>,
    val speedTraps: List<Point>,
    val overtakeDetection: Point?,
    val overtakeActivation: TrackTick?,
) {
    /** The drawn circuit's convex hull: all a turned fit needs to size it. */
    val hull: List<Point> = convexHull(sectors.flatMap { it.second })

    /** Car world coordinates to rotated map units. */
    fun mapPosition(x: Double, z: Double): Point = rotation.apply(transform.apply(x, z))

    /** The overtaking zones in force: DRS, or the SLM zones for full or partial grip. */
    fun aeroZones(slm: Boolean, slmTrackStatus: Int): List<OffsetZone> = when {
        !slm -> drsZones
        slmTrackStatus == 1 -> slmWetZones
        else -> slmDryZones
    }
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

    fun changeSpeed(next: Float) {
        base = read(System.nanoTime())
        startedAt = System.nanoTime()
        speed = next
    }

    fun reset(seconds: Double = 0.0) {
        playing = false
        base = seconds.coerceIn(0.0, max(total, seconds))
        startedAt = System.nanoTime()
    }
}

internal class MapLap(val lap: AnalysisLap, val color: Color, val label: String)

private val TrackWidth = 3.dp
private val ZoneWidth = 1.75.dp
private val ZoneOffset = 8.dp
private val TickHalf = 7.dp
private val StartFinishHalf = 7.dp
private val PointRadius = 4.dp
/** Radius of the gap left in the track where one sector meets the next. */
private val JunctionGap = 4.dp
/** Distance of a sector's name from the centreline, past its zone line. */
private val SectorLabelOffset = 20.dp

/**
 * The circuit with each compared car where it was [elapsed] seconds into its
 * lap: each sector in its own tint and named beside it, start/finish, speed
 * traps, and the overtaking aids of the car's era (DRS zones, or the 2026 SLM
 * zones for the track's grip with the overtake detection and activation
 * points). With [focusSector] (0 for S1) the other sectors fade, matching the
 * sector the graph is zoomed to.
 */
@Composable
internal fun AnalysisMap(
    trackId: Int,
    laps: List<MapLap>,
    elapsed: State<Double>,
    aeroMode: String,
    modifier: Modifier = Modifier,
    focusSector: Int? = null,
    slmTrackStatus: Int = -1,
) {
    val context = LocalContext.current.applicationContext
    val outline by produceState<TrackOutline?>(null, context, trackId) {
        value = null
        if (trackId >= 0) value = withContext(Dispatchers.IO) { loadTrackOutline(context, trackId) }
    }
    val colors = MaterialTheme.colorScheme
    val palette = if (colors.surface.luminance() < 0.5f) DarkMapPalette else LightMapPalette
    val trackColor = colors.onSurface
    val markerOutline = colors.surface
    val labelStyle = MaterialTheme.typography.labelMedium.copy(fontWeight = FontWeight.SemiBold)
    val carLabelStyle = MaterialTheme.typography.labelSmall.copy(fontWeight = FontWeight.SemiBold)
    val messageStyle = MaterialTheme.typography.bodyMedium.copy(color = colors.onSurfaceVariant)
    val measurer = rememberTextMeasurer()
    val slm = aeroMode == "slm"

    BoxWithConstraints(modifier.clipToBounds()) {
        val density = LocalDensity.current
        val width = constraints.maxWidth.toFloat()
        val height = constraints.maxHeight.toFloat()
        val lastTurn = remember(outline) { MapTurnMemory() }
        val fit = remember(outline, width, height, density) {
            outline?.let { track ->
                // Room outside the centreline for the offset zones and the labels.
                val pad = with(density) { (SectorLabelOffset + 10.dp).toPx() }
                fitTrack(track, width, height, pad, MapTurnPolicy.Analysis, lastTurn)
            }
        }
        // Marker positions, recomputed as the map clock moves every frame.
        val markers = remember(fit, laps, outline) {
            derivedStateOf(structuralEqualityPolicy()) {
                val track = outline
                val seconds = elapsed.value
                if (fit == null || track == null) return@derivedStateOf emptyList()
                laps.map { entry ->
                    val (x, z) = entry.lap.positionAt(seconds.coerceIn(0.0, entry.lap.duration))
                        ?: return@map null
                    fit.toScreen(track.mapPosition(x, z))
                }
            }
        }
        Spacer(
            Modifier
                .fillMaxSize()
                // The cars move every frame; keep that redraw in its own layer.
                .graphicsLayer()
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
                    val originX = floor(fit.left - pad).coerceAtLeast(0f)
                    val originY = floor(fit.top - pad).coerceAtLeast(0f)
                    val boxWidth = (ceil(fit.right + pad) - originX).coerceIn(1f, size.width - originX)
                    val boxHeight = (ceil(fit.bottom + pad) - originY).coerceIn(1f, size.height - originY)
                    val circuit = ImageBitmap(boxWidth.toInt().coerceAtLeast(1), boxHeight.toInt().coerceAtLeast(1))
                    val origin = Offset(originX, originY)
                    CanvasDrawScope().draw(this, layoutDirection, androidx.compose.ui.graphics.Canvas(circuit), Size(boxWidth, boxHeight)) {
                        drawCircuit(track, fit, origin, palette, trackColor, slm, slmTrackStatus, focusSector)
                    }
                    val circuitTexture = circuit.asAndroidBitmap().copy(android.graphics.Bitmap.Config.HARDWARE, false)
                        ?.asImageBitmap() ?: circuit
                    val sectorNames = measureSectorLabels(track, palette, trackColor, labelStyle, focusSector) { text, style -> measurer.measure(text, style) }
                    val sectorPlaces = sectorLabelPlaces(track, fit, sectorNames, track.aeroZones(slm, slmTrackStatus), size.width, size.height)

                    // Each car's name, small, in its lap colour on a dark tag.
                    val labels = laps.map { measurer.measure(it.label, carLabelStyle.copy(color = it.color)) }
                    val dotRadius = 5.dp.toPx()
                    val dotOutline = 1.5.dp.toPx()
                    val halo = 10.dp.toPx()
                    val gap = 2.dp.toPx()
                    val padX = 4.dp.toPx()
                    val padY = 1.dp.toPx()
                    val corner = CornerRadius(3.dp.toPx())
                    val tag = markerOutline.copy(alpha = 0.85f)

                    onDrawBehind {
                        drawImage(circuitTexture, topLeft = origin)
                        drawSectorLabels(sectorNames, sectorPlaces)
                        val positions = markers.value
                        // The comparison car first, so the current car sits on top.
                        for (index in positions.indices.reversed()) {
                            val position = positions[index] ?: continue
                            val color = laps[index].color
                            drawCircle(color.copy(alpha = 0.22f), halo, position)
                            drawCircle(markerOutline, dotRadius + dotOutline, position)
                            drawCircle(color, dotRadius, position)
                        }
                        // Names over the current car and under the comparison
                        // car, so two cars side by side stay readable.
                        for (index in positions.indices) {
                            val position = positions[index] ?: continue
                            val text = labels[index]
                            val tagWidth = text.size.width + padX * 2
                            val tagHeight = text.size.height + padY * 2
                            val left = (position.x - tagWidth / 2f).coerceIn(0f, max(0f, size.width - tagWidth))
                            val top = if (index == 0) {
                                position.y - halo - gap - tagHeight
                            } else {
                                position.y + halo + gap
                            }.coerceIn(0f, max(0f, size.height - tagHeight))
                            drawRoundRect(tag, Offset(left, top), Size(tagWidth, tagHeight), corner)
                            drawText(text, topLeft = Offset(left + padX, top + padY))
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
    slmTrackStatus: Int,
    focusSector: Int? = null,
) {
    fun screen(point: Point) = fit.toScreen(point) - origin
    val trackWidth = TrackWidth.toPx()
    val trackStroke = Stroke(trackWidth, cap = StrokeCap.Round, join = StrokeJoin.Round)
    // The map numbers its sectors from 1; each is drawn in its own tint.
    for ((index, points) in track.sectors) {
        val color = palette.sectors.getOrElse(index - 1) { trackColor }
        val faded = focusSector != null && index != focusSector + 1
        drawPath(polyline(points.map(::screen)), if (faded) color.copy(alpha = 0.25f) else color, style = trackStroke)
    }
    // A small gap where one sector meets the next, and where the lap starts.
    val gap = max(JunctionGap.toPx(), trackWidth)
    val seams = track.junctions.map { it.point } + listOfNotNull(track.sectors.first().second.firstOrNull())
    for (seam in seams) drawCircle(Color.Black, gap, screen(seam), blendMode = BlendMode.Clear)

    fun tick(tick: TrackTick, half: Float, color: Color, width: Float) {
        val centre = screen(tick.point)
        val normal = fit.turn(tick.normal)
        drawLine(color, centre - normal * half, centre + normal * half, width, StrokeCap.Round)
    }
    val tickHalf = max(TickHalf.toPx(), trackWidth / 2 + 4.dp.toPx())

    // Overtaking aids: DRS zones, or on the 2026 cars the SLM zones for the
    // track's grip and the overtake detection and activation points.
    val zoneOffset = max(ZoneOffset.toPx(), trackWidth / 2 + 5.dp.toPx())
    val zoneStroke = Stroke(ZoneWidth.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round)
    val zones = track.aeroZones(slm, slmTrackStatus)
    val zoneColor = when {
        !slm -> palette.drs
        slmTrackStatus == 1 -> palette.slmWet
        else -> palette.slmDry
    }
    for (zone in zones) {
        val points = zone.points.mapIndexed { index, point ->
            screen(point) + fit.turn(perpendicular(zone.points, index)) * (zoneOffset * zone.outwardSign)
        }
        drawPath(polyline(points), zoneColor, style = zoneStroke)
    }
    // The points that belong to the zones are small and in the zones' colour:
    // a ring where the gap is measured, a short tick where the aid opens.
    val markStroke = Stroke(1.5.dp.toPx())
    if (slm) {
        track.overtakeDetection?.let { drawCircle(zoneColor, PointRadius.toPx(), screen(it), style = markStroke) }
        track.overtakeActivation?.let { tick(it, tickHalf - 2.dp.toPx(), zoneColor, 1.5.dp.toPx()) }
    }

    for (trap in track.speedTraps) drawCircle(trackColor.copy(alpha = 0.55f), PointRadius.toPx(), screen(trap), style = markStroke)
    track.startFinish?.let {
        tick(it, max(StartFinishHalf.toPx(), trackWidth / 2 + 4.dp.toPx()), trackColor, 2.dp.toPx())
    }
}

/** Each sector's name in its tint, faded like its sector, measured once. */
private fun measureSectorLabels(
    track: TrackOutline,
    palette: MapPalette,
    trackColor: Color,
    style: TextStyle,
    focusSector: Int?,
    measure: (String, TextStyle) -> TextLayoutResult,
): List<TextLayoutResult> = track.sectors.map { (sector, _) ->
    val color = palette.sectors.getOrElse(sector - 1) { trackColor }
    val faded = focusSector != null && sector != focusSector + 1
    measure("S$sector", style.copy(color = if (faded) color.copy(alpha = 0.35f) else color))
}

/**
 * Where each sector's name goes: beside its own sector, on whichever side and
 * at whichever point along it leaves the most room from every part of the
 * track and the zone lines, preferring the sector's middle, on screen and
 * clear of the other names. Null for a name with nowhere to go.
 */
private fun Density.sectorLabelPlaces(
    track: TrackOutline,
    fit: TrackFit,
    names: List<TextLayoutResult>,
    zones: List<OffsetZone>,
    width: Float,
    height: Float,
): List<Offset?> {
    val offset = SectorLabelOffset.toPx()
    val wanted = 10.dp.toPx()
    val edge = 8.dp.toPx()
    val zoneOffset = max(ZoneOffset.toPx(), TrackWidth.toPx() / 2 + 5.dp.toPx())
    // Everything a name must keep clear of, in screen units.
    val obstacles = ArrayList<Point>()
    for (point in track.centerline) obstacles += fit.toScreen(point)
    for (zone in zones) zone.points.forEachIndexed { index, point ->
        obstacles += fit.toScreen(point) + fit.turn(perpendicular(zone.points, index)) * (zoneOffset * zone.outwardSign)
    }
    val placed = ArrayList<androidx.compose.ui.geometry.Rect>()
    val samples = 24
    return track.sectors.mapIndexed { slot, (_, points) ->
        val text = names.getOrNull(slot) ?: return@mapIndexed null
        if (points.size < 3) return@mapIndexed null
        val halfW = text.size.width / 2f
        val halfH = text.size.height / 2f
        var best: Offset? = null
        var bestScore = Float.NEGATIVE_INFINITY
        for (step in 1 until samples) {
            val index = points.size * step / samples
            val at = fit.toScreen(points[index])
            val normal = fit.turn(perpendicular(points, index))
            for (side in intArrayOf(1, -1)) {
                val centre = at + normal * (offset * side)
                if (centre.x - halfW < edge || centre.x + halfW > width - edge || centre.y - halfH < edge || centre.y + halfH > height - edge) continue
                val box = androidx.compose.ui.geometry.Rect(centre.x - halfW, centre.y - halfH, centre.x + halfW, centre.y + halfH)
                if (placed.any { it.overlaps(box.inflate(4.dp.toPx())) }) continue
                var room = Float.POSITIVE_INFINITY
                for (point in obstacles) {
                    val dx = max(abs(point.x - centre.x) - halfW, 0f)
                    val dy = max(abs(point.y - centre.y) - halfH, 0f)
                    room = min(room, hypot(dx, dy))
                }
                val score = min(room, wanted) - abs(step - samples / 2f) * 0.25f
                if (score > bestScore) {
                    bestScore = score
                    best = centre
                }
            }
        }
        best?.also { placed += androidx.compose.ui.geometry.Rect(it.x - halfW, it.y - halfH, it.x + halfW, it.y + halfH) }
    }
}

/** The sector names at the places [sectorLabelPlaces] found. */
private fun DrawScope.drawSectorLabels(names: List<TextLayoutResult>, places: List<Offset?>) {
    names.forEachIndexed { index, text ->
        val centre = places.getOrNull(index) ?: return@forEachIndexed
        drawText(text, topLeft = Offset(centre.x - text.size.width / 2f, centre.y - text.size.height / 2f))
    }
}

private fun polyline(points: List<Point>) = Path().apply {
    points.firstOrNull()?.let { moveTo(it.x, it.y) }
    for (index in 1 until points.size) lineTo(points[index].x, points[index].y)
}

/**
 * Turn, scale and offset that fit the circuit into an area, centred: map units
 * are turned by [degrees] about the map origin, scaled, then moved. [left],
 * [top], [right] and [bottom] are the turned circuit's bounds on screen.
 */
private class TrackFit(
    val degrees: Float,
    val scale: Float,
    val offsetX: Float,
    val offsetY: Float,
    val left: Float,
    val top: Float,
    val right: Float,
    val bottom: Float,
) {
    private val cos = kotlin.math.cos(Math.toRadians(degrees.toDouble())).toFloat()
    private val sin = kotlin.math.sin(Math.toRadians(degrees.toDouble())).toFloat()

    fun toScreen(point: Point) = Offset(
        (cos * point.x - sin * point.y) * scale + offsetX,
        (sin * point.x + cos * point.y) * scale + offsetY,
    )

    /** A direction in map units (a normal) as it points on screen. */
    fun turn(vector: Point) = Offset(cos * vector.x - sin * vector.y, sin * vector.x + cos * vector.y)
}

/**
 * How freely a map may turn away from the desktop's orientation to fill its
 * area. A turn is taken only when it makes the circuit at least [minGain]
 * larger than the orientation it would replace, so near ties keep the
 * familiar shape, and a resize keeps the last turn unless the new best beats
 * it by the same margin.
 */
private enum class MapTurnPolicy(val minGain: Float, val quarterTurnsOnly: Boolean) {
    /** The Map tab: the whole screen is the map, so any angle that fills it more. */
    Live(minGain = 0.04f, quarterTurnsOnly = false),

    /**
     * The Analysis map shares the screen and is read against the desktop's
     * sector table and the graphs: only quarter turns, so straights stay
     * straight across the screen, and only for a clear gain.
     */
    Analysis(minGain = 0.08f, quarterTurnsOnly = true),
}

/** The turn a map last chose, kept across resizes of the same circuit. */
private class MapTurnMemory {
    var degrees: Float? = null
}

private fun fitTrack(
    track: TrackOutline,
    width: Float,
    height: Float,
    pad: Float,
    policy: MapTurnPolicy,
    memory: MapTurnMemory,
): TrackFit {
    val degrees = chooseTurn(track.hull, width - 2 * pad, height - 2 * pad, policy, memory.degrees)
    memory.degrees = degrees
    return fitAt(track.hull, degrees, width, height, pad)
}

private fun fitAt(hull: List<Point>, degrees: Float, width: Float, height: Float, pad: Float): TrackFit {
    val extent = turnedExtent(hull, degrees)
    val boundsWidth = (extent.right - extent.left).coerceAtLeast(1f)
    val boundsHeight = (extent.bottom - extent.top).coerceAtLeast(1f)
    val scale = min((width - 2 * pad) / boundsWidth, (height - 2 * pad) / boundsHeight).coerceAtLeast(1e-3f)
    val offsetX = (width - boundsWidth * scale) / 2f - extent.left * scale
    val offsetY = (height - boundsHeight * scale) / 2f - extent.top * scale
    return TrackFit(
        degrees, scale, offsetX, offsetY,
        left = extent.left * scale + offsetX,
        top = extent.top * scale + offsetY,
        right = extent.right * scale + offsetX,
        bottom = extent.bottom * scale + offsetY,
    )
}

/** The circuit's bounding box, in map units, after turning it by [degrees]. */
private fun turnedExtent(hull: List<Point>, degrees: Float): androidx.compose.ui.geometry.Rect {
    val radians = Math.toRadians(degrees.toDouble())
    val cos = cos(radians).toFloat()
    val sin = sin(radians).toFloat()
    var minX = Float.POSITIVE_INFINITY
    var maxX = Float.NEGATIVE_INFINITY
    var minY = Float.POSITIVE_INFINITY
    var maxY = Float.NEGATIVE_INFINITY
    for (point in hull) {
        val x = cos * point.x - sin * point.y
        val y = sin * point.x + cos * point.y
        minX = min(minX, x)
        maxX = max(maxX, x)
        minY = min(minY, y)
        maxY = max(maxY, y)
    }
    return androidx.compose.ui.geometry.Rect(minX, minY, maxX, maxY)
}

/** How large the circuit is drawn at [degrees] in an area of [width] by [height]: the fit's scale. */
private fun scaleAt(hull: List<Point>, degrees: Float, width: Float, height: Float): Float {
    val extent = turnedExtent(hull, degrees)
    return min(width / extent.width.coerceAtLeast(1f), height / extent.height.coerceAtLeast(1f))
}

/**
 * The turn, in degrees, that draws the circuit largest in an area of [width]
 * by [height], within [policy].
 *
 * A turn and the same turn plus 180° need the same box, so only −90° to 90°
 * are tried: the circuit is never shown upside down. Quarter-turn policies try
 * −90°, 0° and 90°. Free policies sweep every 2°, then narrow the best step
 * down to a tenth of a degree, and snap to a quarter turn within 2° of it if
 * that costs under half a percent, so straights line up with the screen when
 * they nearly do. Among near ties the smaller turn wins.
 *
 * The result then has to beat what it would replace by [MapTurnPolicy.minGain]:
 * the last turn when the area is resized, else the desktop's orientation (0°).
 */
private fun chooseTurn(hull: List<Point>, width: Float, height: Float, policy: MapTurnPolicy, previous: Float?): Float {
    if (hull.size < 3 || width <= 0f || height <= 0f) return 0f
    fun size(degrees: Float) = scaleAt(hull, degrees, width, height)

    var best = 0f
    var bestSize = size(0f)
    fun consider(degrees: Float) {
        val at = size(degrees)
        // A clearly larger fit wins; a near tie goes to the smaller turn.
        if (at > bestSize * 1.005f || (at > bestSize * 0.995f && abs(degrees) < abs(best))) {
            best = degrees
            bestSize = at
        }
    }
    if (policy.quarterTurnsOnly) {
        consider(90f)
        consider(-90f)
    } else {
        var degrees = -88f
        while (degrees < 90f) {
            consider(degrees)
            degrees += 2f
        }
        // Narrow the best step: the fit's size is smooth between steps.
        var low = best - 2f
        var high = best + 2f
        while (high - low > 0.1f) {
            val a = low + (high - low) / 3f
            val b = high - (high - low) / 3f
            if (size(a) < size(b)) low = a else high = b
        }
        val refined = (low + high) / 2f
        if (size(refined) > bestSize) {
            best = refined
            bestSize = size(refined)
        }
        // Keep the turn within −90° to 90°.
        if (best >= 90f) best -= 180f
        if (best < -90f) best += 180f
        val quarter = (best / 90f).roundToInt() * 90f
        if (abs(best - quarter) <= 2f && size(quarter) >= bestSize * 0.995f) {
            best = if (quarter >= 90f) -90f else quarter
            bestSize = size(best)
        }
    }

    // Keep the turn already shown, or the desktop's, unless the best clearly beats it.
    val incumbent = previous?.takeIf { !policy.quarterTurnsOnly || it % 90f == 0f } ?: 0f
    val incumbentSize = size(incumbent)
    return when {
        size(0f) * (1 + policy.minGain) >= bestSize -> 0f
        incumbentSize * (1 + policy.minGain) >= bestSize -> incumbent
        else -> best
    }
}

/** The convex hull of [points], counter-clockwise (Andrew's monotone chain). */
private fun convexHull(points: List<Point>): List<Point> {
    val sorted = points.distinct().sortedWith(compareBy({ it.x }, { it.y }))
    if (sorted.size < 3) return sorted
    fun cross(o: Point, a: Point, b: Point) = (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x)
    val hull = ArrayList<Point>(sorted.size * 2)
    for (point in sorted) {
        while (hull.size >= 2 && cross(hull[hull.size - 2], hull[hull.size - 1], point) <= 0f) hull.removeAt(hull.lastIndex)
        hull += point
    }
    val lowerSize = hull.size + 1
    for (index in sorted.indices.reversed()) {
        val point = sorted[index]
        while (hull.size >= lowerSize && cross(hull[hull.size - 2], hull[hull.size - 1], point) <= 0f) hull.removeAt(hull.lastIndex)
        hull += point
    }
    hull.removeAt(hull.lastIndex)
    return hull
}

/** Height of [AnalysisPlaybackBar], and of each of its touch targets. */
private val BarHeight = 36.dp

/**
 * Play, scrub and speed for a fixed comparison's own map clock: one slim row
 * with the elapsed time and a scrubber marked at the sector splits.
 */
@Composable
internal fun AnalysisPlaybackBar(
    clock: AnalysisMapClock,
    elapsed: State<Double>,
    total: Double,
    /** Seconds into the lap where each sector ends, ticked on the scrubber. */
    sectorMarks: List<Double>,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val timeStyle = MaterialTheme.typography.labelMedium.copy(fontFeatureSettings = "tnum")
    // Tenths of a second, as the label shows them: the clock moves every
    // frame, but the label needs to recompose only ten times a second.
    val tenths by remember { derivedStateOf(structuralEqualityPolicy()) { (elapsed.value * 10).roundToInt() } }
    // Wide enough for "0:00.0" at this size, so the scrubber never shifts.
    val timeWidth = with(LocalDensity.current) { (timeStyle.fontSize.toPx() * 3.4f).toDp() }
    var speedOpen by remember { mutableStateOf(false) }
    // One slim line under the map: play, time, scrubber and speed, no chrome.
    Row(
        modifier.fillMaxWidth().height(BarHeight).padding(horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(BarHeight)
                .clip(CircleShape)
                .clickable(onClickLabel = if (clock.playing) "Pause" else "Play", onClick = clock::toggle),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                painterResource(if (clock.playing) R.drawable.ic_pause else R.drawable.ic_play),
                contentDescription = if (clock.playing) "Pause" else "Play",
                modifier = Modifier.size(20.dp),
                tint = colors.primary,
            )
        }
        // Drawn, not composed: it changes ten times a second while playing.
        Readout(
            runs = { listOf(ReadoutRun(formatElapsed(tenths / 10.0, 0.1), colors.onSurfaceVariant, bold = false)) },
            style = timeStyle,
            modifier = Modifier.padding(start = 2.dp).width(timeWidth),
        )
        Scrubber(clock, elapsed, total, sectorMarks, Modifier.weight(1f))
        Box(
            Modifier
                .height(BarHeight)
                .widthIn(min = BarHeight)
                .clip(MaterialTheme.shapes.small)
                .clickable(onClickLabel = "Change speed") { speedOpen = true },
            contentAlignment = Alignment.Center,
        ) {
            Text(speedLabel(clock.speed), style = timeStyle, color = colors.primary, maxLines = 1)
        }
    }
    if (speedOpen) {
        SpeedDialog(
            speed = clock.speed,
            onPick = { next ->
                clock.changeSpeed(next)
                speedOpen = false
            },
            onDismiss = { speedOpen = false },
        )
    }
}

internal fun speedLabel(speed: Float): String =
    "${if (speed < 1f) speed.toString() else speed.toInt().toString()}×"

/** The speeds as a radio list in a dialog, as Android shows a select's options. */
@Composable
internal fun SpeedDialog(
    speed: Float,
    onPick: (Float) -> Unit,
    onDismiss: () -> Unit,
    speeds: List<Float> = PlaybackSpeeds,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Playback speed") },
        text = {
            Column(Modifier.selectableGroup()) {
                for (option in speeds) {
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .height(52.dp)
                            .clip(MaterialTheme.shapes.small)
                            .selectable(selected = option == speed, role = Role.RadioButton) { onPick(option) },
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(selected = option == speed, onClick = null)
                        Text(
                            speedLabel(option),
                            modifier = Modifier.padding(start = 16.dp),
                            style = MaterialTheme.typography.bodyLarge,
                        )
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/**
 * A thin track filled up to the clock, with a round thumb that grows while
 * dragged. Pressing anywhere on it seeks there. Drawn, not composed, so the
 * moving clock only redraws it.
 */
@Composable
private fun Scrubber(
    clock: AnalysisMapClock,
    elapsed: State<Double>,
    total: Double,
    marks: List<Double>,
    modifier: Modifier,
) {
    val colors = MaterialTheme.colorScheme
    var dragging by remember { mutableStateOf(false) }
    val inset = 10.dp
    fun fraction(): Float = if (total > 0) (elapsed.value / total).toFloat().coerceIn(0f, 1f) else 0f
    Spacer(
        modifier
            .height(BarHeight)
            .graphicsLayer()
            .semantics {
                contentDescription = "Lap position"
                progressBarRangeInfo = ProgressBarRangeInfo(fraction(), 0f..1f)
                setProgress { value ->
                    clock.seek(value.toDouble() * total)
                    true
                }
            }
            .pointerInput(total) {
                val edge = inset.toPx()
                fun seekTo(x: Float) {
                    if (total <= 0) return
                    val span = (size.width - 2 * edge).coerceAtLeast(1f)
                    clock.seek(((x - edge) / span).coerceIn(0f, 1f).toDouble() * total)
                }
                awaitEachGesture {
                    val down = awaitFirstDown()
                    dragging = true
                    seekTo(down.position.x)
                    down.consume()
                    drag(down.id) { change ->
                        seekTo(change.position.x)
                        change.consume()
                    }
                    dragging = false
                }
            }
            .drawBehind {
                val edge = inset.toPx()
                val span = size.width - 2 * edge
                val y = size.height / 2
                val track = 3.dp.toPx()
                val corner = CornerRadius(track / 2)
                val at = edge + span * fraction()
                drawRoundRect(colors.surfaceContainerHighest, Offset(edge, y - track / 2), Size(span, track), corner)
                drawRoundRect(colors.primary, Offset(edge, y - track / 2), Size(at - edge, track), corner)
                // Sector boundaries, so a sector can be found by eye.
                if (total > 0) for (mark in marks) {
                    if (mark <= 0 || mark >= total) continue
                    val x = edge + span * (mark / total).toFloat()
                    drawLine(colors.onSurfaceVariant, Offset(x, y - 5.dp.toPx()), Offset(x, y + 5.dp.toPx()), 1.dp.toPx())
                }
                drawCircle(colors.primary, (if (dragging) 8.dp else 5.dp).toPx(), Offset(at, y))
            },
    )
}

/** One car on [LiveTrackMap]: its index, livery colour and label, as on the desktop map. */
internal class LiveMapCar(val index: Int, val color: Color, val label: String)

// A car's positions can arrive anywhere from every frame to a few times a
// second, often in bursts. Each car is drawn a little in the past, between the
// two samples either side of that moment, so it always moves.
private const val MinDelayNanos = 33_000_000L
private const val MaxDelayNanos = 700_000_000L
// The delay is this many typical update intervals, so the next sample has
// usually arrived before the car reaches the newest one.
private const val DelayIntervals = 1.6
private const val Samples = 6
// A dot that would move further than this share of the map teleported (pit
// lane, reset) and snaps instead of sliding across.
private const val SnapFraction = 0.2f

/**
 * A play-out buffer per car for [LiveTrackMap]: the last few positions (in
 * pixels) with the frame they arrived on, and a smoothed update interval. A
 * car is drawn at "now minus its delay", interpolated between the samples
 * around that moment. Nothing is dropped from the stream; this only decides
 * where between two received positions a dot is drawn.
 */
private class CarGlides {
    private val times = LongArray(CARS * Samples)
    private val xs = FloatArray(CARS * Samples)
    private val ys = FloatArray(CARS * Samples)
    private val counts = IntArray(CARS)
    private val intervals = DoubleArray(CARS)

    /** Layout changed: buffered pixels are stale, so start every car afresh. */
    fun reset() {
        counts.fill(0)
    }

    /** Where to draw car [index] at [frame], given its newest position in pixels. */
    fun position(index: Int, targetX: Float, targetY: Float, frame: Long, snap: Float): Offset {
        val base = index * Samples
        val count = counts[index]
        val newest = base + count - 1
        if (count == 0 || targetX != xs[newest] || targetY != ys[newest]) {
            val jump = count > 0 && (abs(targetX - xs[newest]) > snap || abs(targetY - ys[newest]) > snap)
            if (count == 0 || jump) {
                counts[index] = 0
                intervals[index] = 0.0
            } else {
                val interval = (frame - times[newest]).toDouble()
                // The dot is drawn at its delay behind now. If that has passed
                // the newest sample, the dot has been waiting there: start the
                // new segment from that moment, so it does not jump ahead.
                val shownAt = frame - delayOf(index)
                if (shownAt > times[newest]) times[newest] = shownAt
                intervals[index] = if (intervals[index] == 0.0) interval else intervals[index] * 0.8 + interval * 0.2
            }
            push(index, frame, targetX, targetY)
        }
        return at(index, frame)
    }

    private fun delayOf(index: Int): Long =
        (intervals[index] * DelayIntervals).toLong().coerceIn(MinDelayNanos, MaxDelayNanos)

    private fun push(index: Int, frame: Long, x: Float, y: Float) {
        val base = index * Samples
        if (counts[index] == Samples) {
            System.arraycopy(times, base + 1, times, base, Samples - 1)
            System.arraycopy(xs, base + 1, xs, base, Samples - 1)
            System.arraycopy(ys, base + 1, ys, base, Samples - 1)
            counts[index] = Samples - 1
        }
        val slot = base + counts[index]
        times[slot] = frame
        xs[slot] = x
        ys[slot] = y
        counts[index]++
    }

    private fun at(index: Int, frame: Long): Offset {
        val base = index * Samples
        val count = counts[index]
        val time = frame - delayOf(index)
        if (count == 1 || time <= times[base]) return Offset(xs[base], ys[base])
        for (slot in base + 1 until base + count) {
            if (time <= times[slot]) {
                val span = (times[slot] - times[slot - 1]).toFloat().coerceAtLeast(1f)
                val t = ((time - times[slot - 1]) / span).coerceIn(0f, 1f)
                return Offset(xs[slot - 1] + (xs[slot] - xs[slot - 1]) * t, ys[slot - 1] + (ys[slot] - ys[slot - 1]) * t)
            }
        }
        val newest = base + count - 1
        return Offset(xs[newest], ys[newest])
    }

    companion object {
        const val CARS = 24
    }
}

/**
 * The circuit with every car where it is now, drawn like the desktop map: a
 * dot in each car's livery colour with a label above it. The circuit and the
 * labels are drawn once into textures. Each display frame draws every car a
 * little in the past, between the positions received either side of that
 * moment, so cars move smoothly whatever the update rate. The positions are
 * read while drawing, so nothing recomposes.
 */
@Composable
internal fun LiveTrackMap(
    trackId: Int,
    aeroMode: String,
    /** The newest positions, read while drawing. */
    positions: () -> MapPositions,
    /** The display frame time in nanoseconds; changes every frame while shown. */
    frameTick: State<Long>,
    /** Cars to draw; later ones sit on top. */
    cars: List<LiveMapCar>,
    trackColor: Color,
    modifier: Modifier = Modifier,
    slmTrackStatus: Int = -1,
) {
    val context = LocalContext.current.applicationContext
    val outline by produceState<TrackOutline?>(null, context, trackId) {
        value = null
        if (trackId >= 0) value = withContext(Dispatchers.IO) { loadTrackOutline(context, trackId) }
    }
    val measurer = rememberTextMeasurer()
    val labelStyle = androidx.compose.ui.text.TextStyle(
        fontSize = 8.5.sp,
        fontWeight = FontWeight.Bold,
        fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace,
        color = MaterialTheme.colorScheme.inverseOnSurface,
    )
    // The car labels' backing: M3's plain-tooltip surface.
    val pill = MaterialTheme.colorScheme.inverseSurface.copy(alpha = 0.92f)
    val messageStyle = androidx.compose.ui.text.TextStyle(fontSize = 13.sp, color = trackColor.copy(alpha = 0.6f))
    val slm = aeroMode == "slm"
    val palette = if (MaterialTheme.colorScheme.surface.luminance() < 0.5f) DarkMapPalette else LightMapPalette
    val sectorStyle = MaterialTheme.typography.labelMedium.copy(fontWeight = FontWeight.SemiBold)
    val glides = remember { CarGlides() }
    BoxWithConstraints(modifier.clipToBounds()) {
        val density = LocalDensity.current
        val width = constraints.maxWidth.toFloat()
        val height = constraints.maxHeight.toFloat()
        val lastTurn = remember(outline) { MapTurnMemory() }
        val fit = remember(outline, width, height, density) {
            outline?.let { track ->
                val pad = with(density) { (SectorLabelOffset + 8.dp).toPx() }
                fitTrack(track, width, height, pad, MapTurnPolicy.Live, lastTurn)
            }
        }
        Spacer(
            Modifier
                .fillMaxSize()
                .graphicsLayer()
                .semantics { contentDescription = "Live map of every car" }
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
                    val pad = (ZoneOffset + ZoneWidth + 6.dp).toPx()
                    val originX = floor(fit.left - pad).coerceAtLeast(0f)
                    val originY = floor(fit.top - pad).coerceAtLeast(0f)
                    val boxWidth = (ceil(fit.right + pad) - originX).coerceIn(1f, size.width - originX)
                    val boxHeight = (ceil(fit.bottom + pad) - originY).coerceIn(1f, size.height - originY)
                    val circuit = ImageBitmap(boxWidth.toInt().coerceAtLeast(1), boxHeight.toInt().coerceAtLeast(1))
                    val origin = Offset(originX, originY)
                    CanvasDrawScope().draw(this, layoutDirection, androidx.compose.ui.graphics.Canvas(circuit), Size(boxWidth, boxHeight)) {
                        drawCircuit(track, fit, origin, palette, trackColor, slm, slmTrackStatus)
                    }
                    val circuitTexture = circuit.asAndroidBitmap().copy(android.graphics.Bitmap.Config.HARDWARE, false)
                        ?.asImageBitmap() ?: circuit
                    val sectorNames = measureSectorLabels(track, palette, trackColor, sectorStyle, null) { text, style -> measurer.measure(text, style) }
                    val sectorPlaces = sectorLabelPlaces(track, fit, sectorNames, track.aeroZones(slm, slmTrackStatus), size.width, size.height)

                    // The desktop label: a pill with the livery colour down its
                    // left edge and the three-letter code, drawn once.
                    val labelHeight = 13.dp.toPx()
                    val accent = 3.dp.toPx()
                    val padX = 5.dp.toPx()
                    val minWidth = 30.dp.toPx()
                    val corner = CornerRadius(3.dp.toPx())
                    val sprites = cars.map { car ->
                        val text = measurer.measure(car.label, labelStyle)
                        val spriteWidth = max(minWidth, text.size.width + accent + padX * 2)
                        val sprite = ImageBitmap(ceil(spriteWidth).toInt(), ceil(labelHeight).toInt())
                        CanvasDrawScope().draw(this, layoutDirection, androidx.compose.ui.graphics.Canvas(sprite), Size(spriteWidth, labelHeight)) {
                            drawRoundRect(pill, size = size, cornerRadius = corner)
                            drawRoundRect(car.color, size = Size(accent * 2, size.height), cornerRadius = corner)
                            drawRect(pill, topLeft = Offset(accent, 0f), size = Size(accent, size.height))
                            drawText(text, topLeft = Offset(accent + (size.width - accent - text.size.width) / 2f, (size.height - text.size.height) / 2f))
                        }
                        sprite.asAndroidBitmap().copy(android.graphics.Bitmap.Config.HARDWARE, false)?.asImageBitmap() ?: sprite
                    }
                    val dotRadius = 4.5.dp.toPx()
                    val labelGap = 3.dp.toPx()
                    val snap = SnapFraction * max(size.width, size.height)
                    glides.reset()
                    onDrawBehind {
                        val frame = frameTick.value
                        drawImage(circuitTexture, topLeft = origin)
                        drawSectorLabels(sectorNames, sectorPlaces)
                        val now = positions()
                        cars.forEachIndexed { slot, car ->
                            val index = car.index
                            if (index !in 0 until CarGlides.CARS || !now.hasPosition(index)) return@forEachIndexed
                            val target = fit.toScreen(track.mapPosition(now.xAt(index), now.zAt(index)))
                            val point = glides.position(index, target.x, target.y, frame, snap)
                            val x = point.x
                            val y = point.y
                            drawCircle(car.color, dotRadius, point)
                            val sprite = sprites[slot]
                            // Exactly over the dot, so label and dot glide together.
                            drawImage(
                                sprite,
                                topLeft = Offset(x - sprite.width / 2f, y - dotRadius - labelGap - sprite.height),
                            )
                        }
                    }
                },
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
        centerline = rotatedCenterline,
        startFinish = startFinish,
        drsZones = zones("drs_zones"),
        slmDryZones = zones("slm_dry"),
        slmWetZones = zones("slm_wet"),
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
