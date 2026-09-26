package com.tracknrace.android.pages

import android.graphics.Bitmap
import android.graphics.Paint
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.tracknrace.android.AnalysisCatalog
import com.tracknrace.android.AnalysisDeltaCurve
import com.tracknrace.android.AnalysisLap
import com.tracknrace.android.AnalysisSeries
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
private const val STACKED_TITLE_SLOT_DP = 18f
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
    val syncedTooltip: Boolean,
    val sectorBoundaries: Boolean,
    /** The current lap is still being played: draw it only up to the desktop cursor. */
    val realtime: Boolean,
    val zoomEnabled: Boolean,
    val currentLabel: String,
    val comparisonLabel: String,
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

    /** Inspected x in chart units and the touch's y in pixels, or null. */
    var crosshair by mutableStateOf<Pair<Double, Float>?>(null)

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

internal class ChartPanel(val item: AnalysisSeries?, val top: Float, val bottom: Float)

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

    fun panelAt(y: Float): ChartPanel? =
        panels.firstOrNull { y >= it.top && y <= it.bottom }
            ?: panels.minByOrNull { min(abs(y - it.top), abs(y - it.bottom)) }
}

/** A measured label; [rotated] labels are turned 90° clockwise about their centre. */
private class LabelAt(val layout: TextLayoutResult, val topLeft: Offset, val rotated: Boolean = false)

/**
 * The Analysis graph: every selected metric of the current lap, and of the
 * comparison lap dimmed beneath it, on one canvas or in one panel each.
 * Values are normalised per metric, so one y axis is drawn per scale.
 */
@Composable
internal fun AnalysisChart(
    input: AnalysisChartInput,
    state: AnalysisChartState,
    /** Seconds from the current lap's start to the desktop cursor; NaN when not following it. */
    cursorOffset: State<Double>,
    /** Seconds into the laps shown by the map clock, for split-view cursors. */
    mapElapsed: State<Double>?,
    onInspect: (Double) -> Unit,
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
    val titleStyle = MaterialTheme.typography.labelMedium.copy(color = axisColor, fontWeight = FontWeight.SemiBold)
    val latestInput by rememberUpdatedState(input)
    val latestInspect by rememberUpdatedState(onInspect)

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
        val height = constraints.maxHeight
        val domain = state.domain(input.fullRange)
        val layout = remember(input, width, height, domain, density) {
            layoutChart(input, width.toFloat(), height.toFloat(), density.density, domain)
        }
        val raster = remember { mutableStateOf<ChartRaster?>(null) }
        LaunchedEffect(layout, background) {
            if (width <= 0 || height <= 0) return@LaunchedEffect
            // A zoom gesture changes the range every frame; let it settle
            // before rendering, and keep showing the stretched previous raster.
            if (raster.value != null) delay(RASTER_SETTLE_MS)
            raster.value = rasterizeTraces(input, layout, width, height, density.density, background)
        }
        // Per-frame inputs reduced to what is drawn: whole pixels. The desktop
        // cursor and map clock move every frame, but the cut they place on the
        // traces moves a pixel only every few frames.
        val cutoff = remember(layout, input.realtime) {
            derivedStateOf { cutoffPixel(input, layout.geometry, width.toFloat(), cursorOffset.value) }
        }
        val mapCursors = remember(layout, mapElapsed) {
            derivedStateOf {
                val elapsed = mapElapsed?.value ?: return@derivedStateOf emptyList()
                listOf(input.current, input.comparison).map { lap ->
                    lap ?: return@map -1
                    val clamped = elapsed.coerceIn(0.0, lap.duration)
                    val x = if (input.distanceMode) lap.progress?.distanceAt(clamped) ?: Double.NaN else clamped
                    if (x.isFinite()) layout.geometry.xToPx(x).roundToInt() else -1
                }
            }
        }
        Box(
            Modifier
                .fillMaxSize()
                .semantics { contentDescription = "Lap analysis graph" }
                .pointerInput(Unit) {
                    var lastTapAt = Long.MIN_VALUE / 2
                    var lastTapX = 0f
                    awaitEachGesture {
                        val down = awaitFirstDown(requireUnconsumed = false)
                        var releasedAt = down.uptimeMillis
                        var transformed = false
                        var moved = false
                        val geometry = state.geometry
                        if (geometry != null) state.crosshair = geometry.pxToX(down.position.x) to down.position.y
                        down.consume()
                        while (true) {
                            val event = awaitPointerEvent()
                            val pressed = event.changes.filter { it.pressed }
                            if (pressed.isEmpty()) {
                                releasedAt = event.changes.maxOf { it.uptimeMillis }
                                event.changes.forEach { it.consume() }
                                break
                            }
                            val current = latestInput
                            val active = state.geometry
                            if (pressed.size >= 2) {
                                transformed = true
                                state.crosshair = null
                                if (current.zoomEnabled && active != null) {
                                    val full = current.fullRange
                                    val minExtent = if (current.distanceMode) MIN_ZOOM_METRES else MIN_ZOOM_SECONDS
                                    val zoom = event.calculateZoom()
                                    val centroid = event.calculateCentroid(useCurrent = true)
                                    val width = (active.right - active.left).coerceAtLeast(1f)
                                    val unitsPerPx = (active.domainMax - active.domainMin) / width
                                    if (zoom > 0f && zoom != 1f) {
                                        state.zoomBy(1.0 / zoom, active.pxToX(centroid.x), full, minExtent)
                                    }
                                    val pan = event.calculatePan()
                                    if (pan.x != 0f) state.panBy(-pan.x * unitsPerPx, full, minExtent)
                                }
                            } else if (!transformed && active != null) {
                                val position = pressed[0].position
                                if ((position - down.position).getDistance() > viewConfiguration.touchSlop) moved = true
                                state.crosshair = active.pxToX(position.x) to position.y
                            }
                            event.changes.forEach { it.consume() }
                        }
                        // A second quick tap near the first opens that point on the map.
                        val tap = !transformed && !moved && releasedAt - down.uptimeMillis <= DOUBLE_TAP_MS
                        if (tap && down.uptimeMillis - lastTapAt <= DOUBLE_TAP_MS &&
                            abs(down.position.x - lastTapX) < 48.dp.toPx()
                        ) {
                            lastTapAt = Long.MIN_VALUE / 2
                            state.geometry?.let { geometry ->
                                inspectElapsed(latestInput, geometry.pxToX(down.position.x))?.let(latestInspect)
                            }
                        } else if (tap) {
                            lastTapAt = releasedAt
                            lastTapX = down.position.x
                        } else {
                            lastTapAt = Long.MIN_VALUE / 2
                        }
                    }
                }
                .drawWithCache {
                    val dp = density.density
                    val axisSlot = 38f * dp
                    val geometry = layout.geometry
                    val left = geometry.left
                    val right = geometry.right
                    val top = geometry.top
                    val bottom = geometry.bottom
                    val domainMin = geometry.domainMin
                    val domainMax = geometry.domainMax
                    val panels = geometry.panels
                    val full = input.fullRange
                    val axisItems = layout.axisItems
                    val deltaRange = input.delta?.range ?: 0.5
                    state.geometry = geometry

                    fun label(text: String, style: TextStyle = labelStyle) = measurer.measure(text, style)
                    val labels = mutableListOf<LabelAt>()

                    // Y axes.
                    fun axisValues(item: AnalysisSeries): (Float) -> String =
                        if (item.metricId == AnalysisCatalog.DELTA_ID) {
                            { t -> String.format(Locale.US, "%+.1f", (t - 0.5) * 2 * deltaRange) }
                        } else {
                            val metric = AnalysisCatalog.scaleMetric(item.metricId)
                            if (metric == null) { _ -> "" } else { t ->
                                metric.axisFormat(metric.min + t * (metric.max - metric.min))
                            }
                        }
                    fun axisColorOf(item: AnalysisSeries) =
                        if (item.metricId == AnalysisCatalog.DELTA_ID) input.deltaPositive else Color(item.color)
                    if (input.stacked) {
                        for (panel in panels) {
                            val item = panel.item ?: continue
                            val color = axisColorOf(item)
                            // The title runs down the strip right of the panel, clear
                            // of the traces, and is cut short to the panel's height.
                            val panelHeight = (panel.bottom - panel.top).roundToInt().coerceAtLeast(1)
                            val title = measurer.measure(
                                AnalysisCatalog.labelOf(item),
                                titleStyle.copy(color = color),
                                overflow = TextOverflow.Ellipsis,
                                maxLines = 1,
                                constraints = Constraints(maxWidth = panelHeight),
                            )
                            val centre = Offset(right + STACKED_TITLE_SLOT_DP * dp / 2f, (panel.top + panel.bottom) / 2f)
                            // A title cut down to a few letters in a thin panel says
                            // nothing; the tooltip still names the metric.
                            if (!title.hasVisualOverflow || panelHeight >= 40f * dp) {
                                labels += LabelAt(
                                    title,
                                    Offset(centre.x - title.size.width / 2f, centre.y - title.size.height / 2f),
                                    rotated = true,
                                )
                            }
                            if (!item.showYAxis) continue
                            val format = axisValues(item)
                            val labelHeight = label("0", labelStyle).size.height.toFloat()
                            for (t in stackedTicks(panel.bottom - panel.top, labelHeight)) {
                                val text = label(format(t), labelStyle.copy(color = color))
                                val y = panel.bottom - t * (panel.bottom - panel.top)
                                labels += LabelAt(
                                    text,
                                    Offset(left - text.size.width - 4f * dp, (y - text.size.height / 2f).clampTo(panel.top - 2f * dp, panel.bottom - text.size.height + 2f * dp)),
                                )
                            }
                        }
                    } else {
                        axisItems.forEachIndexed { index, item ->
                            val onLeft = index % 2 == 0
                            val slot = index / 2
                            val color = axisColorOf(item)
                            val format = axisValues(item)
                            for (t in YTicks) {
                                val text = label(format(t), labelStyle.copy(color = color))
                                val y = bottom - t * (bottom - top)
                                val x = if (onLeft) {
                                    left - slot * axisSlot - text.size.width - 4f * dp
                                } else {
                                    right + slot * axisSlot + 4f * dp
                                }
                                labels += LabelAt(text, Offset(x, (y - text.size.height / 2f).clampTo(0f, bottom - text.size.height / 2f)))
                            }
                        }
                    }

                    // X axis: sector names between the splits, otherwise distance or time ticks.
                    val splits = if (input.sectorBoundaries) {
                        resolvedSectorSplits(input.current, input.comparison).map {
                            if (input.distanceMode) it.distance else it.elapsedSeconds
                        }
                    } else {
                        emptyList()
                    }
                    val xTicks = mutableListOf<Double>()
                    if (input.sectorBoundaries) {
                        val bounds = listOf(0.0) + splits + full
                        for (sector in 0 until bounds.size - 1) {
                            val from = max(bounds[sector], domainMin)
                            val to = min(bounds[sector + 1], domainMax)
                            if (to <= from) continue
                            val text = label("S${sector + 1}", titleStyle)
                            val center = geometry.xToPx((from + to) / 2)
                            labels += LabelAt(text, Offset(center - text.size.width / 2f, bottom + 4f * dp))
                        }
                    } else {
                        val span = domainMax - domainMin
                        val step = niceStep(span, max(2, ((right - left) / (72f * dp)).toInt()), input.distanceMode)
                        var tick = ceil(domainMin / step) * step
                        while (tick <= domainMax + 1e-9) {
                            xTicks += tick
                            val text = label(if (input.distanceMode) "${tick.roundToInt()} m" else formatElapsed(tick, step))
                            val x = geometry.xToPx(tick) - text.size.width / 2f
                            labels += LabelAt(text, Offset(x.clampTo(0f, size.width - text.size.width), bottom + 4f * dp))
                            tick += step
                        }
                    }

                    val dash = PathEffect.dashPathEffect(floatArrayOf(4f * dp, 4f * dp))

                    onDrawBehind {
                        // Grid.
                        for (panel in panels) {
                            val ticks = if (input.stacked) floatArrayOf(0f, 0.5f, 1f) else YTicks
                            for (t in ticks) {
                                val y = panel.bottom - t * (panel.bottom - panel.top)
                                drawLine(gridColor, Offset(left, y), Offset(right, y), 1f)
                            }
                        }
                        for (tick in xTicks) {
                            val x = geometry.xToPx(tick)
                            drawLine(gridColor, Offset(x, top), Offset(x, bottom), 1f)
                        }
                        for (split in splits) {
                            if (split <= domainMin || split >= domainMax) continue
                            val x = geometry.xToPx(split)
                            drawLine(axisColor.copy(alpha = 0.6f), Offset(x, top), Offset(x, bottom), 1f * dp, pathEffect = dash)
                        }

                        // The playing lap ends at the desktop cursor.
                        // The playing lap ends at the desktop cursor. Read as a
                        // whole pixel, so a frame is drawn only when it moves.
                        val cutoffPx = cutoff.value.toFloat()

                        clipRect(left, 0f, right, size.height) {
                            // Traces are rasterised off the main thread; a frame only
                            // blits them. While a new zoom renders, the previous
                            // raster is stretched onto the new range.
                            raster.value?.let { traces ->
                                val span = domainMax - domainMin
                                val scaleX = ((traces.domainMax - traces.domainMin) / span).toFloat()
                                val shift = ((traces.domainMin - domainMin) / span * (right - left)).toFloat()
                                withTransform({
                                    translate(left + shift, 0f)
                                    scale(scaleX, 1f, Offset.Zero)
                                    translate(-left, 0f)
                                }) {
                                    traces.comparison?.let { drawImage(it) }
                                }
                                clipRect(right = cutoffPx) {
                                    withTransform({
                                        translate(left + shift, 0f)
                                        scale(scaleX, 1f, Offset.Zero)
                                        translate(-left, 0f)
                                    }) {
                                        drawImage(traces.current)
                                    }
                                }
                            }
                            val colorsOfLaps = input.mapCursorColors
                            if (colorsOfLaps != null) {
                                mapCursors.value.forEachIndexed { index, px ->
                                    if (px < 0) return@forEachIndexed
                                    drawLine(colorsOfLaps.getOrElse(index) { crosshairColor }, Offset(px.toFloat(), top), Offset(px.toFloat(), bottom), 1.5f * dp)
                                }
                            }
                        }

                        for (label in labels) {
                            if (!label.rotated) {
                                drawText(label.layout, topLeft = label.topLeft)
                                continue
                            }
                            val centre = Offset(
                                label.topLeft.x + label.layout.size.width / 2f,
                                label.topLeft.y + label.layout.size.height / 2f,
                            )
                            rotate(90f, centre) { drawText(label.layout, topLeft = label.topLeft) }
                        }

                        state.crosshair?.let { (x, _) ->
                            if (x < domainMin || x > domainMax) return@let
                            val px = geometry.xToPx(x)
                            drawLine(crosshairColor, Offset(px, top), Offset(px, bottom), 1f * dp)
                        }
                    }
                },
        )

        ChartTooltipLayer(input, state, cursorOffset, constraints.maxWidth)
    }
}

// Reads the crosshair in its own scope, so moving it recomposes only the
// tooltip and never rebuilds the chart's cached traces.
@Composable
private fun ChartTooltipLayer(
    input: AnalysisChartInput,
    state: AnalysisChartState,
    cursorOffset: State<Double>,
    maxWidth: Int,
) {
    val crosshair = state.crosshair ?: return
    val geometry = state.geometry ?: return
    val rows = tooltipRows(input, geometry, crosshair, cursorOffset.value)
    if (rows.isEmpty()) return
    val density = LocalDensity.current
    val onRight = geometry.xToPx(crosshair.first) < maxWidth / 2f
    val widthPx = with(density) { 180.dp.toPx() }
    val margin = with(density) { 8.dp.toPx() }
    val x = if (onRight) maxWidth - widthPx - margin else margin
    AnalysisTooltip(
        rows,
        header = if (input.distanceMode) "${crosshair.first.roundToInt()} m" else formatElapsed(crosshair.first, 0.1),
        modifier = Modifier.offset { IntOffset(x.roundToInt(), 6.dp.roundToPx()) },
    )
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
    val top = 8f * dp
    val bottom = height - 20f * dp
    val items = input.panelItems()
    // Combined mode draws one axis per scale, alternating sides; stacked mode
    // one per panel on the left.
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
    val leftCount = if (input.stacked) {
        if (items.any { it.showYAxis }) 1 else 0
    } else {
        (axisItems.size + 1) / 2
    }
    val rightCount = if (input.stacked) 0 else axisItems.size / 2
    val left = max(8f * dp, leftCount * axisSlot)
    // Stacked panels keep a strip on the right for their rotated titles.
    val right = width - if (input.stacked) STACKED_TITLE_SLOT_DP * dp else max(8f * dp, rightCount * axisSlot)
    val gap = 8f * dp
    val panels = if (input.stacked && items.isNotEmpty()) {
        val panelHeight = (bottom - top - gap * (items.size - 1)) / items.size
        items.mapIndexed { index, item ->
            val panelTop = top + index * (panelHeight + gap)
            ChartPanel(item, panelTop, panelTop + panelHeight)
        }
    } else {
        listOf(ChartPanel(null, top, bottom))
    }
    return ChartLayout(
        ChartGeometry(left, right, top, bottom, domain.first, domain.second, panels),
        items,
        axisItems,
    )
}

/**
 * The traces of one layout: the comparison lap, and the current lap with the
 * delta. Each is a full-width layer the GPU has to fill, so the comparison is
 * null without a comparison lap, and merged into [current] when the current
 * lap is not being cut at the desktop cursor.
 */
internal class ChartRaster(
    val comparison: ImageBitmap?,
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
    width: Int,
    height: Int,
    dp: Float,
    background: Color,
): ChartRaster = withContext(Dispatchers.Default) {
    val geometry = layout.geometry
    val separate = input.realtime && input.comparison != null
    val currentBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
    val comparisonBitmap = if (separate) Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888) else null
    val currentCanvas = android.graphics.Canvas(currentBitmap)
    // Merged: the comparison is drawn first on the same canvas, beneath.
    val comparisonCanvas = comparisonBitmap?.let { android.graphics.Canvas(it) } ?: currentCanvas
    for (canvas in listOf(comparisonCanvas, currentCanvas).distinct()) {
        canvas.clipRect(geometry.left, 0f, geometry.right, height.toFloat())
    }
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
    // Hand the frames GPU-resident copies: a software bitmap is a texture
    // upload the render thread would otherwise have to manage every frame.
    ChartRaster(
        comparisonBitmap?.toHardware()?.asImageBitmap(),
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

/**
 * Which y labels a stacked panel of [height] px has room for: min, mid and max
 * when tall, then min and max, then only the max, then none. With many
 * metrics stacked, three labels per panel would overlap each other and the
 * next panel's.
 */
private fun stackedTicks(height: Float, labelHeight: Float): FloatArray = when {
    height >= labelHeight * 3.5f -> floatArrayOf(0f, 0.5f, 1f)
    height >= labelHeight * 2.2f -> floatArrayOf(0f, 1f)
    height >= labelHeight -> floatArrayOf(1f)
    else -> FloatArray(0)
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

private class TooltipRow(val label: String, val value: String, val color: Color, val heading: Boolean = false)

private fun tooltipRows(
    input: AnalysisChartInput,
    geometry: ChartGeometry,
    crosshair: Pair<Double, Float>,
    cursorOffset: Double,
): List<TooltipRow> {
    val (x, y) = crosshair
    val hovered = if (input.stacked && !input.syncedTooltip) geometry.panelAt(y)?.item?.metricId else null
    val rows = mutableListOf<TooltipRow>()
    // The playing lap has not reached samples beyond the desktop cursor yet.
    val currentLimit = if (input.realtime && cursorOffset.isFinite()) {
        if (input.distanceMode) input.current?.progress?.distanceAt(cursorOffset) ?: Double.NEGATIVE_INFINITY else cursorOffset
    } else {
        Double.POSITIVE_INFINITY
    }
    fun appendRole(lap: AnalysisLap?, heading: String, current: Boolean) {
        lap ?: return
        if (current && x > currentLimit) return
        val roleRows = mutableListOf<TooltipRow>()
        for (item in input.series) {
            if (!item.visible || item.metricId == AnalysisCatalog.DELTA_ID) continue
            if (hovered != null && item.metricId != hovered) continue
            for (memberId in AnalysisCatalog.memberIds(item)) {
                val metric = AnalysisCatalog.metricById[memberId] ?: continue
                val plot = lap.plot(metric, input.distanceMode) ?: continue
                val index = plot.nearestIndex(x)
                val normalized = if (index >= 0) plot.y[index].toDouble() else Double.NaN
                val value = metric.min + normalized * (metric.max - metric.min)
                roleRows += TooltipRow(
                    metric.label,
                    if (value.isFinite()) metric.format(value) else "—",
                    Color(AnalysisCatalog.lineColor(item, memberId)),
                )
            }
        }
        if (roleRows.isNotEmpty()) {
            rows += TooltipRow(heading, "", Color.Unspecified, heading = true)
            rows += roleRows
        }
    }
    appendRole(input.current, "${input.currentLabel} · L${input.current?.lapNumber ?: "—"}", current = true)
    appendRole(input.comparison, "${input.comparisonLabel} · L${input.comparison?.lapNumber ?: "—"}", current = false)
    val delta = input.delta
    if (delta != null && input.showDelta && input.distanceMode && x <= currentLimit &&
        (hovered == null || hovered == AnalysisCatalog.DELTA_ID)
    ) {
        var best = -1
        var bestDistance = Double.POSITIVE_INFINITY
        for (index in 0 until delta.size) {
            if (!delta.valid[index]) continue
            val distance = abs(delta.distance[index] - x)
            if (distance < bestDistance) { bestDistance = distance; best = index }
        }
        if (best >= 0) {
            val value = delta.delta[best]
            rows += TooltipRow(
                "Delta",
                String.format(Locale.US, "%+.3f s", value),
                if (value > 0) input.deltaPositive else input.deltaNegative,
            )
        }
    }
    return rows
}

@Composable
private fun AnalysisTooltip(rows: List<TooltipRow>, header: String, modifier: Modifier = Modifier) {
    // A Material rich tooltip: surface container, medium shape, level 2 shadow.
    Surface(
        modifier = modifier.widthIn(max = 180.dp),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainer,
        shadowElevation = 3.dp,
    ) {
        Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
            Text(
                header,
                style = MaterialTheme.typography.titleSmall.copy(fontFeatureSettings = "tnum"),
                color = MaterialTheme.colorScheme.onSurface,
            )
            for (row in rows) {
                if (row.heading) {
                    Text(
                        row.label,
                        modifier = Modifier.padding(top = 6.dp),
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                    )
                    continue
                }
                Row(Modifier.padding(top = 2.dp), verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(8.dp).background(row.color, CircleShape))
                    Text(
                        row.label,
                        modifier = Modifier.weight(1f).padding(start = 6.dp),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                    )
                    Text(
                        row.value,
                        modifier = Modifier.padding(start = 8.dp),
                        style = MaterialTheme.typography.bodySmall.copy(fontFeatureSettings = "tnum"),
                        color = MaterialTheme.colorScheme.onSurface,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                    )
                }
            }
        }
    }
}
