package com.nogoat.tracknrace.pages

import android.content.Context
import com.nogoat.tracknrace.MapPositions
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.RadioButton
import androidx.compose.material3.TextButton
import androidx.compose.ui.semantics.Role
import androidx.compose.foundation.clickable
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.Box
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
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
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.rememberUpdatedState
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
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.compositeOver
import androidx.compose.ui.graphics.drawscope.CanvasDrawScope
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
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
import com.nogoat.tracknrace.AnalysisLap
import com.nogoat.tracknrace.MapApex
import com.nogoat.tracknrace.MapLapPaint
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
    /** Unit normal at each [centerline] point, pointing out of the circuit. */
    val outward: List<Point>,
    val startFinish: TrackTick?,
    val drsZones: List<OffsetZone>,
    val slmDryZones: List<OffsetZone>,
    val slmWetZones: List<OffsetZone>,
    val speedTraps: List<Point>,
    val overtakeDetection: Point?,
    val overtakeActivation: TrackTick?,
    /** The [centerline] point nearest the start/finish line, where the lap begins. */
    private val lapStart: Int = 0,
) {
    /** The drawn circuit's convex hull: all a turned fit needs to size it. */
    val hull: List<Point> = convexHull(sectors.flatMap { it.second })

    /** The centreline from the start/finish line round the lap, as the desktop paints its layers. */
    val lapLine: List<Point> by lazy { centerline.drop(lapStart) + centerline.take(lapStart) }

    /** [outward] in [lapLine]'s order. */
    val lapOutward: List<Point> by lazy { outward.drop(lapStart) + outward.take(lapStart) }

    /** Each [lapLine] point's fraction (0-1) of the lap, by distance along the line. */
    val lapFraction: FloatArray by lazy {
        val line = lapLine
        val out = FloatArray(line.size)
        var arc = 0f
        for (index in 1 until line.size) {
            arc += hypot(line[index].x - line[index - 1].x, line[index].y - line[index - 1].y)
            out[index] = arc
        }
        val total = if (line.size > 1) arc + hypot(line[0].x - line.last().x, line[0].y - line.last().y) else 0f
        if (total > 0f) for (index in out.indices) out[index] /= total
        out
    }

    /** The [lapLine] index at or before [fraction] of the lap. */
    fun lapIndexAt(fraction: Double): Int {
        val fractions = lapFraction
        if (fractions.isEmpty()) return 0
        val target = (((fraction % 1.0) + 1.0) % 1.0).toFloat()
        var low = 0
        var high = fractions.size - 1
        while (low < high) {
            val mid = (low + high + 1) ushr 1
            if (fractions[mid] <= target) low = mid else high = mid - 1
        }
        return low
    }

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

/**
 * The zoom of one or more Analysis maps. Maps given the same control share
 * it, so the two maps of the Both view zoom and move together, and a reset
 * button outside them can read [zoomed] and [reset] them to the whole circuit.
 */
@Stable
internal class MapZoomControl {
    /** The zoom the maps are drawn at. */
    internal var view by mutableStateOf(MapZoom.Whole)

    /** The part of a pinch still under the fingers, on top of [view]. */
    internal val pinch = mutableStateOf(MapZoom.Whole)

    val zoomed: Boolean get() = view.scale > 1f + ZOOM_EPSILON

    fun reset() {
        view = MapZoom.Whole
        pinch.value = MapZoom.Whole
    }
}

/**
 * What the track is painted with instead of its sector tints: one lap's map
 * layer along the lap ([Layer]), two laps' as bands side by side, the first
 * lap inside ([Bands]), or each lap's recorded line with no circuit under
 * it ([RacingLine]), as the desktop draws it. A layer still loading draws
 * the circuit dimmed.
 */
internal sealed interface MapTrackPaint {
    data class Layer(val paint: MapLapPaint?) : MapTrackPaint
    data class Bands(val inside: MapLapPaint?, val outside: MapLapPaint?) : MapTrackPaint
    data object RacingLine : MapTrackPaint
}

/** A braking zone marked on the map: its number, where it starts on the lap, and its colour. */
internal class MapZoneMark(val number: Int, val distanceM: Double, val color: Color)

private val TrackWidth = 3.dp
private val ZoneWidth = 1.75.dp
private val ZoneOffset = 8.dp
private val TickHalf = 7.dp
private val StartFinishHalf = 7.dp
private val PointRadius = 4.dp
/** Radius of the gap left in the track where one sector meets the next. */
private val JunctionGap = 4.dp
/** On the live map, the gap either side of a sector boundary's tick. */
private val BoundaryGap = 1.5.dp
/** The live map's badges for overtake detection, activation and speed traps. */
private val BadgeRadius = 10.dp
private val BadgeReach = 21.dp
private val BadgeIcon = 14.dp
/** The live map's car dots, the ring cut round them, and their labels. */
private val LiveDotRadius = 5.dp
private val LiveDotRing = 2.dp
private val LiveLabelHeight = 15.dp
private val LiveLabelReach = 12.dp
/** The Analysis map's braking zone badges: their disc and their stem. */
private val ZoneBadgeRadius = 9.dp
private val ZoneBadgeReach = 21.dp
/** Distance of a sector's name from the centreline, past its zone line. */
private val SectorLabelOffset = 20.dp
/** The touch target around a braking zone's badge. */
private val ZoneMarkTarget = 36.dp
/** How far a corner speed's chip sits outside the centreline. */
private val ApexReach = 15.dp
/** Each band's width when two laps share the track ([MapTrackPaint.Bands]). */
private val BandWidth = 2.5.dp
/** A lap's recorded line on the Racing Line layer. */
private val RacingLineWidth = 1.75.dp

/**
 * The circuit with each compared car where it was [elapsed] seconds into its
 * lap: each sector in its own tint and named beside it, start/finish, speed
 * traps, and the overtaking aids of the car's era (DRS zones, or the 2026 SLM
 * zones for the track's grip with the overtake detection and activation
 * points). With [focusSector] (0 for S1) the other sectors fade, matching the
 * sector the graph is zoomed to. [zoneMarks] number braking zones where
 * the first lap reached them, each a badge on a stem beside the track,
 * [selectedZone] filled in; tapping one calls [onZone] with its index. Cars
 * are ringed dots with their names on chips beside the track; sector
 * boundaries, overtake points and speed traps are drawn as on the live map.
 * With [onScrub], a car can be dragged along its lap, which reports the
 * seconds into the lap it was dragged to. Pinching zooms the map; with
 * [zoomControl], the zoom is kept there, shared with any other map given
 * the same control, and a reset from there returns it to the whole circuit.
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
    zoneMarks: List<MapZoneMark> = emptyList(),
    selectedZone: Int = -1,
    onZone: (Int) -> Unit = {},
    trackPaint: MapTrackPaint? = null,
    /** Corner speeds labelled beside the track: the Speed layer's apexes. */
    apexes: List<MapApex> = emptyList(),
    onScrub: ((Double) -> Unit)? = null,
    zoomControl: MapZoomControl? = null,
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
    val scrub = rememberUpdatedState(onScrub)

    BoxWithConstraints(modifier.clipToBounds()) {
        val density = LocalDensity.current
        val width = constraints.maxWidth.toFloat()
        val height = constraints.maxHeight.toFloat()
        val lastTurn = remember(outline) { MapTurnMemory() }
        val baseFit = remember(outline, width, height, density) {
            outline?.let { track ->
                // Room outside the centreline for the offset zones and the labels.
                val pad = with(density) { (SectorLabelOffset + 10.dp).toPx() }
                fitTrack(track, width, height, pad, MapTurnPolicy.Analysis, lastTurn)
            }
        }
        // Pinch zoom. [view] is the zoom the map is drawn at, folded into its
        // fit so lines and labels stay their size and crisp; [pinch] is the part
        // still under the fingers, shown by scaling the drawn map until they
        // lift, so a pinch never redraws the circuit. A new track or a new size
        // starts again from the whole circuit. Both live in the zoom control,
        // which the two maps of the Both view share, so they zoom as one.
        val zoom = zoomControl ?: remember { MapZoomControl() }
        LaunchedEffect(zoom, outline, width, height) { zoom.reset() }
        val view = zoom.view
        val pinch = zoom.pinch
        val fit = remember(baseFit, view) { baseFit?.zoomed(view) }
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
        // Overtake detection, activation and speed trap badges, as on the live map.
        val badges = remember(fit, outline, slm, width, height, density) {
            val track = outline
            if (fit == null || track == null) emptyList() else with(density) { mapBadges(track, fit, slm, width, height) }
        }
        // Braking zones: where the first lap reached each one's start, and a
        // numbered badge off the track on a stem, clear of the track, the
        // circuit's badges and the zones already placed.
        val zoneSpots = remember(fit, outline, laps, zoneMarks, badges, density) {
            val track = outline
            val lap = laps.firstOrNull()?.lap
            if (fit == null || track == null || lap == null) return@remember emptyList<Pair<Offset, Offset>?>()
            val line = track.centerline.map(fit::toScreen)
            val outward = track.outward.map(fit::turn)
            val taken = ArrayList<Offset>(badges.map { it.centre })
            with(density) {
                zoneMarks.map { mark ->
                    val time = lap.progress?.timeAtDistance(mark.distanceM)?.takeIf { it.isFinite() } ?: return@map null
                    val (x, z) = lap.positionAt(time) ?: return@map null
                    val anchor = fit.toScreen(track.mapPosition(x, z))
                    val centre = badgeSpot(
                        line, outward, anchor, taken, width, height,
                        radius = ZoneBadgeRadius.toPx(), reach = ZoneBadgeReach.toPx(), angles = ZoneBadgeAngles,
                    )
                    taken += centre
                    anchor to centre
                }
            }
        }
        val boundaryColor = colors.onSurfaceVariant
        val unpaintedColor = colors.onSurface.copy(alpha = 0.22f)
        val apexStyle = MaterialTheme.typography.labelSmall.copy(color = colors.onSurface, fontWeight = FontWeight.SemiBold)
        val labelFill = colors.surfaceContainer
        val labelOutline = colors.outlineVariant
        val leaderColor = colors.onSurfaceVariant.copy(alpha = 0.6f)

        // Each lap's recorded positions on screen, for dragging its car along the lap.
        val carPaths = remember(fit, outline, laps) {
            val track = outline
            if (fit == null || track == null) emptyList() else laps.map { entry -> carPath(entry.lap, track, fit) }
        }
        val scrubbing = onScrub != null
        // Gestures on the map as it stands, outside the pinch layer, so a pinch
        // is read against fingers that are not moving with what they move.
        Box(
            Modifier
                .fillMaxSize()
                .pointerInput(scrubbing, carPaths, baseFit, zoom) {
                    val grab = CarGrab.toPx()
                    val reach = CarDragReach.toPx()
                    val slop = viewConfiguration.touchSlop
                    val tapReach = 48.dp.toPx()
                    var lastTapAt = Long.MIN_VALUE / 2
                    var lastTap = Offset.Zero
                    awaitEachGesture {
                        val down = awaitFirstDown()
                        // A car under the finger is dragged along its lap.
                        if (scrubbing) {
                            val positions = markers.value
                            var car = -1
                            var nearest = grab
                            positions.forEachIndexed { index, position ->
                                if (position == null) return@forEachIndexed
                                val distance = (position - down.position).getDistance()
                                if (distance <= nearest) {
                                    nearest = distance
                                    car = index
                                }
                            }
                            val path = carPaths.getOrNull(car)
                            if (path != null) {
                                down.consume()
                                var seconds = elapsed.value
                                drag(down.id) { change ->
                                    path.timeNear(change.position, seconds, CAR_DRAG_WINDOW_SECONDS, reach)?.let { time ->
                                        seconds = time
                                        scrub.value?.invoke(time)
                                    }
                                    change.consume()
                                }
                                lastTapAt = Long.MIN_VALUE / 2
                                return@awaitEachGesture
                            }
                        }
                        val base = baseFit ?: return@awaitEachGesture
                        // Two fingers pinch and move; one finger moves the map once it is zoomed in.
                        var moved = false
                        var pinched = false
                        var releasedAt = down.uptimeMillis
                        while (true) {
                            val event = awaitPointerEvent()
                            val pressed = event.changes.filter { it.pressed }
                            if (pressed.isEmpty()) {
                                releasedAt = event.changes.maxOf { it.uptimeMillis }
                                break
                            }
                            if (pressed.size >= 2) {
                                pinched = true
                                pinch.value = pinch.value.pinched(
                                    event.calculateZoom(), event.calculateCentroid(useCurrent = true), event.calculatePan(), zoom.view.scale,
                                )
                                event.changes.forEach { it.consume() }
                                continue
                            }
                            val finger = pressed[0]
                            if (!moved && (finger.position - down.position).getDistance() > slop) moved = true
                            if (moved && zoom.view.scale * pinch.value.scale > 1f + ZOOM_EPSILON) {
                                pinch.value = pinch.value.panned(finger.positionChange())
                                finger.consume()
                            }
                        }
                        val mapWidth = size.width.toFloat()
                        val mapHeight = size.height.toFloat()
                        if (pinched || moved) {
                            if (pinch.value != MapZoom.Whole) {
                                zoom.view = zoom.view.then(pinch.value).clamped(base, mapWidth, mapHeight)
                                pinch.value = MapZoom.Whole
                            }
                            lastTapAt = Long.MIN_VALUE / 2
                            return@awaitEachGesture
                        }
                        // A double tap zooms in on the spot, or back out to the whole circuit.
                        val quick = releasedAt - down.uptimeMillis <= MAP_DOUBLE_TAP_MS
                        if (quick && down.uptimeMillis - lastTapAt <= MAP_DOUBLE_TAP_MS && (down.position - lastTap).getDistance() < tapReach) {
                            zoom.view = if (zoom.view.scale > 1f + ZOOM_EPSILON) {
                                MapZoom.Whole
                            } else {
                                MapZoom.Whole.pinched(DOUBLE_TAP_ZOOM, down.position, Offset.Zero, 1f).clamped(base, mapWidth, mapHeight)
                            }
                            lastTapAt = Long.MIN_VALUE / 2
                        } else {
                            lastTapAt = releasedAt
                            lastTap = down.position
                        }
                    }
                },
        ) {
            Box(
                Modifier
                    .fillMaxSize()
                    .graphicsLayer {
                        val live = pinch.value
                        scaleX = live.scale
                        scaleY = live.scale
                        translationX = live.offset.x
                        translationY = live.offset.y
                        transformOrigin = TransformOrigin(0f, 0f)
                    },
            ) {
                Spacer(
                    Modifier
                        .fillMaxSize()
                        // The cars move every frame; keep that redraw in its own layer,
                        // offscreen so the ring round each dot can cut through to the page.
                        .graphicsLayer { compositingStrategy = CompositingStrategy.Offscreen }
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
                            // bitmap covering its bounding box and its badges, then each
                            // frame only blits it and places the cars.
                            val pad = (BadgeReach + BadgeRadius + 4.dp).toPx()
                            val originX = floor(fit.left - pad).coerceAtLeast(0f)
                            val originY = floor(fit.top - pad).coerceAtLeast(0f)
                            val boxWidth = (ceil(fit.right + pad) - originX).coerceIn(1f, size.width - originX)
                            val boxHeight = (ceil(fit.bottom + pad) - originY).coerceIn(1f, size.height - originY)
                            val circuit = ImageBitmap(boxWidth.toInt().coerceAtLeast(1), boxHeight.toInt().coerceAtLeast(1))
                            val origin = Offset(originX, originY)
                            CanvasDrawScope().draw(this, layoutDirection, androidx.compose.ui.graphics.Canvas(circuit), Size(boxWidth, boxHeight)) {
                                drawCircuit(
                                    track, fit, origin, palette, trackColor, slm, slmTrackStatus, focusSector,
                                    boundaryColor = boundaryColor, badges = badges,
                                    trackPaint = trackPaint, laps = laps, unpainted = unpaintedColor,
                                )
                            }
                            val circuitTexture = circuit.asAndroidBitmap().copy(android.graphics.Bitmap.Config.HARDWARE, false)
                                ?.asImageBitmap() ?: circuit
                            val sectorNames = measureSectorLabels(track, palette, trackColor, labelStyle, focusSector) { text, style -> measurer.measure(text, style) }
                            // Corner speeds on small chips just outside the track.
                            val apexLabels = apexes.mapNotNull { apex ->
                                val length = (trackPaint as? MapTrackPaint.Layer)?.paint?.trackLengthM ?: return@mapNotNull null
                                if (length <= 0.0) return@mapNotNull null
                                val index = track.lapIndexAt(apex.distanceM / length)
                                val anchor = fit.toScreen(track.lapLine[index])
                                val centre = anchor + fit.turn(track.lapOutward[index]) * (ApexReach.toPx())
                                centre to measurer.measure(apex.speedKph.roundToInt().toString(), apexStyle)
                            }
                            val sectorPlaces = sectorLabelPlaces(track, fit, sectorNames, track.aeroZones(slm, slmTrackStatus), size.width, size.height)

                            // Each car's name as the lap picker sets it, in its lap
                            // colour, on a tonal chip beside the track.
                            val names = laps.map { measurer.measure(it.label, carLabelStyle.copy(color = it.color)) }
                            val padX = 5.dp.toPx()
                            val widths = FloatArray(laps.size) { max(30.dp.toPx(), names[it].size.width + padX * 2) }
                            val heights = FloatArray(laps.size) { max(LiveLabelHeight.toPx(), names[it].size.height + 2.dp.toPx()) }
                            val corner = CornerRadius(4.dp.toPx())
                            val hairline = 1.dp.toPx()
                            val line = track.centerline.map(fit::toScreen)
                            val snap = SnapFraction * max(size.width, size.height)
                            val layout = LabelLayout(
                                ruler = TrackRuler(line, track.outward.map(fit::turn), smoothing = 18.dp.toPx()),
                                reach = LiveLabelReach.toPx(), gap = 3.dp.toPx(), margin = 2.dp.toPx(),
                                width = size.width, height = size.height, snap = snap,
                                switchAfter = 16.dp.toPx(), swapAfter = 10.dp.toPx(), mergeShorterThan = 60.dp.toPx(),
                                typicalWidth = widths.maxOrNull() ?: 30.dp.toPx(), typicalHeight = heights.maxOrNull() ?: LiveLabelHeight.toPx(),
                                slots = laps.size,
                            )
                            val points = arrayOfNulls<Offset>(laps.size)
                            val nearest = IntArray(laps.size)
                            val seen = BooleanArray(laps.size)
                            val chips = arrayOfNulls<Offset>(laps.size)
                            val lostDistance = 12.dp.toPx()
                            val dotRadius = LiveDotRadius.toPx()
                            val ring = LiveDotRing.toPx()
                            val leaderWidth = 1.dp.toPx()
                            val stemWidth = 1.2.dp.toPx()
                            val anchorRadius = 2.5.dp.toPx()

                            onDrawBehind {
                                drawImage(circuitTexture, topLeft = origin)
                                drawSectorLabels(sectorNames, sectorPlaces)
                                for ((centre, text) in apexLabels) {
                                    val chipSize = Size(text.size.width + padX * 2, text.size.height.toFloat())
                                    val topLeft = Offset(centre.x - chipSize.width / 2f, centre.y - chipSize.height / 2f)
                                    val round = CornerRadius(chipSize.height / 2f)
                                    drawRoundRect(labelFill, topLeft, chipSize, round)
                                    drawRoundRect(labelOutline, topLeft, chipSize, round, style = Stroke(hairline))
                                    drawText(text, topLeft = Offset(centre.x - text.size.width / 2f, centre.y - text.size.height / 2f))
                                }
                                // Each braking zone's stem and the dot where it starts;
                                // the numbered badge itself is drawn on top, tappable.
                                zoneSpots.forEachIndexed { index, spot ->
                                    spot ?: return@forEachIndexed
                                    val color = zoneMarks.getOrNull(index)?.color ?: return@forEachIndexed
                                    drawLine(color, spot.first, spot.second, stemWidth)
                                    drawCircle(color, anchorRadius, spot.first)
                                }
                                val positions = markers.value
                                for (index in laps.indices) {
                                    val position = positions.getOrNull(index)
                                    points[index] = position
                                    if (position == null) {
                                        seen[index] = false
                                        continue
                                    }
                                    nearest[index] = followTrack(line, position, if (seen[index]) nearest[index] else -1, lostDistance)
                                    seen[index] = true
                                }
                                layout.layout(points, nearest, widths, heights, chips)
                                for (index in laps.indices) {
                                    val position = points[index] ?: continue
                                    val chip = chips[index] ?: continue
                                    drawLine(leaderColor, position, chip, leaderWidth)
                                }
                                // The comparison car first, so the current car sits on top.
                                for (index in laps.indices.reversed()) {
                                    val position = points[index] ?: continue
                                    drawCircle(Color.Black, dotRadius + ring, position, blendMode = BlendMode.Clear)
                                    drawCircle(laps[index].color, dotRadius, position)
                                }
                                for (index in laps.indices) {
                                    val chip = chips[index] ?: continue
                                    val topLeft = Offset(chip.x - widths[index] / 2f, chip.y - heights[index] / 2f)
                                    val chipSize = Size(widths[index], heights[index])
                                    drawRoundRect(labelFill, topLeft, chipSize, corner)
                                    drawRoundRect(
                                        labelOutline, topLeft + Offset(hairline / 2, hairline / 2),
                                        Size(chipSize.width - hairline, chipSize.height - hairline), corner, style = Stroke(hairline),
                                    )
                                    val text = names[index]
                                    drawText(text, topLeft = Offset(chip.x - text.size.width / 2f, chip.y - text.size.height / 2f))
                                }
                            }
                        },
                )
                // Braking zone badges: tappable, the selected one filled in.
                val half = with(density) { ZoneMarkTarget.toPx() / 2f }
                zoneSpots.forEachIndexed { index, spot ->
                    spot ?: return@forEachIndexed
                    val centre = spot.second
                    val mark = zoneMarks[index]
                    val on = index == selectedZone
                    Box(
                        Modifier
                            .offset { IntOffset((centre.x - half).roundToInt(), (centre.y - half).roundToInt()) }
                            .size(ZoneMarkTarget)
                            .clip(CircleShape)
                            .clickable(onClickLabel = "Show zone ${mark.number}") { onZone(index) }
                            .semantics { contentDescription = "Braking zone ${mark.number}" },
                        contentAlignment = Alignment.Center,
                    ) {
                        Box(
                            Modifier
                                .size(ZoneBadgeRadius * 2)
                                .background(if (on) mark.color else mark.color.copy(alpha = 0.25f).compositeOver(markerOutline), CircleShape)
                                .border(1.2.dp, mark.color, CircleShape),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                "${mark.number}",
                                style = carLabelStyle.copy(fontSize = 10.sp, fontWeight = FontWeight.Bold),
                                color = if (on) markerOutline else colors.onSurface,
                                maxLines = 1,
                            )
                        }
                    }
                }
            }
        }
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
    /**
     * The live map's style: each sector boundary is an even cut across the
     * track with a tick in this colour, and [badges] stand in for the small
     * detection, activation and speed trap marks.
     */
    boundaryColor: Color? = null,
    badges: List<MapBadge> = emptyList(),
    trackPaint: MapTrackPaint? = null,
    laps: List<MapLap> = emptyList(),
    unpainted: Color = trackColor.copy(alpha = 0.22f),
) {
    fun screen(point: Point) = fit.toScreen(point) - origin
    val trackWidth = TrackWidth.toPx()
    val trackStroke = Stroke(trackWidth, cap = StrokeCap.Round, join = StrokeJoin.Round)
    when (trackPaint) {
        null -> {
            // The map numbers its sectors from 1; each is drawn in its own tint.
            for ((index, points) in track.sectors) {
                val color = palette.sectors.getOrElse(index - 1) { trackColor }
                val faded = focusSector != null && index != focusSector + 1
                drawPath(polyline(points.map(::screen)), if (faded) color.copy(alpha = 0.25f) else color, style = trackStroke)
            }
        }
        is MapTrackPaint.Layer -> drawPaintedLap(track, fit, ::screen, trackPaint.paint, trackWidth, 0f, unpainted)
        is MapTrackPaint.Bands -> {
            // Two bands either side of the centreline: the first lap inside.
            val band = BandWidth.toPx()
            val reach = band / 2f + 0.5.dp.toPx()
            drawPaintedLap(track, fit, ::screen, trackPaint.inside, band, -reach, unpainted)
            drawPaintedLap(track, fit, ::screen, trackPaint.outside, band, reach, unpainted)
        }
        MapTrackPaint.RacingLine -> {
            // Each lap's recorded line as the desktop draws it, the current lap
            // first, with no circuit under it.
            val lineStroke = Stroke(RacingLineWidth.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round)
            for (entry in laps) {
                val lap = entry.lap
                if (lap.positionX.size < 2) continue
                val points = ArrayList<Point>(lap.positionX.size)
                for (index in lap.positionX.indices) {
                    val x = lap.positionX[index]
                    val z = lap.positionZ[index]
                    if (!x.isFinite() || !z.isFinite()) continue
                    points += screen(track.mapPosition(x.toDouble(), z.toDouble()))
                }
                if (points.size > 1) drawPath(polyline(points), entry.color, style = lineStroke)
            }
        }
    }
    fun tick(tick: TrackTick, half: Float, color: Color, width: Float) {
        val centre = screen(tick.point)
        val normal = fit.turn(tick.normal)
        drawLine(color, centre - normal * half, centre + normal * half, width, StrokeCap.Round)
    }
    val tickHalf = max(TickHalf.toPx(), trackWidth / 2 + 4.dp.toPx())
    val startFinishHalf = max(StartFinishHalf.toPx(), trackWidth / 2 + 4.dp.toPx())

    if (boundaryColor != null) {
        // A band cut across the track, the same on both sides of its tick and
        // centred on the boundary, so every join shows the same small gap.
        val cutHalf = trackWidth / 2 + 1.dp.toPx()
        fun boundary(at: TrackTick, half: Float, color: Color, width: Float) {
            val centre = screen(at.point)
            val normal = fit.turn(at.normal)
            drawLine(
                Color.Black, centre - normal * cutHalf, centre + normal * cutHalf,
                width + BoundaryGap.toPx() * 2, StrokeCap.Butt, blendMode = BlendMode.Clear,
            )
            drawLine(color, centre - normal * half, centre + normal * half, width, StrokeCap.Round)
        }
        for (junction in track.junctions) boundary(junction, tickHalf, boundaryColor, 1.75.dp.toPx())
        track.startFinish?.let { boundary(it, startFinishHalf, trackColor, 2.dp.toPx()) }
    } else {
        // A small gap where one sector meets the next, and where the lap starts.
        val gap = max(JunctionGap.toPx(), trackWidth)
        val seams = track.junctions.map { it.point } + listOfNotNull(track.sectors.first().second.firstOrNull())
        for (seam in seams) drawCircle(Color.Black, gap, screen(seam), blendMode = BlendMode.Clear)
    }

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
    if (boundaryColor != null) {
        for (badge in badges) drawBadge(badge, origin, zoneColor, trackColor)
        return
    }
    if (slm) {
        track.overtakeDetection?.let { drawCircle(zoneColor, PointRadius.toPx(), screen(it), style = markStroke) }
        track.overtakeActivation?.let { tick(it, tickHalf - 2.dp.toPx(), zoneColor, 1.5.dp.toPx()) }
    }

    for (trap in track.speedTraps) drawCircle(trackColor.copy(alpha = 0.55f), PointRadius.toPx(), screen(trap), style = markStroke)
    track.startFinish?.let { tick(it, startFinishHalf, trackColor, 2.dp.toPx()) }
}

/**
 * The lap line from start/finish, each stretch in [paint]'s colour at its
 * middle, [offset] pixels outward of the centreline (negative is inside).
 * Neighbouring stretches of one colour share a stroke so their joins stay
 * clean; unpainted stretches, or all of a layer still loading, are [unpainted].
 */
private fun DrawScope.drawPaintedLap(
    track: TrackOutline,
    fit: TrackFit,
    screen: (Point) -> Offset,
    paint: MapLapPaint?,
    width: Float,
    offset: Float,
    unpainted: Color,
) {
    val line = track.lapLine
    val outward = track.lapOutward
    val fractions = track.lapFraction
    val count = line.size
    if (count < 2) return
    val stroke = Stroke(width, cap = StrokeCap.Round, join = StrokeJoin.Round)
    fun at(index: Int) = screen(line[index]) + fit.turn(outward[index]) * offset
    var runColor: Color? = null
    var path = Path()
    for (index in 0 until count) {
        val next = (index + 1) % count
        val middle = (fractions[index] + (if (next == 0) 1f else fractions[next])) / 2.0
        val color = paint?.colorAt(middle)?.let { Color(it) } ?: unpainted
        if (color != runColor) {
            runColor?.let { drawPath(path, it, style = stroke) }
            runColor = color
            path = Path().apply { at(index).let { moveTo(it.x, it.y) } }
        }
        at(next).let { path.lineTo(it.x, it.y) }
    }
    runColor?.let { drawPath(path, it, style = stroke) }
}

/** What a badge on the live map stands for. */
private enum class BadgeKind { Detection, Activation, SpeedTrap }

/** A badge on the live map: the point on the track it marks and where it sits, in screen pixels. */
private class MapBadge(val kind: BadgeKind, val anchor: Offset, val centre: Offset)

/**
 * Where the live map's badges go: each a short stem off its point, outside
 * the circuit unless the inside has more room (the screen edge, other track,
 * or a badge already placed). Overtake detection and activation on the 2026
 * cars; speed traps always.
 */
private fun Density.mapBadges(track: TrackOutline, fit: TrackFit, slm: Boolean, width: Float, height: Float): List<MapBadge> {
    val line = track.centerline.map(fit::toScreen)
    val outward = track.outward.map(fit::turn)
    val placed = ArrayList<MapBadge>()
    fun place(kind: BadgeKind, at: Point) {
        if (line.isEmpty()) return
        val anchor = fit.toScreen(at)
        placed += MapBadge(kind, anchor, badgeSpot(line, outward, anchor, placed.map { it.centre }, width, height))
    }
    if (slm) {
        track.overtakeDetection?.let { place(BadgeKind.Detection, it) }
        track.overtakeActivation?.let { place(BadgeKind.Activation, it.point) }
    }
    for (trap in track.speedTraps) place(BadgeKind.SpeedTrap, trap)
    return placed
}

/**
 * Where a badge of [radius] on a stem of [reach] from [anchor] should sit:
 * along the track's normal there, outside or inside, turned by each of
 * [angles] (degrees), taking the spot with the most room from the track,
 * the screen edge and the badges already [taken]. Outside, and straight out,
 * win a near tie.
 */
private fun Density.badgeSpot(
    line: List<Offset>,
    outward: List<Offset>,
    anchor: Offset,
    taken: List<Offset>,
    width: Float,
    height: Float,
    radius: Float = BadgeRadius.toPx(),
    reach: Float = BadgeReach.toPx(),
    angles: FloatArray = StraightOut,
): Offset {
    val normal = outward.getOrElse(nearestIndex(line, anchor)) { Offset(0f, -1f) }
    val edge = 2.dp.toPx()
    val clearOf = radius + BadgeRadius.toPx()
    var best = anchor + normal * reach
    var bestScore = Float.NEGATIVE_INFINITY
    for (side in intArrayOf(1, -1)) for (angle in angles) {
        val radians = Math.toRadians(angle.toDouble())
        val c = cos(radians).toFloat()
        val s = sin(radians).toFloat()
        val n = normal * side.toFloat()
        val direction = Offset(n.x * c - n.y * s, n.x * s + n.y * c)
        val centre = anchor + direction * reach
        if (centre.x < radius + edge || centre.y < radius + edge || centre.x > width - radius - edge || centre.y > height - radius - edge) continue
        var room = Float.POSITIVE_INFINITY
        for (point in line) room = min(room, hypot(point.x - centre.x, point.y - centre.y) - radius)
        for (other in taken) room = min(room, hypot(other.x - centre.x, other.y - centre.y) - clearOf)
        val score = room + (if (side == 1) 4.dp.toPx() else 0f) - abs(angle) / 35f * 2.dp.toPx()
        if (score > bestScore) {
            bestScore = score
            best = centre
        }
    }
    return best
}

private val StraightOut = floatArrayOf(0f)
/** Braking zones sit close together in the infield, so they may also lean. */
private val ZoneBadgeAngles = floatArrayOf(0f, -35f, 35f)

/**
 * One badge: a dot on the track, a stem, and a tinted disc with its icon. The
 * overtake points take the zone colour; the speed trap stays neutral.
 */
private fun DrawScope.drawBadge(badge: MapBadge, origin: Offset, zoneColor: Color, trackColor: Color) {
    val anchor = badge.anchor - origin
    val centre = badge.centre - origin
    val radius = BadgeRadius.toPx()
    val trap = badge.kind == BadgeKind.SpeedTrap
    val ring = if (trap) trackColor.copy(alpha = 0.55f) else zoneColor
    val ink = if (trap) trackColor else androidx.compose.ui.graphics.lerp(zoneColor, trackColor, 0.45f)
    val toAnchor = anchor - centre
    val length = toAnchor.getDistance().takeIf { it > 0f } ?: 1f
    drawLine(ring, anchor, centre + toAnchor * (radius / length), 1.2.dp.toPx())
    drawCircle(if (trap) trackColor.copy(alpha = 0.85f) else zoneColor, 2.5.dp.toPx(), anchor)
    drawCircle(ring.copy(alpha = if (trap) 0.12f else 0.22f), radius, centre)
    drawCircle(ring, radius - 0.5.dp.toPx(), centre, style = Stroke(1.dp.toPx()))

    // The icon, from a 24-unit grid.
    val size = BadgeIcon.toPx()
    val scale = size / 24f
    val corner = centre - Offset(size / 2f, size / 2f)
    fun at(x: Float, y: Float) = Offset(corner.x + x * scale, corner.y + y * scale)
    val stroke = Stroke(2.4f * scale, cap = StrokeCap.Round, join = StrokeJoin.Round)
    when (badge.kind) {
        BadgeKind.Detection -> {
            drawCircle(ink, 8f * scale, at(12f, 13.5f), style = stroke)
            drawPath(
                Path().apply {
                    moveTo(at(12f, 9.5f).x, at(12f, 9.5f).y)
                    lineTo(at(12f, 13.5f).x, at(12f, 13.5f).y)
                    lineTo(at(14.5f, 15.5f).x, at(14.5f, 15.5f).y)
                    moveTo(at(9.5f, 2.5f).x, at(9.5f, 2.5f).y)
                    lineTo(at(14.5f, 2.5f).x, at(14.5f, 2.5f).y)
                },
                ink, style = stroke,
            )
        }
        BadgeKind.Activation -> drawPath(
            Path().apply {
                val bolt = listOf(at(13.5f, 2f), at(5f, 13.5f), at(11.5f, 13.5f), at(10.5f, 22f), at(19f, 10.5f), at(12.5f, 10.5f))
                moveTo(bolt[0].x, bolt[0].y)
                for (point in bolt.drop(1)) lineTo(point.x, point.y)
                close()
            },
            ink,
        )
        BadgeKind.SpeedTrap -> {
            drawArc(ink, 151.9f, 236.2f, useCenter = false, topLeft = at(3.5f, 5f), size = Size(17f * scale, 17f * scale), style = stroke)
            drawLine(ink, at(12f, 14.5f), at(16f, 10f), stroke.width, StrokeCap.Round)
        }
    }
}

/** How near a car a press must land to pick it up, and how far a drag may stray from its lap before it looks further. */
private val CarGrab = 28.dp
private val CarDragReach = 40.dp
/** A dragged car looks this many seconds either side of where it was first, so a hairpin's other side is not taken. */
private const val CAR_DRAG_WINDOW_SECONDS = 4.0

/** One lap's recorded positions on screen, and when it was at each. */
private class CarPath(private val times: FloatArray, private val xs: FloatArray, private val ys: FloatArray) {
    /**
     * The seconds into the lap at the point of the lap nearest [point]:
     * within [window] seconds of [around] first, and over the whole lap only
     * when that nearest point is more than [reach] away.
     */
    fun timeNear(point: Offset, around: Double, window: Double, reach: Float): Double? {
        val near = nearest(point, around - window, around + window)
        if (near != null && near.second <= reach) return near.first
        return nearest(point, Double.NEGATIVE_INFINITY, Double.POSITIVE_INFINITY)?.first
    }

    private fun nearest(point: Offset, from: Double, to: Double): Pair<Double, Float>? {
        var bestTime = Double.NaN
        var bestDistance = Float.POSITIVE_INFINITY
        for (index in 0 until times.size - 1) {
            val t0 = times[index]
            val t1 = times[index + 1]
            if (t1 < from || t0 > to) continue
            val x0 = xs[index]
            val y0 = ys[index]
            val x1 = xs[index + 1]
            val y1 = ys[index + 1]
            if (x0.isNaN() || x1.isNaN()) continue
            val dx = x1 - x0
            val dy = y1 - y0
            val lengthSquared = dx * dx + dy * dy
            val along = if (lengthSquared > 0f) (((point.x - x0) * dx + (point.y - y0) * dy) / lengthSquared).coerceIn(0f, 1f) else 0f
            val distance = hypot(point.x - (x0 + dx * along), point.y - (y0 + dy * along))
            if (distance < bestDistance) {
                bestDistance = distance
                bestTime = t0 + (t1 - t0) * along.toDouble()
            }
        }
        return if (bestTime.isNaN()) null else bestTime to bestDistance
    }
}

private fun carPath(lap: AnalysisLap, track: TrackOutline, fit: TrackFit): CarPath {
    val count = lap.positionTime.size
    val xs = FloatArray(count)
    val ys = FloatArray(count)
    for (index in 0 until count) {
        val x = lap.positionX[index]
        val z = lap.positionZ[index]
        if (!x.isFinite() || !z.isFinite()) {
            xs[index] = Float.NaN
            ys[index] = Float.NaN
            continue
        }
        val at = fit.toScreen(track.mapPosition(x.toDouble(), z.toDouble()))
        xs[index] = at.x
        ys[index] = at.y
    }
    return CarPath(lap.positionTime, xs, ys)
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

    /** This fit seen through [zoom]: the same turn, larger and moved. */
    fun zoomed(zoom: MapZoom): TrackFit {
        if (zoom == MapZoom.Whole) return this
        val k = zoom.scale
        val dx = zoom.offset.x
        val dy = zoom.offset.y
        return TrackFit(
            degrees, scale * k, offsetX * k + dx, offsetY * k + dy,
            left = left * k + dx,
            top = top * k + dy,
            right = right * k + dx,
            bottom = bottom * k + dy,
        )
    }
}

/** The Analysis map's zoom limit, and how far a double tap zooms in. */
private const val MAX_MAP_ZOOM = 6f
private const val DOUBLE_TAP_ZOOM = 2.5f
private const val ZOOM_EPSILON = 0.001f
private const val MAP_DOUBLE_TAP_MS = 300L

/**
 * A zoom of the map, in screen pixels: points scaled by [scale] about the
 * map's top-left corner, then moved by [offset]. [Whole] is the whole circuit.
 */
internal data class MapZoom(val scale: Float, val offset: Offset) {
    /** This zoom, then [next] on top of it. */
    fun then(next: MapZoom) = MapZoom(scale * next.scale, offset * next.scale + next.offset)

    /**
     * Pinched by [zoom] about [centroid] and moved by [pan], with the total
     * (this on top of [under]) kept between the whole circuit and [MAX_MAP_ZOOM].
     */
    fun pinched(zoom: Float, centroid: Offset, pan: Offset, under: Float): MapZoom {
        val total = (under * scale * zoom).coerceIn(1f, MAX_MAP_ZOOM)
        val step = total / (under * scale)
        return MapZoom(scale * step, (offset - centroid) * step + centroid + pan)
    }

    fun panned(by: Offset) = MapZoom(scale, offset + by)

    companion object {
        val Whole = MapZoom(1f, Offset.Zero)
    }
}

/**
 * Kept so the circuit of [fit] always crosses the middle of a [width] by
 * [height] map, so it can never be moved out of sight; at the whole
 * circuit, exactly the fit.
 */
private fun MapZoom.clamped(fit: TrackFit, width: Float, height: Float): MapZoom {
    if (scale <= 1f + ZOOM_EPSILON) return MapZoom.Whole
    val x = offset.x.coerceIn(width / 2f - fit.right * scale, width / 2f - fit.left * scale)
    val y = offset.y.coerceIn(height / 2f - fit.bottom * scale, height / 2f - fit.top * scale)
    return MapZoom(scale.coerceAtMost(MAX_MAP_ZOOM), Offset(x, y))
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

/** Height of the [Scrubber]: its touch target. */
private val BarHeight = 36.dp

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
internal fun Scrubber(
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
 * The circuit with every car where it is now: a dot in each car's livery
 * colour, ringed in the page colour, with its label set beside the circuit
 * on a hairline. Sector boundaries are even cuts with a tick, and overtake
 * detection, activation and speed traps are icon badges on short stems. The
 * circuit and the labels are drawn once into textures. Each display frame draws every car a
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
    val colors = MaterialTheme.colorScheme
    val labelStyle = androidx.compose.ui.text.TextStyle(
        fontSize = 9.5.sp,
        fontWeight = FontWeight.SemiBold,
        letterSpacing = 0.2.sp,
        color = colors.onSurface,
    )
    // The car labels: a tonal surface with a hairline outline, set off the track.
    val labelFill = colors.surfaceContainer
    val labelOutline = colors.outlineVariant
    val leaderColor = colors.onSurfaceVariant.copy(alpha = 0.6f)
    val boundaryColor = colors.onSurfaceVariant
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
                // Offscreen, so the ring round each dot can cut through to the page.
                .graphicsLayer { compositingStrategy = CompositingStrategy.Offscreen }
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
                    // Room round the circuit for the zone lines and the badges.
                    val pad = (BadgeReach + BadgeRadius + 4.dp).toPx()
                    val originX = floor(fit.left - pad).coerceAtLeast(0f)
                    val originY = floor(fit.top - pad).coerceAtLeast(0f)
                    val boxWidth = (ceil(fit.right + pad) - originX).coerceIn(1f, size.width - originX)
                    val boxHeight = (ceil(fit.bottom + pad) - originY).coerceIn(1f, size.height - originY)
                    val circuit = ImageBitmap(boxWidth.toInt().coerceAtLeast(1), boxHeight.toInt().coerceAtLeast(1))
                    val origin = Offset(originX, originY)
                    val badges = mapBadges(track, fit, slm, size.width, size.height)
                    CanvasDrawScope().draw(this, layoutDirection, androidx.compose.ui.graphics.Canvas(circuit), Size(boxWidth, boxHeight)) {
                        drawCircuit(track, fit, origin, palette, trackColor, slm, slmTrackStatus, boundaryColor = boundaryColor, badges = badges)
                    }
                    val circuitTexture = circuit.asAndroidBitmap().copy(android.graphics.Bitmap.Config.HARDWARE, false)
                        ?.asImageBitmap() ?: circuit
                    val sectorNames = measureSectorLabels(track, palette, trackColor, sectorStyle, null) { text, style -> measurer.measure(text, style) }
                    val sectorPlaces = sectorLabelPlaces(track, fit, sectorNames, track.aeroZones(slm, slmTrackStatus), size.width, size.height)

                    // Each label, drawn once: the code on a tonal surface with a hairline outline.
                    val labelHeight = LiveLabelHeight.toPx()
                    val padX = 5.dp.toPx()
                    val minWidth = 30.dp.toPx()
                    val corner = CornerRadius(4.dp.toPx())
                    val hairline = 1.dp.toPx()
                    val sprites = cars.map { car ->
                        val text = measurer.measure(car.label, labelStyle)
                        val spriteWidth = max(minWidth, text.size.width + padX * 2)
                        val sprite = ImageBitmap(ceil(spriteWidth).toInt(), ceil(labelHeight).toInt())
                        CanvasDrawScope().draw(this, layoutDirection, androidx.compose.ui.graphics.Canvas(sprite), Size(spriteWidth, labelHeight)) {
                            drawRoundRect(labelFill, size = size, cornerRadius = corner)
                            drawRoundRect(
                                labelOutline, topLeft = Offset(hairline / 2, hairline / 2),
                                size = Size(size.width - hairline, size.height - hairline),
                                cornerRadius = corner, style = Stroke(hairline),
                            )
                            drawText(text, topLeft = Offset((size.width - text.size.width) / 2f, (size.height - text.size.height) / 2f))
                        }
                        sprite.asAndroidBitmap().copy(android.graphics.Bitmap.Config.HARDWARE, false)?.asImageBitmap() ?: sprite
                    }
                    val line = track.centerline.map(fit::toScreen)
                    val snap = SnapFraction * max(size.width, size.height)
                    val labelWidths = FloatArray(cars.size) { sprites[it].width.toFloat() }
                    val labelHeights = FloatArray(cars.size) { sprites[it].height.toFloat() }
                    val layout = LabelLayout(
                        ruler = TrackRuler(line, track.outward.map(fit::turn), smoothing = 18.dp.toPx()),
                        reach = LiveLabelReach.toPx(), gap = 3.dp.toPx(), margin = 2.dp.toPx(),
                        width = size.width, height = size.height, snap = snap,
                        switchAfter = 16.dp.toPx(), swapAfter = 10.dp.toPx(), mergeShorterThan = 60.dp.toPx(),
                        typicalWidth = labelWidths.maxOrNull() ?: minWidth, typicalHeight = labelHeight,
                        slots = cars.size,
                    )
                    val dotRadius = LiveDotRadius.toPx()
                    val ring = LiveDotRing.toPx()
                    val leaderWidth = 1.dp.toPx()
                    val points = arrayOfNulls<Offset>(cars.size)
                    val nearest = IntArray(cars.size)
                    val seen = BooleanArray(cars.size)
                    val lostDistance = 12.dp.toPx()
                    val labels = arrayOfNulls<Offset>(cars.size)
                    glides.reset()
                    onDrawBehind {
                        val frame = frameTick.value
                        drawImage(circuitTexture, topLeft = origin)
                        drawSectorLabels(sectorNames, sectorPlaces)
                        val now = positions()
                        cars.forEachIndexed { slot, car ->
                            val index = car.index
                            points[slot] = null
                            if (index !in 0 until CarGlides.CARS || !now.hasPosition(index)) {
                                seen[slot] = false
                                return@forEachIndexed
                            }
                            val target = fit.toScreen(track.mapPosition(now.xAt(index), now.zAt(index)))
                            val point = glides.position(index, target.x, target.y, frame, snap)
                            points[slot] = point
                            nearest[slot] = followTrack(line, point, if (seen[slot]) nearest[slot] else -1, lostDistance)
                            seen[slot] = true
                        }
                        layout.layout(points, nearest, labelWidths, labelHeights, labels)
                        cars.indices.forEach { slot ->
                            val point = points[slot] ?: return@forEach
                            val label = labels[slot] ?: return@forEach
                            drawLine(leaderColor, point, label, leaderWidth)
                        }
                        cars.forEachIndexed { slot, car ->
                            val point = points[slot] ?: return@forEachIndexed
                            drawCircle(Color.Black, dotRadius + ring, point, blendMode = BlendMode.Clear)
                            drawCircle(car.color, dotRadius, point)
                        }
                        cars.indices.forEach { slot ->
                            val label = labels[slot] ?: return@forEach
                            val sprite = sprites[slot]
                            drawImage(sprite, topLeft = Offset(label.x - sprite.width / 2f, label.y - sprite.height / 2f))
                        }
                    }
                },
        )
    }
}

/** Index of the [line] point nearest [point]. */
private fun nearestIndex(line: List<Offset>, point: Offset): Int {
    var best = 0
    var bestDistance = Float.POSITIVE_INFINITY
    for (index in line.indices) {
        val distance = distanceSquared(line[index], point)
        if (distance < bestDistance) {
            bestDistance = distance
            best = index
        }
    }
    return best
}

/**
 * Where a car is along the centreline: searched forwards and back from its
 * last place [hint] first, so it cannot hop to a nearby stretch of other track
 * (an S-bend, a hairpin); the whole lap only when it is new or has strayed
 * more than [lost] from the local best.
 */
private fun followTrack(line: List<Offset>, point: Offset, hint: Int, lost: Float): Int {
    if (hint < 0 || line.isEmpty()) return nearestIndex(line, point)
    var best = hint
    var bestDistance = Float.POSITIVE_INFINITY
    for (step in -FollowWindow..FollowWindow) {
        val index = ((hint + step) % line.size + line.size) % line.size
        val distance = distanceSquared(line[index], point)
        if (distance < bestDistance) {
            bestDistance = distance
            best = index
        }
    }
    return if (bestDistance > lost * lost) nearestIndex(line, point) else best
}

private const val FollowWindow = 40

/**
 * The centreline on screen measured by distance along it, as one closed loop,
 * with the outward normal averaged over [smoothing] of track either side so
 * labels turn gently through a corner instead of swinging round its apex.
 */
private class TrackRuler(private val line: List<Offset>, normals: List<Offset>, smoothing: Float) {
    val count = line.size
    val arc = FloatArray(count)
    val length: Float
    private val smooth: Array<Offset>

    init {
        var total = 0f
        for (index in 1 until count) {
            total += (line[index] - line[index - 1]).getDistance()
            arc[index] = total
        }
        length = (total + if (count > 1) (line[0] - line[count - 1]).getDistance() else 0f).coerceAtLeast(1f)
        smooth = Array(count) { index ->
            var sum = normals[index]
            for (direction in intArrayOf(1, -1)) {
                var step = 1
                while (step < count / 2) {
                    val other = ((index + direction * step) % count + count) % count
                    if (span(index, other) > smoothing) break
                    sum += normals[other]
                    step++
                }
            }
            val size = sum.getDistance()
            if (size > 0f) sum / size else normals[index]
        }
    }

    /** Distance along the loop between two points, the short way round. */
    private fun span(a: Int, b: Int): Float {
        val direct = abs(arc[a] - arc[b])
        return min(direct, length - direct)
    }

    fun wrap(distance: Float) = ((distance % length) + length) % length

    /** The segment holding [distance]: the last point at or before it. */
    fun segment(distance: Float): Int {
        val target = wrap(distance)
        var low = 0
        var high = count - 1
        while (low < high) {
            val mid = (low + high + 1) / 2
            if (arc[mid] <= target) low = mid else high = mid - 1
        }
        return low
    }

    fun at(distance: Float): Offset {
        val index = segment(distance)
        val next = (index + 1) % count
        val end = if (next == 0) length else arc[next]
        val span = end - arc[index]
        val fraction = if (span > 0f) (wrap(distance) - arc[index]) / span else 0f
        return line[index] + (line[next] - line[index]) * fraction
    }

    fun normal(distance: Float): Offset = smooth[segment(distance)]

    fun tangent(distance: Float): Offset {
        val index = segment(distance)
        val step = line[(index + 1) % count] - line[index]
        val size = step.getDistance()
        return if (size > 0f) step / size else Offset(1f, 0f)
    }

    /**
     * How far along the loop [point] is, projected onto the segments either
     * side of its nearest centreline point, so it moves smoothly rather than
     * in steps from one centreline point to the next.
     */
    fun project(point: Offset, nearest: Int): Float {
        var best = arc[nearest]
        var bestDistance = Float.POSITIVE_INFINITY
        for (start in intArrayOf((nearest - 1 + count) % count, nearest)) {
            val end = (start + 1) % count
            val a = line[start]
            val segment = line[end] - a
            val lengthSquared = segment.x * segment.x + segment.y * segment.y
            val t = if (lengthSquared > 0f) (((point.x - a.x) * segment.x + (point.y - a.y) * segment.y) / lengthSquared).coerceIn(0f, 1f) else 0f
            val onto = a + segment * t
            val distance = distanceSquared(onto, point)
            if (distance < bestDistance) {
                bestDistance = distance
                val segmentEnd = if (end == 0) length else arc[end]
                best = arc[start] + (segmentEnd - arc[start]) * t
            }
        }
        return wrap(best)
    }
}

/**
 * Lays the car labels out beside the circuit.
 *
 * Which side of the track a label goes on is decided once per stretch of
 * track, not per car: outside, or inside where the outside runs off the
 * screen, with short flips merged away, so the side only changes at a few
 * fixed places on the lap. A car crossing one of those places switches side
 * only once it is [switchAfter] into the new stretch.
 *
 * Labels on the same side are spaced along the track in the order the cars
 * run, each as near its own car as the spacing allows, so a pack fans out
 * evenly and the hairlines never cross. Two cars only swap order once one is
 * [swapAfter] clear of the other, so cars running side by side keep their
 * labels still. What is drawn eases toward the layout, so labels glide.
 */
private class LabelLayout(
    private val ruler: TrackRuler,
    private val reach: Float,
    private val gap: Float,
    private val margin: Float,
    private val width: Float,
    private val height: Float,
    private val snap: Float,
    private val switchAfter: Float,
    private val swapAfter: Float,
    mergeShorterThan: Float,
    typicalWidth: Float,
    typicalHeight: Float,
    slots: Int,
) {
    private val sideAt = IntArray(ruler.count)
    private val side = IntArray(slots)
    private val along = FloatArray(slots)
    private val pushed = FloatArray(slots)
    private val rank = IntArray(slots) { -1 }
    private val shownX = FloatArray(slots)
    private val shownY = FloatArray(slots)
    private val shown = BooleanArray(slots)
    private val order = ArrayList<Int>(slots)
    private val base = FloatArray(slots)
    private val wanted = FloatArray(slots)
    private val blockSum = FloatArray(slots)
    private val blockSize = IntArray(slots)

    init {
        for (index in 0 until ruler.count) {
            val distance = ruler.arc[index]
            sideAt[index] = when {
                fits(distance, 1, typicalWidth, typicalHeight) -> 1
                fits(distance, -1, typicalWidth, typicalHeight) -> -1
                else -> 1
            }
        }
        // Merge short runs into the run before them, so the side changes only
        // at a few places. Twice, as merging can leave new short runs.
        repeat(2) { mergeShortRuns(mergeShorterThan) }
    }

    private fun mergeShortRuns(shorter: Float) {
        val count = ruler.count
        if (count < 2) return
        // Start at a run boundary so no run is split across the wrap.
        val start = (0 until count).firstOrNull { sideAt[it] != sideAt[(it - 1 + count) % count] } ?: return
        var runStart = start
        var step = 1
        while (step <= count) {
            val index = (start + step) % count
            if (step == count || sideAt[index] != sideAt[runStart]) {
                val runEnd = (start + step - 1) % count
                var runLength = ruler.arc[runEnd] - ruler.arc[runStart]
                if (runLength < 0f) runLength += ruler.length
                if (runLength < shorter) {
                    val before = sideAt[(runStart - 1 + count) % count]
                    var fill = runStart
                    while (true) {
                        sideAt[fill] = before
                        if (fill == runEnd) break
                        fill = (fill + 1) % count
                    }
                }
                runStart = index
            }
            step++
        }
    }

    /** Fills [out] with each label's centre; null where its car has no position. */
    fun layout(points: Array<Offset?>, nearest: IntArray, w: FloatArray, h: FloatArray, out: Array<Offset?>) {
        for (slot in points.indices) {
            val point = points[slot]
            if (point == null) {
                shown[slot] = false
                side[slot] = 0
                rank[slot] = -1
                continue
            }
            along[slot] = ruler.project(point, nearest[slot])
            val here = sideAt[ruler.segment(along[slot])]
            val next = when {
                side[slot] == 0 -> here
                side[slot] == here -> here
                // Switch only once well into the new stretch.
                sideAt[ruler.segment(along[slot] - switchAfter)] == here -> here
                else -> side[slot]
            }
            // A label joining the other side's queue takes its place there afresh.
            if (next != side[slot]) rank[slot] = -1
            side[slot] = next
        }
        for (group in intArrayOf(1, -1)) spread(points, group, w, h)
        for (slot in points.indices) {
            if (points[slot] == null) {
                out[slot] = null
                continue
            }
            val target = centre(pushed[slot], side[slot], w[slot], h[slot])
            if (!shown[slot] || hypot(target.x - shownX[slot], target.y - shownY[slot]) > snap) {
                shownX[slot] = target.x
                shownY[slot] = target.y
                shown[slot] = true
            } else {
                shownX[slot] += (target.x - shownX[slot]) * Ease
                shownY[slot] += (target.y - shownY[slot]) * Ease
            }
            out[slot] = Offset(
                shownX[slot].coerceIn(margin + w[slot] / 2f, width - margin - w[slot] / 2f),
                shownY[slot].coerceIn(margin + h[slot] / 2f, height - margin - h[slot] / 2f),
            )
        }
    }

    /** Spaces one side's labels along the track, keeping the cars' order. */
    private fun spread(points: Array<Offset?>, group: Int, w: FloatArray, h: FloatArray) {
        order.clear()
        for (slot in points.indices) if (points[slot] != null && side[slot] == group) order += slot
        if (order.isEmpty()) return
        order.sortBy { along[it] }
        // Unroll the loop at the widest gap between cars, so a pack across the
        // start/finish line stays one pack.
        var cut = 0
        var widest = -1f
        for (index in order.indices) {
            val before = if (index == 0) along[order.last()] - ruler.length else along[order[index - 1]]
            val gapHere = along[order[index]] - before
            if (gapHere > widest) {
                widest = gapHere
                cut = index
            }
        }
        if (cut > 0) {
            val head = order.subList(cut, order.size).toList()
            val tail = order.subList(0, cut).toList()
            order.clear()
            order += head
            for (slot in tail) {
                order += slot
            }
        }
        var offset = 0f
        for (index in order.indices) {
            val slot = order[index]
            if (index > 0 && along[slot] + offset < pushed[order[index - 1]] - ruler.length / 2f) offset += ruler.length
            pushed[slot] = along[slot] + offset
        }
        // Keep last frame's order between cars that are nearly level: a pair
        // only swaps once the one behind is clearly ahead.
        for (pass in order.indices) {
            var swapped = false
            for (index in 1 until order.size) {
                val a = order[index - 1]
                val b = order[index]
                if (rank[a] >= 0 && rank[b] >= 0 && rank[a] > rank[b] && pushed[b] - pushed[a] < swapAfter) {
                    order[index - 1] = b
                    order[index] = a
                    swapped = true
                }
            }
            if (!swapped) break
        }
        // Where last frame's order is kept, a car's own spot can sit behind the
        // one before it; the fit below simply pools them.
        for (index in order.indices) rank[order[index]] = index

        // The positions nearest each car's own that keep every pair far enough
        // apart: shift out the running spacing, fit a non-decreasing sequence
        // (pool adjacent violators), then add the spacing back.
        var spacing = 0f
        for (index in order.indices) {
            if (index > 0) spacing += separation(order[index - 1], order[index], w, h)
            base[index] = spacing
            wanted[index] = pushed[order[index]] - spacing
        }
        var blocks = 0
        for (index in order.indices) {
            blockSum[blocks] = wanted[index]
            blockSize[blocks] = 1
            blocks++
            while (blocks > 1 && blockSum[blocks - 2] / blockSize[blocks - 2] > blockSum[blocks - 1] / blockSize[blocks - 1]) {
                blockSum[blocks - 2] += blockSum[blocks - 1]
                blockSize[blocks - 2] += blockSize[blocks - 1]
                blocks--
            }
        }
        var index = 0
        for (block in 0 until blocks) {
            val mean = blockSum[block] / blockSize[block]
            repeat(blockSize[block]) {
                pushed[order[index]] = mean + base[index]
                index++
            }
        }
    }

    /** How far apart along the track two labels must be so their boxes clear. */
    private fun separation(a: Int, b: Int, w: FloatArray, h: FloatArray): Float {
        val tangent = ruler.tangent((pushed[a] + pushed[b]) / 2f)
        val across = (w[a] + w[b]) / 2f + gap
        val down = (h[a] + h[b]) / 2f + gap
        val byWidth = if (abs(tangent.x) > 1e-3f) across / abs(tangent.x) else Float.POSITIVE_INFINITY
        val byHeight = if (abs(tangent.y) > 1e-3f) down / abs(tangent.y) else Float.POSITIVE_INFINITY
        return min(byWidth, byHeight)
    }

    private fun centre(distance: Float, group: Int, w: Float, h: Float): Offset {
        val normal = ruler.normal(distance) * group.toFloat()
        val out = reach + w / 2f * abs(normal.x) + h / 2f * abs(normal.y)
        return ruler.at(distance) + normal * out
    }

    private fun fits(distance: Float, group: Int, w: Float, h: Float): Boolean {
        val c = centre(distance, group, w, h)
        return c.x - w / 2f >= margin && c.x + w / 2f <= width - margin && c.y - h / 2f >= margin && c.y + h / 2f <= height - margin
    }

    private companion object {
        const val Ease = 0.3f
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
        outward = rotatedCenterline.indices.map { index ->
            val normal = perpendicular(rotatedCenterline, index)
            if (pointInPolygon(rotatedCenterline[index] + normal * 3f, rotatedCenterline)) -normal else normal
        },
        startFinish = startFinish,
        drsZones = zones("drs_zones"),
        slmDryZones = zones("slm_dry"),
        slmWetZones = zones("slm_wet"),
        speedTraps = if (traps == null) emptyList() else List(traps.length()) { point(traps.optJSONArray(it)) }
            .mapNotNull { it?.let(rotation::apply) },
        overtakeDetection = point(root.optJSONArray("overtake_detection_point"))?.let(rotation::apply),
        overtakeActivation = activation,
        lapStart = startFinish?.let { finish ->
            rotatedCenterline.indices.minByOrNull { distanceSquared(rotatedCenterline[it], finish.point) }
        } ?: 0,
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
