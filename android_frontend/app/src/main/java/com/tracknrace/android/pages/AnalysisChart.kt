package com.tracknrace.android.pages

import android.graphics.Bitmap
import android.graphics.Paint
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.Density
import androidx.compose.ui.graphics.drawscope.CanvasDrawScope
import com.tracknrace.android.AnalysisCatalog
import com.tracknrace.android.AnalysisDeltaCurve
import com.tracknrace.android.AnalysisDeltaSummary
import com.tracknrace.android.AnalysisLap
import com.tracknrace.android.AnalysisSeries
import com.tracknrace.android.formatDelta
import com.tracknrace.android.resolvedSectorSplits
import java.util.Locale
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext

private const val MIN_ZOOM_SECONDS = 0.5
private const val MIN_ZOOM_METRES = 25.0
private const val DOUBLE_TAP_MS = 320L
private const val RASTER_SETTLE_MS = 48L
private const val HEADER_DP = 20f
private const val HEADER_SAMPLE_MS = 100L
private const val PANEL_GAP_DP = 6f
private const val X_AXIS_DP = 20f
// Below this a stacked panel's trace is too flat to read; past it the graph
// scrolls. A landscape phone is too short for that, so it settles for less.
private const val MIN_PANEL_DP = 88f
private const val MIN_PANEL_SHORT_DP = 52f
private const val SHORT_VIEWPORT_DP = 420f
private val YTicks = floatArrayOf(0f, 0.25f, 0.5f, 0.75f, 1f)

/** Everything one chart frame draws; laps compare by identity. */
@Immutable
internal data class AnalysisChartInput(
    val current: AnalysisLap?,
    val comparison: AnalysisLap?,
    val series: List<AnalysisSeries>,
    val distanceMode: Boolean,
    val trackLengthM: Double,
    val delta: AnalysisDeltaCurve?,
    val showDelta: Boolean,
    val stacked: Boolean,
    val sectorBoundaries: Boolean,
    /** The current lap is still being played: draw it only up to the desktop cursor. */
    val realtime: Boolean,
    val zoomEnabled: Boolean,
    val deltaPositive: Color,
    val deltaNegative: Color,
    /** Split view in fixed mode: mark where each lap's car is on the map. */
    val mapCursorColors: List<Color>?,
) {
    /** Full x extent: the track length or longest lap in metres, else the longest lap in seconds. */
    val fullRange: Double
        get() = if (distanceMode) {
            if (trackLengthM > 0) trackLengthM else max(current?.lastDistance ?: 0.0, comparison?.lastDistance ?: 0.0)
        } else {
            max(current?.duration ?: 0.0, comparison?.duration ?: 0.0)
        }.coerceAtLeast(1.0)

    fun panelItems(): List<AnalysisSeries> = series.filter {
        it.visible && AnalysisCatalog.hasLines(it) &&
            (it.metricId != AnalysisCatalog.DELTA_ID || showDelta)
    }
}

/** Zoom and the inspected position, kept by the page across recompositions. */
@Stable
internal class AnalysisChartState {
    /** Visible x range, or null for the whole lap. */
    var zoom by mutableStateOf<Pair<Double, Double>?>(null)

    /** Inspected x in chart units, or null. */
    var crosshair by mutableStateOf<Double?>(null)

    internal var geometry: ChartGeometry? = null

    val zoomed: Boolean get() = zoom != null

    fun resetZoom() {
        zoom = null
    }

    fun domain(full: Double): Pair<Double, Double> = zoom ?: (0.0 to full)

    fun zoomBy(factor: Double, anchor: Double, full: Double, minExtent: Double) {
        val (min, max) = domain(full)
        applyDomain(anchor + (min - anchor) * factor, anchor + (max - anchor) * factor, full, minExtent)
    }

    fun panBy(delta: Double, full: Double, minExtent: Double) {
        val (min, max) = domain(full)
        applyDomain(min + delta, max + delta, full, minExtent)
    }

    private fun applyDomain(requestedMin: Double, requestedMax: Double, full: Double, minExtent: Double) {
        val extent = (requestedMax - requestedMin).coerceIn(min(minExtent, full), full)
        var start = requestedMin - (extent - (requestedMax - requestedMin)) / 2
        start = start.coerceIn(0.0, full - extent)
        zoom = if (extent >= full - 1e-6) null else start to start + extent
    }
}

/** One plot area. [headerTop] is where its title row starts, above [top]. */
internal class ChartPanel(val item: AnalysisSeries?, val headerTop: Float, val top: Float, val bottom: Float)

internal class ChartGeometry(
    val left: Float,
    val right: Float,
    val top: Float,
    val bottom: Float,
    val domainMin: Double,
    val domainMax: Double,
    val panels: List<ChartPanel>,
) {
    fun xToPx(x: Double): Float =
        (left + (x - domainMin) / (domainMax - domainMin) * (right - left)).toFloat()

    fun pxToX(px: Float): Double =
        domainMin + ((px - left) / (right - left)).toDouble().coerceIn(0.0, 1.0) * (domainMax - domainMin)
}

private class LabelAt(val layout: TextLayoutResult, val topLeft: Offset, val backdrop: Boolean = false)

private enum class GestureMode { UNDECIDED, SCRUB, SCROLL, TRANSFORM }

/**
 * The Analysis graph: every selected metric of the current lap, and of the
 * comparison lap dimmed beneath it, on one canvas or in one panel each. Each
 * panel's title row reads the values under the crosshair, or at the desktop
 * cursor while following it. Too many stacked panels to fit scroll vertically.
 */
@Composable
internal fun AnalysisChart(
    input: AnalysisChartInput,
    state: AnalysisChartState,
    /** Seconds from the current lap's start to the desktop cursor; NaN when not following it. */
    cursorOffset: State<Double>,
    /** Seconds into the laps shown by the map clock, for split-view cursors. */
    mapElapsed: State<Double>?,
    /** Sector deltas drawn beside the sector names, or null without a comparison. */
    sectorDeltas: State<AnalysisDeltaSummary>?,
    /** Seconds into the current lap at the inspected point, as the finger moves. */
    onScrub: (Double) -> Unit,
    onDoubleTap: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val background = colors.surface
    val gridColor = colors.outlineVariant.copy(alpha = 0.35f)
    val axisColor = colors.onSurfaceVariant
    val crosshairColor = colors.outline
    val density = LocalDensity.current
    val measurer = rememberTextMeasurer(cacheSize = 96)
    // Material type scale, with tabular figures so axis numbers line up.
    val labelStyle = MaterialTheme.typography.labelSmall.copy(color = axisColor, fontFeatureSettings = "tnum")
    val sectorStyle = MaterialTheme.typography.labelMedium.copy(
        color = axisColor,
        fontWeight = FontWeight.SemiBold,
        fontFeatureSettings = "tnum",
    )
    val sectorPaints = remember(density, sectorStyle.fontSize) {
        val sizePx = with(density) { sectorStyle.fontSize.toPx() }
        textPaint(sizePx, FontWeight.SemiBold.weight) to textPaint(sizePx, FontWeight.Medium.weight)
    }
    val latestInput by rememberUpdatedState(input)
    val latestScrub by rememberUpdatedState(onScrub)
    val latestDoubleTap by rememberUpdatedState(onDoubleTap)

    LaunchedEffect(input.zoomEnabled, input.fullRange, input.distanceMode) {
        if (!input.zoomEnabled) state.resetZoom()
        state.zoom?.let { (min, max) ->
            if (max > input.fullRange) state.resetZoom()
            else if (max - min <= 0) state.resetZoom()
        }
    }
    LaunchedEffect(input.distanceMode, input.current, input.comparison) {
        state.crosshair = null
    }

    BoxWithConstraints(modifier.clipToBounds()) {
        val width = constraints.maxWidth
        val viewport = constraints.maxHeight
        val items = remember(input) { input.panelItems() }
        // Stacked panels keep a readable height; the chart grows and scrolls past it.
        val height = if (input.stacked && items.isNotEmpty()) {
            val needed = with(density) {
                val minPanel = if (viewport.toDp().value < SHORT_VIEWPORT_DP) MIN_PANEL_SHORT_DP else MIN_PANEL_DP
                (items.size * (minPanel + HEADER_DP) + (items.size - 1) * PANEL_GAP_DP + X_AXIS_DP + 4f).dp.roundToPx()
            }
            max(viewport, needed)
        } else {
            viewport
        }
        val scrollable = height > viewport
        val latestScrollable by rememberUpdatedState(scrollable)
        val domain = state.domain(input.fullRange)
        val layout = remember(input, width, height, domain, density) {
            layoutChart(input, width.toFloat(), height.toFloat(), density.density, domain)
        }
        val decor = remember(layout, labelStyle) {
            buildDecor(input, layout, width.toFloat(), height.toFloat(), density.density) { text, color ->
                measurer.measure(text, if (color == null) labelStyle else labelStyle.copy(color = color))
            }
        }
        val raster = remember { mutableStateOf<ChartRaster?>(null) }
        LaunchedEffect(layout, decor, background) {
            if (width <= 0 || height <= 0) return@LaunchedEffect
            // A zoom gesture changes the range every frame; let it settle
            // before rendering, and keep showing the stretched previous raster.
            if (raster.value != null) delay(RASTER_SETTLE_MS)
            raster.value = rasterizeTraces(
                input, layout, decor, width, height, density, background, gridColor, axisColor,
            )
        }
        // Per-frame inputs reduced to what is drawn: whole pixels. The desktop
        // cursor and map clock move every frame, but the cut they place on the
        // traces moves a pixel only every few frames.
        val cutoff = remember(layout, input.realtime) {
            derivedStateOf(structuralEqualityPolicy()) { cutoffPixel(input, layout.geometry, width.toFloat(), cursorOffset.value) }
        }
        val mapCursors = remember(layout, mapElapsed) {
            derivedStateOf(structuralEqualityPolicy()) {
                val elapsed = mapElapsed?.value ?: return@derivedStateOf emptyList()
                listOf(input.current, input.comparison).map { lap ->
                    lap ?: return@map -1
                    val clamped = elapsed.coerceIn(0.0, lap.duration)
                    val x = if (input.distanceMode) lap.progress?.distanceAt(clamped) ?: Double.NaN else clamped
                    if (x.isFinite()) layout.geometry.xToPx(x).roundToInt() else -1
                }
            }
        }
        state.geometry = layout.geometry
        val scroll = rememberScrollState()
        Box(Modifier.fillMaxWidth().height(with(density) { viewport.toDp() }).verticalScroll(scroll, enabled = scrollable)) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(with(density) { height.toDp() })
                    .semantics { contentDescription = "Lap analysis graph" }
                    .pointerInput(Unit) {
                        var lastTapAt = Long.MIN_VALUE / 2
                        var lastTapX = 0f
                        fun inspect(px: Float) {
                            val geometry = state.geometry ?: return
                            val x = geometry.pxToX(px)
                            state.crosshair = x
                            inspectElapsed(latestInput, x)?.let(latestScrub)
                        }
                        awaitEachGesture {
                            val down = awaitFirstDown(requireUnconsumed = false)
                            var mode = GestureMode.UNDECIDED
                            var releasedAt = down.uptimeMillis
                            while (true) {
                                val event = awaitPointerEvent()
                                val pressed = event.changes.filter { it.pressed }
                                if (pressed.isEmpty()) {
                                    releasedAt = event.changes.maxOf { it.uptimeMillis }
                                    if (mode != GestureMode.SCROLL) event.changes.forEach { it.consume() }
                                    break
                                }
                                if (mode == GestureMode.SCROLL) continue
                                val current = latestInput
                                val active = state.geometry
                                if (pressed.size >= 2) {
                                    mode = GestureMode.TRANSFORM
                                    if (current.zoomEnabled && active != null) {
                                        val full = current.fullRange
                                        val minExtent = if (current.distanceMode) MIN_ZOOM_METRES else MIN_ZOOM_SECONDS
                                        val zoom = event.calculateZoom()
                                        val centroid = event.calculateCentroid(useCurrent = true)
                                        val span = (active.right - active.left).coerceAtLeast(1f)
                                        val unitsPerPx = (active.domainMax - active.domainMin) / span
                                        if (zoom > 0f && zoom != 1f) {
                                            state.zoomBy(1.0 / zoom, active.pxToX(centroid.x), full, minExtent)
                                        }
                                        val pan = event.calculatePan()
                                        if (pan.x != 0f) state.panBy(-pan.x * unitsPerPx, full, minExtent)
                                    }
                                    event.changes.forEach { it.consume() }
                                    continue
                                }
                                if (mode == GestureMode.TRANSFORM) continue
                                val position = pressed[0].position
                                if (mode == GestureMode.UNDECIDED) {
                                    val moved = position - down.position
                                    if (moved.getDistance() <= viewConfiguration.touchSlop) continue
                                    // A mostly vertical drag scrolls a graph taller
                                    // than the screen; anything else inspects.
                                    mode = if (latestScrollable && abs(moved.y) > abs(moved.x)) {
                                        GestureMode.SCROLL
                                    } else {
                                        GestureMode.SCRUB
                                    }
                                    if (mode == GestureMode.SCROLL) continue
                                }
                                inspect(position.x)
                                event.changes.forEach { it.consume() }
                            }
                            if (mode != GestureMode.UNDECIDED) {
                                lastTapAt = Long.MIN_VALUE / 2
                                return@awaitEachGesture
                            }
                            // A tap inspects; a second quick tap near it opens the map.
                            inspect(down.position.x)
                            if (releasedAt - down.uptimeMillis > DOUBLE_TAP_MS) {
                                lastTapAt = Long.MIN_VALUE / 2
                            } else if (down.uptimeMillis - lastTapAt <= DOUBLE_TAP_MS &&
                                abs(down.position.x - lastTapX) < 48.dp.toPx()
                            ) {
                                lastTapAt = Long.MIN_VALUE / 2
                                latestDoubleTap()
                            } else {
                                lastTapAt = releasedAt
                                lastTapX = down.position.x
                            }
                        }
                    }
            ) {
                // Layers, bottom to top. Only the traces layer reads per-frame
                // state; the grid and the labels are recorded once per layout
                // and the sector labels once per change of their deltas, so a
                // moving cursor never re-draws or re-measures text.
                Spacer(
                    // Its own layer: this redraws as the cursor moves, and
                    // must not drag every other node's drawing along with it.
                    Modifier.matchParentSize().graphicsLayer().drawBehind {
                        val geometry = layout.geometry
                        val dp = density.density
                        val cutoffPx = cutoff.value.toFloat()
                        raster.value?.let { traces ->
                            val settled = traces.domainMin == geometry.domainMin && traces.domainMax == geometry.domainMax
                            val span = geometry.domainMax - geometry.domainMin
                            val scaleX = ((traces.domainMax - traces.domainMin) / span).toFloat()
                            val shift = ((traces.domainMin - geometry.domainMin) / span * (geometry.right - geometry.left)).toFloat()
                            fun stretched(block: androidx.compose.ui.graphics.drawscope.DrawScope.() -> Unit) {
                                // While a new zoom renders, the previous raster is
                                // stretched onto the new range inside the plot.
                                if (settled) {
                                    block()
                                } else {
                                    clipRect(geometry.left, 0f, geometry.right, size.height) {
                                        withTransform({
                                            translate(geometry.left + shift, 0f)
                                            scale(scaleX, 1f, Offset.Zero)
                                            translate(-geometry.left, 0f)
                                        }) { block() }
                                    }
                                }
                            }
                            // Grid, labels and the comparison lap: one image.
                            traces.base?.let { base -> stretched { drawImage(base) } }
                            // The playing lap, cut at the desktop cursor.
                            clipRect(right = cutoffPx) { stretched { drawImage(traces.current) } }
                        }
                        clipRect(geometry.left, 0f, geometry.right, size.height) {
                            val colorsOfLaps = input.mapCursorColors
                            if (colorsOfLaps != null) {
                                mapCursors.value.forEachIndexed { index, px ->
                                    if (px < 0) return@forEachIndexed
                                    drawLine(colorsOfLaps.getOrElse(index) { crosshairColor }, Offset(px.toFloat(), geometry.top), Offset(px.toFloat(), geometry.bottom), 1.5f * dp)
                                }
                            }
                        }
                        state.crosshair?.let { x ->
                            if (x < geometry.domainMin || x > geometry.domainMax) return@let
                            val px = geometry.xToPx(x)
                            drawLine(crosshairColor, Offset(px, geometry.top), Offset(px, geometry.bottom), 1f * dp)
                        }
                    },
                )
                if (decor.sectorCentres.isNotEmpty()) {
                    Spacer(
                        Modifier
                            .align(Alignment.BottomStart)
                            .fillMaxWidth()
                            .height(X_AXIS_DP.dp)
                            // A plain layer: the deltas change ten times a second,
                            // and re-rendering a cached texture that often costs a
                            // render pass each time on a tiled GPU.
                            .graphicsLayer()
                            .drawBehind {
                                // Sector names, each with its delta while comparing.
                                // The deltas change ten times a second, so they are
                                // painted directly rather than laid out as text.
                                val summary = sectorDeltas?.value
                                val (namePaint, deltaPaint) = sectorPaints
                                val metrics = namePaint.fontMetrics
                                val baseline = 3f * density.density - metrics.ascent
                                val canvas = drawContext.canvas.nativeCanvas
                                namePaint.color = axisColor.toArgb()
                                for ((sector, centre) in decor.sectorCentres) {
                                    val name = "S${sector + 1}"
                                    val value = summary?.sectors?.getOrNull(sector)
                                    val delta = if (summary == null) "" else "  " + if (value == null) "\u2014" else formatDelta(value)
                                    val nameWidth = namePaint.measureText(name)
                                    val total = nameWidth + if (delta.isEmpty()) 0f else deltaPaint.measureText(delta)
                                    val x = (centre - total / 2f).clampTo(0f, size.width - total)
                                    canvas.drawText(name, x, baseline, namePaint)
                                    if (delta.isNotEmpty()) {
                                        deltaPaint.color = deltaColor(value, axisColor, input.deltaPositive, input.deltaNegative).toArgb()
                                        canvas.drawText(delta, x + nameWidth, baseline, deltaPaint)
                                    }
                                }
                            },
                    )
                }
            }
            PanelHeaders(input, layout, state, cursorOffset, mapElapsed)
        }
    }
}

/** Grid, sector split lines and axis labels of one layout, measured once. */
private class ChartDecor(
    val labels: List<LabelAt>,
    val xTicks: List<Double>,
    val splits: List<Double>,
    val sectorCentres: List<Pair<Int, Float>>,
    val axisY: Float,
)

private fun buildDecor(
    input: AnalysisChartInput,
    layout: ChartLayout,
    width: Float,
    height: Float,
    dp: Float,
    /** Measures a label in the axis style, in [Color] when given. */
    measure: (String, Color?) -> TextLayoutResult,
): ChartDecor {
    val geometry = layout.geometry
    val left = geometry.left
    val right = geometry.right
    val top = geometry.top
    val bottom = geometry.bottom
    val domainMin = geometry.domainMin
    val domainMax = geometry.domainMax
    val full = input.fullRange
    val axisSlot = 38f * dp
    val deltaRange = input.delta?.range ?: 0.5
    val labels = mutableListOf<LabelAt>()

    fun axisValues(item: AnalysisSeries): (Float) -> String =
        if (item.metricId == AnalysisCatalog.DELTA_ID) {
            { t -> String.format(Locale.US, "%+.1f", (t - 0.5) * 2 * deltaRange) }
        } else {
            val metric = AnalysisCatalog.scaleMetric(item.metricId)
            if (metric == null) { _ -> "" } else { t ->
                metric.axisFormat(metric.min + t * (metric.max - metric.min))
            }
        }
    if (input.stacked) {
        // Max and min inside each panel's top and bottom left corners.
        for (panel in geometry.panels) {
            val item = panel.item ?: continue
            if (!item.showYAxis) continue
            val format = axisValues(item)
            val maxLabel = measure(format(1f), null)
            val minLabel = measure(format(0f), null)
            labels += LabelAt(maxLabel, Offset(left + 2f * dp, panel.top), backdrop = true)
            labels += LabelAt(minLabel, Offset(left + 2f * dp, panel.bottom - minLabel.size.height), backdrop = true)
        }
    } else {
        layout.axisItems.forEachIndexed { index, item ->
            val onLeft = index % 2 == 0
            val slot = index / 2
            val color = if (item.metricId == AnalysisCatalog.DELTA_ID) input.deltaPositive else Color(item.color)
            val format = axisValues(item)
            for (t in YTicks) {
                val text = measure(format(t), color)
                val y = bottom - t * (bottom - top)
                val x = if (onLeft) left - slot * axisSlot - text.size.width - 4f * dp else right + slot * axisSlot + 4f * dp
                labels += LabelAt(text, Offset(x, (y - text.size.height / 2f).clampTo(top - 2f * dp, bottom - text.size.height / 2f)))
            }
        }
    }

    // X axis: sector names between the splits, otherwise distance or time ticks.
    val splits = if (input.sectorBoundaries) {
        resolvedSectorSplits(input.current, input.comparison).map { if (input.distanceMode) it.distance else it.elapsedSeconds }
    } else {
        emptyList()
    }
    val sectorCentres = mutableListOf<Pair<Int, Float>>()
    val xTicks = mutableListOf<Double>()
    val axisY = height - X_AXIS_DP * dp + 3f * dp
    if (input.sectorBoundaries) {
        val bounds = listOf(0.0) + splits + full
        for (sector in 0 until bounds.size - 1) {
            val from = max(bounds[sector], domainMin)
            val to = min(bounds[sector + 1], domainMax)
            if (to <= from) continue
            sectorCentres += sector to geometry.xToPx((from + to) / 2)
        }
    } else {
        val span = domainMax - domainMin
        val step = niceStep(span, max(2, ((right - left) / (72f * dp)).toInt()), input.distanceMode)
        var tick = ceil(domainMin / step) * step
        while (tick <= domainMax + 1e-9) {
            xTicks += tick
            val text = measure(if (input.distanceMode) "${tick.roundToInt()} m" else formatElapsed(tick, step), null)
            val x = geometry.xToPx(tick) - text.size.width / 2f
            labels += LabelAt(text, Offset(x.clampTo(0f, width - text.size.width), axisY))
            tick += step
        }
    }
    return ChartDecor(labels, xTicks, splits, sectorCentres, axisY)
}

private fun DrawScope.drawGrid(
    decor: ChartDecor,
    geometry: ChartGeometry,
    stacked: Boolean,
    gridColor: Color,
    axisColor: Color,
    dp: Float,
) {
    val ticks = if (stacked) floatArrayOf(0f, 0.5f, 1f) else YTicks
    for (panel in geometry.panels) {
        for (t in ticks) {
            val y = panel.bottom - t * (panel.bottom - panel.top)
            drawLine(gridColor, Offset(geometry.left, y), Offset(geometry.right, y), 1f)
        }
    }
    for (tick in decor.xTicks) {
        val x = geometry.xToPx(tick)
        drawLine(gridColor, Offset(x, geometry.top), Offset(x, geometry.bottom), 1f)
    }
    val dash = PathEffect.dashPathEffect(floatArrayOf(4f * dp, 4f * dp))
    for (split in decor.splits) {
        if (split <= geometry.domainMin || split >= geometry.domainMax) continue
        val x = geometry.xToPx(split)
        drawLine(axisColor.copy(alpha = 0.6f), Offset(x, geometry.top), Offset(x, geometry.bottom), 1f * dp, pathEffect = dash)
    }
}

internal fun deltaColor(value: Double?, neutral: Color, positive: Color, negative: Color): Color = when {
    value == null || !value.isFinite() || abs(value) < 0.0005 -> neutral
    value > 0 -> positive
    else -> negative
}

/** One run of a panel header's value text. */
@Immutable
private data class HeaderRun(val text: String, val color: Color?, val muted: Boolean = false)

/**
 * Each panel's title row: a colour dot, the metric's name, and its values
 * where the chart is being read. These replace a floating tooltip, so nothing
 * covers the traces.
 */
@Composable
private fun PanelHeaders(
    input: AnalysisChartInput,
    layout: ChartLayout,
    state: AnalysisChartState,
    cursorOffset: State<Double>,
    mapElapsed: State<Double>?,
) {
    val colors = MaterialTheme.colorScheme
    val density = LocalDensity.current
    // The cursor and map clock move every frame, and re-shaping four lines of
    // text that often is most of a low-end phone's frame. A readout needs no
    // more than ten updates a second; a finger on the graph still updates it
    // on every move.
    val sampledCursor = remember { mutableDoubleStateOf(Double.NaN) }
    val sampledMap = remember { mutableStateOf<Double?>(null) }
    LaunchedEffect(cursorOffset, mapElapsed) {
        while (true) {
            sampledCursor.doubleValue = cursorOffset.value
            sampledMap.value = mapElapsed?.value
            delay(HEADER_SAMPLE_MS)
        }
    }
    val values by remember(input, layout) {
        derivedStateOf(structuralEqualityPolicy()) {
            val cursor = sampledCursor.doubleValue
            val x = inspectedX(input, state.crosshair, cursor, sampledMap.value)
            layout.items.associate { it.metricId to headerRuns(input, it, x, cursor) }
        }
    }
    val labelStyle = MaterialTheme.typography.labelMedium.copy(fontFeatureSettings = "tnum")
    @Composable
    fun Title(item: AnalysisSeries) {
        val color = if (item.metricId == AnalysisCatalog.DELTA_ID) input.deltaPositive else Color(item.color)
        Row(Modifier.staticLayer(), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(8.dp).background(color, CircleShape))
            Text(
                AnalysisCatalog.labelOf(item),
                modifier = Modifier.padding(start = 6.dp),
                style = labelStyle,
                fontWeight = FontWeight.SemiBold,
                color = colors.onSurface,
                maxLines = 1,
            )
        }
    }
    @Composable
    fun Values(runs: List<HeaderRun>?, modifier: Modifier = Modifier, align: TextAlign = TextAlign.Start) {
        if (runs.isNullOrEmpty()) return
        Text(
            buildAnnotatedString {
                for (run in runs) {
                    withStyle(
                        SpanStyle(
                            color = run.color ?: if (run.muted) colors.onSurfaceVariant else colors.onSurface,
                            fontWeight = if (run.muted) FontWeight.Normal else FontWeight.SemiBold,
                        ),
                    ) { append(run.text) }
                }
            },
            modifier = modifier,
            style = labelStyle,
            textAlign = align,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
    val headerHeight = HEADER_DP.dp
    if (input.stacked) {
        for (panel in layout.geometry.panels) {
            val item = panel.item ?: continue
            Row(
                Modifier
                    .offset { IntOffset(0, panel.headerTop.roundToInt()) }
                    .fillMaxWidth()
                    .height(headerHeight)
                    .padding(horizontal = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Title(item)
                // Read while drawing: a new value repaints this line only.
                Readout(
                    runs = {
                        values[item.metricId].orEmpty().map { run ->
                            ReadoutRun(
                                run.text,
                                run.color ?: if (run.muted) colors.onSurfaceVariant else colors.onSurface,
                                bold = !run.muted,
                            )
                        }
                    },
                    style = labelStyle,
                    modifier = Modifier.weight(1f).padding(start = 12.dp),
                    alignEnd = true,
                )
            }
        }
    } else if (layout.items.isNotEmpty()) {
        // One shared plot: a legend row of every metric with its value.
        Row(
            Modifier
                .offset { IntOffset(0, with(density) { 2.dp.roundToPx() }) }
                .fillMaxWidth()
                .height(headerHeight)
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            for (item in layout.items) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Title(item)
                    Values(values[item.metricId], Modifier.padding(start = 6.dp))
                }
            }
        }
    }
}

/** Chart x the headers read: the crosshair, else the map clock or desktop cursor. */
private fun inspectedX(input: AnalysisChartInput, crosshair: Double?, cursorOffset: Double, mapElapsed: Double?): Double? {
    crosshair?.let { return it }
    val lap = input.current ?: return null
    val elapsed = when {
        input.realtime && cursorOffset.isFinite() -> cursorOffset
        !input.realtime && mapElapsed != null -> mapElapsed
        else -> return null
    }.coerceIn(0.0, lap.duration)
    return if (input.distanceMode) lap.progress?.distanceAt(elapsed)?.takeIf { it.isFinite() } else elapsed
}

private fun headerRuns(input: AnalysisChartInput, item: AnalysisSeries, x: Double?, cursorOffset: Double): List<HeaderRun> {
    x ?: return emptyList()
    // The playing lap has not reached samples beyond the desktop cursor yet.
    val currentLimit = if (input.realtime && cursorOffset.isFinite()) {
        if (input.distanceMode) input.current?.progress?.distanceAt(cursorOffset) ?: Double.NEGATIVE_INFINITY else cursorOffset
    } else {
        Double.POSITIVE_INFINITY
    }
    val currentLap = input.current?.takeIf { x <= currentLimit + 1e-6 }
    if (item.metricId == AnalysisCatalog.DELTA_ID) {
        val delta = input.delta ?: return emptyList()
        if (currentLap == null) return emptyList()
        var best = -1
        var bestDistance = Double.POSITIVE_INFINITY
        for (index in 0 until delta.size) {
            if (!delta.valid[index]) continue
            val distance = abs(delta.distance[index] - x)
            if (distance < bestDistance) { bestDistance = distance; best = index }
        }
        if (best < 0) return emptyList()
        val value = delta.delta[best]
        return listOf(HeaderRun("${formatDelta(value)} s", deltaColor(value, Color.Unspecified, input.deltaPositive, input.deltaNegative).takeIf { it != Color.Unspecified }))
    }
    fun valueOf(lap: AnalysisLap?, memberId: String): Double? {
        lap ?: return null
        val metric = AnalysisCatalog.metricById[memberId] ?: return null
        val plot = lap.plot(metric, input.distanceMode) ?: return null
        val index = plot.nearestIndex(x)
        if (index < 0) return null
        val value = metric.min + plot.y[index] * (metric.max - metric.min)
        return value.takeIf { it.isFinite() }
    }
    val members = AnalysisCatalog.memberIds(item)
    if (members.size == 1) {
        val metric = AnalysisCatalog.metricById[members[0]] ?: return emptyList()
        val current = valueOf(currentLap, members[0])
        val comparison = valueOf(input.comparison, members[0])
        return buildList {
            add(HeaderRun(current?.let(metric.format) ?: "—", null))
            if (input.comparison != null) add(HeaderRun("  ·  ${comparison?.let(metric.format) ?: "—"}", null, muted = true))
        }
    }
    // A combined tyre card: each corner of the current lap, in its line colour.
    return members.mapIndexedNotNull { index, memberId ->
        val metric = AnalysisCatalog.metricById[memberId] ?: return@mapIndexedNotNull null
        val corner = AnalysisCatalog.cornerKeyOf(item.metricId, memberId)
            ?.let { key -> AnalysisCatalog.corners.firstOrNull { it.key == key }?.label } ?: ""
        val value = valueOf(currentLap, memberId)
        HeaderRun(
            "${if (index > 0) "  " else ""}$corner ${value?.let(metric.axisFormat) ?: "—"}",
            Color(AnalysisCatalog.lineColor(item, memberId)),
        )
    }
}

/** Where the playing lap stops being drawn: the desktop cursor, in whole pixels. */
private fun cutoffPixel(input: AnalysisChartInput, geometry: ChartGeometry, width: Float, offset: Double): Int {
    if (!input.realtime) return width.roundToInt()
    val progress = input.current?.progress
    val cutoff = when {
        !offset.isFinite() -> return geometry.left.roundToInt()
        !input.distanceMode -> offset
        progress == null -> return geometry.left.roundToInt()
        offset >= progress.maxTime -> return width.roundToInt()
        else -> progress.distanceAt(offset).takeIf { it.isFinite() } ?: return geometry.left.roundToInt()
    }
    return geometry.xToPx(cutoff).coerceIn(geometry.left, width).roundToInt()
}

/** Seconds into the current lap at chart position [x], for the map. */
private fun inspectElapsed(input: AnalysisChartInput, x: Double): Double? {
    val lap = input.current ?: return null
    val elapsed = if (input.distanceMode) lap.progress?.timeAtDistance(x) ?: return null else x
    return if (elapsed.isFinite()) elapsed.coerceIn(0.0, lap.duration) else null
}

/** Plot area, panels and axes for one size and x range. */
internal class ChartLayout(
    val geometry: ChartGeometry,
    val items: List<AnalysisSeries>,
    val axisItems: List<AnalysisSeries>,
)

private fun layoutChart(
    input: AnalysisChartInput,
    width: Float,
    height: Float,
    dp: Float,
    domain: Pair<Double, Double>,
): ChartLayout {
    val axisSlot = 38f * dp
    val header = HEADER_DP * dp
    val bottom = height - X_AXIS_DP * dp
    val items = input.panelItems()
    // Combined mode draws one axis per scale, alternating sides. Stacked
    // panels label their own min and max inside the plot.
    val axisItems = mutableListOf<AnalysisSeries>()
    if (!input.stacked) {
        val seenScales = mutableSetOf<String>()
        for (item in items) {
            if (!item.showYAxis) continue
            if (item.metricId == AnalysisCatalog.DELTA_ID) { axisItems += item; continue }
            val scale = AnalysisCatalog.scaleMetric(item.metricId) ?: continue
            if (seenScales.add(scale.scaleKey)) axisItems += item
        }
    }
    val leftCount = if (input.stacked) 0 else (axisItems.size + 1) / 2
    val rightCount = if (input.stacked) 0 else axisItems.size / 2
    val left = max(8f * dp, leftCount * axisSlot)
    val right = width - max(8f * dp, rightCount * axisSlot)
    val gap = PANEL_GAP_DP * dp
    val panels = if (input.stacked && items.isNotEmpty()) {
        val slot = (bottom - gap * (items.size - 1)) / items.size
        items.mapIndexed { index, item ->
            val slotTop = index * (slot + gap)
            ChartPanel(item, slotTop, slotTop + header, slotTop + slot)
        }
    } else {
        listOf(ChartPanel(null, 0f, header + 4f * dp, bottom))
    }
    return ChartLayout(
        ChartGeometry(left, right, panels.first().top, bottom, domain.first, domain.second, panels),
        items,
        axisItems,
    )
}

/**
 * One layout rendered: [base] holds the grid, axis labels and comparison lap,
 * [current] the current lap and the delta. Each is a full-size image the GPU
 * fills every frame, so when the current lap is not cut at the desktop cursor
 * everything is merged into [current] and [base] is null.
 */
internal class ChartRaster(
    val base: ImageBitmap?,
    val current: ImageBitmap,
    val domainMin: Double,
    val domainMax: Double,
)

/**
 * Draws every trace into two bitmaps on a background thread. Tessellating
 * tens of thousands of stroked segments on the GPU every frame is what a
 * low-end phone cannot do; a raster is drawn once per layout and blitted.
 */
private suspend fun rasterizeTraces(
    input: AnalysisChartInput,
    layout: ChartLayout,
    decor: ChartDecor,
    width: Int,
    height: Int,
    density: Density,
    background: Color,
    gridColor: Color,
    axisColor: Color,
): ChartRaster = withContext(Dispatchers.Default) {
    val dp = density.density
    val geometry = layout.geometry
    // The playing lap is cut at the desktop cursor, so it needs its own image.
    val separate = input.realtime
    val currentBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
    val baseBitmap = if (separate) Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888) else null
    val currentCanvas = android.graphics.Canvas(currentBitmap)
    // Merged: grid and comparison are drawn first on the same canvas, beneath.
    val baseCanvas = baseBitmap?.let { android.graphics.Canvas(it) } ?: currentCanvas
    val bitmapSize = Size(width.toFloat(), height.toFloat())
    fun decorate(canvas: android.graphics.Canvas, block: androidx.compose.ui.graphics.drawscope.DrawScope.() -> Unit) {
        CanvasDrawScope().draw(density, LayoutDirection.Ltr, androidx.compose.ui.graphics.Canvas(canvas), bitmapSize, block)
    }
    decorate(baseCanvas) { drawGrid(decor, geometry, input.stacked, gridColor, axisColor, dp) }
    val traceClip = android.graphics.RectF(geometry.left, 0f, geometry.right, height.toFloat())
    val baseSave = baseCanvas.save()
    baseCanvas.clipRect(traceClip)
    val currentSave = if (baseCanvas !== currentCanvas) currentCanvas.save().also { currentCanvas.clipRect(traceClip) } else baseSave
    val comparisonCanvas = baseCanvas
    val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    // Last-selected first, so the first card stays on top. Comparison traces
    // go in a first pass so that, merged, they sit beneath every current trace.
    for (pass in 0..1) for (panel in geometry.panels) {
        val panelItems = if (panel.item != null) listOf(panel.item) else layout.items
        for (item in panelItems.reversed()) {
            ensureActive()
            if (item.metricId == AnalysisCatalog.DELTA_ID) {
                if (pass == 0) continue
                val delta = input.delta ?: continue
                val (positive, negative) = deltaPaths(delta, geometry, panel)
                paint.strokeWidth = 2f * dp
                paint.color = input.deltaPositive.toArgb()
                currentCanvas.drawPath(positive, paint)
                paint.color = input.deltaNegative.toArgb()
                currentCanvas.drawPath(negative, paint)
                continue
            }
            for (memberId in AnalysisCatalog.memberIds(item)) {
                val metric = AnalysisCatalog.metricById[memberId] ?: continue
                val color = Color(AnalysisCatalog.lineColor(item, memberId))
                if (pass == 0) input.comparison?.plot(metric, input.distanceMode)?.let { plot ->
                    paint.strokeWidth = 1.25f * dp
                    paint.color = lerp(background, color, 0.42f).toArgb()
                    comparisonCanvas.drawPath(tracePath(plot.x, plot.y, metric.step, geometry, panel), paint)
                }
                ensureActive()
                if (pass == 1) input.current?.plot(metric, input.distanceMode)?.let { plot ->
                    paint.strokeWidth = 1.75f * dp
                    paint.color = color.toArgb()
                    currentCanvas.drawPath(tracePath(plot.x, plot.y, metric.step, geometry, panel), paint)
                }
            }
        }
    }
    baseCanvas.restoreToCount(baseSave)
    if (currentCanvas !== baseCanvas) currentCanvas.restoreToCount(currentSave)
    // Axis labels over the traces they describe (over the comparison only
    // while the current lap is cut separately).
    decorate(baseCanvas) {
        val backdrop = background.copy(alpha = 0.75f)
        val corner = CornerRadius(3f * dp)
        for (label in decor.labels) {
            if (label.backdrop) {
                drawRoundRect(
                    backdrop,
                    label.topLeft - Offset(2f * dp, 0f),
                    Size(label.layout.size.width + 4f * dp, label.layout.size.height.toFloat()),
                    corner,
                )
            }
            drawText(label.layout, topLeft = label.topLeft)
        }
    }
    // Hand the frames GPU-resident copies: a software bitmap is a texture
    // upload the render thread would otherwise have to manage every frame.
    ChartRaster(
        baseBitmap?.toHardware()?.asImageBitmap(),
        currentBitmap.toHardware().asImageBitmap(),
        geometry.domainMin,
        geometry.domainMax,
    )
}

/** An immutable GPU copy, freeing the software pixels; the original if the copy fails. */
private fun Bitmap.toHardware(): Bitmap {
    val hardware = copy(Bitmap.Config.HARDWARE, false) ?: return this
    recycle()
    return hardware
}

private fun tracePath(
    xs: FloatArray,
    ys: FloatArray,
    step: Boolean,
    geometry: ChartGeometry,
    panel: ChartPanel,
): android.graphics.Path {
    val path = android.graphics.Path()
    if (xs.isEmpty()) return path
    // One sample either side of the visible range keeps lines running to the edges.
    val start = (lowerBound(xs, geometry.domainMin) - 1).coerceAtLeast(0)
    val end = (lowerBound(xs, geometry.domainMax) + 1).coerceAtMost(xs.size)
    val height = panel.bottom - panel.top
    var open = false
    var previousY = 0f
    for (index in start until end) {
        val y = ys[index]
        if (y.isNaN()) {
            open = false
            continue
        }
        val px = geometry.xToPx(xs[index].toDouble())
        val py = panel.bottom - y.coerceIn(-0.05f, 1.05f) * height
        if (!open) {
            path.moveTo(px, py)
            open = true
        } else {
            if (step) path.lineTo(px, previousY)
            path.lineTo(px, py)
        }
        previousY = py
    }
    return path
}

/** Positive and negative delta traces, split where the curve crosses zero. */
private fun deltaPaths(
    delta: AnalysisDeltaCurve,
    geometry: ChartGeometry,
    panel: ChartPanel,
): Pair<android.graphics.Path, android.graphics.Path> {
    val positive = android.graphics.Path()
    val negative = android.graphics.Path()
    val range = delta.range
    val height = panel.bottom - panel.top
    fun yOf(seconds: Double) = panel.bottom - (0.5 + seconds / (2 * range)).toFloat().coerceIn(-0.05f, 1.05f) * height
    var previousDistance = Double.NaN
    var previousDelta = Double.NaN
    for (index in 0 until delta.size) {
        val distance = delta.distance[index]
        val value = delta.delta[index]
        if (!delta.valid[index] || !value.isFinite()) {
            previousDistance = Double.NaN
            continue
        }
        val px = geometry.xToPx(distance)
        if (previousDistance.isNaN()) {
            (if (value >= 0) positive else negative).moveTo(px, yOf(value))
        } else {
            val prevPositive = previousDelta >= 0
            val nowPositive = value >= 0
            if (prevPositive == nowPositive) {
                (if (nowPositive) positive else negative).lineTo(px, yOf(value))
            } else {
                val zero = previousDistance + (distance - previousDistance) *
                    abs(previousDelta) / (abs(previousDelta) + abs(value))
                val zeroPx = geometry.xToPx(zero)
                (if (prevPositive) positive else negative).lineTo(zeroPx, yOf(0.0))
                val next = if (nowPositive) positive else negative
                next.moveTo(zeroPx, yOf(0.0))
                next.lineTo(px, yOf(value))
            }
        }
        previousDistance = distance
        previousDelta = value
    }
    return positive to negative
}

/** coerceIn that tolerates an empty range (tiny panels), keeping the lower bound. */
private fun Float.clampTo(low: Float, high: Float): Float = if (high < low) low else coerceIn(low, high)

private fun lowerBound(values: FloatArray, target: Double): Int {
    var lo = 0
    var hi = values.size
    while (lo < hi) {
        val mid = (lo + hi) ushr 1
        if (values[mid] < target) lo = mid + 1 else hi = mid
    }
    return lo
}

private fun niceStep(span: Double, targetCount: Int, metres: Boolean): Double {
    val raw = span / targetCount.coerceAtLeast(1)
    if (!metres && raw >= 10) {
        // Laps read best in whole 10 s, 15 s, 30 s or minute steps.
        return listOf(10.0, 15.0, 20.0, 30.0, 60.0, 120.0).firstOrNull { it >= raw } ?: 120.0
    }
    val magnitude = 10.0.pow(floor(log10(raw)))
    val normalized = raw / magnitude
    val nice = when {
        normalized < 1.5 -> 1.0
        normalized < 3.5 -> 2.0
        normalized < 7.5 -> 5.0
        else -> 10.0
    }
    return nice * magnitude
}

internal fun formatElapsed(seconds: Double, step: Double = 1.0): String {
    val minutes = floor(seconds / 60).toInt()
    val rest = seconds - minutes * 60
    return if (step < 1) {
        String.format(Locale.US, "%d:%04.1f", minutes, rest)
    } else {
        String.format(Locale.US, "%d:%02d", minutes, rest.roundToInt().coerceAtMost(59))
    }
}
