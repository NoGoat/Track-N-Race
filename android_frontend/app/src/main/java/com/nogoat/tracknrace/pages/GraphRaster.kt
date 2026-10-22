package com.nogoat.tracknrace.pages

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.graphics.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import com.nogoat.tracknrace.GraphField
import com.nogoat.tracknrace.LapTrace
import com.nogoat.tracknrace.SampleSeries
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

/**
 * How a line is drawn, as the desktop's series options: straight between
 * samples or held as a step, its width, and an optional translucent fill down
 * (or up) to a baseline.
 */
internal class LineStyle(
    /** Null for a straight line; else where the step falls between samples, 0 at the earlier, 1 at the later. */
    val step: Float? = null,
    val widthDp: Float = 1.5f,
    /** The value the fill reaches to, or null for no fill. */
    val fillBaseline: Double? = null,
)

/** One line of a raster: which field, how it is scaled, its colour and style. */
internal class RasterLine(val field: GraphField, val scale: Float, val color: Color, val style: LineStyle)

/** The desktop's fill opacity under a line. */
internal const val FILL_ALPHA = 0.2f

/** The desktop draws a comparison lap at this opacity. */
internal const val COMPARISON_ALPHA = 0.35f

/** A drop in lap distance larger than this is the start line, not noise. */
internal const val LAP_WRAP_METRES = 100f

/**
 * Builds one line's path, and its fill when it has one, a sample at a time.
 * A gap ends a run: the line starts again and the fill is closed to its baseline.
 */
internal class TracePaths {
    val line = android.graphics.Path()
    val fill = android.graphics.Path()
    private var style = LineStyle()
    private var baseY = 0f
    private var running = false
    private var lastX = 0f
    private var lastY = 0f

    fun reset(style: LineStyle, baseY: Float) {
        line.rewind()
        fill.rewind()
        this.style = style
        this.baseY = baseY
        running = false
    }

    /** Adds a sample at ([x], [y]); [joined] false starts a new run. */
    fun add(x: Float, y: Float, joined: Boolean) {
        val filled = style.fillBaseline != null
        if (!joined || !running) {
            closeRun()
            line.moveTo(x, y)
            if (filled) {
                fill.moveTo(x, baseY)
                fill.lineTo(x, y)
            }
            running = true
        } else {
            val step = style.step
            if (step != null) {
                val stepX = lastX + (x - lastX) * step
                line.lineTo(stepX, lastY)
                line.lineTo(stepX, y)
                if (filled) {
                    fill.lineTo(stepX, lastY)
                    fill.lineTo(stepX, y)
                }
            }
            line.lineTo(x, y)
            if (filled) fill.lineTo(x, y)
        }
        lastX = x
        lastY = y
    }

    /** Closes the last run's fill; call before drawing. */
    fun finish() = closeRun()

    private fun closeRun() {
        if (running && style.fillBaseline != null) {
            fill.lineTo(lastX, baseY)
            fill.close()
        }
        running = false
    }
}

/**
 * The two bitmaps a chart draws into: the one on screen is never drawn over.
 * Adding to a drawing starts from the one on screen, but only the strip it
 * changed since the other was last brought up to date is copied, not the
 * whole bitmap. Background thread, one call at a time.
 */
internal class BitmapPair {
    private val bitmaps = arrayOfNulls<Bitmap>(2)
    // The drawing each bitmap holds, and the x of the last sample drawn into it.
    private val frames = LongArray(2) { -1L }
    private val lastX = FloatArray(2)
    private var nextFrame = 0L
    private val copyPaint = Paint().apply { xfermode = PorterDuffXfermode(PorterDuff.Mode.SRC) }
    private val source = Rect()

    /** A cleared bitmap off screen for a new drawing; returns its slot. */
    fun fresh(width: Int, height: Int, shown: Bitmap?): Int {
        val slot = offScreen(shown)
        val bitmap = ensure(slot, width, height)
        bitmap.eraseColor(android.graphics.Color.TRANSPARENT)
        frames[slot] = nextFrame++
        lastX[slot] = 0f
        return slot
    }

    /**
     * The bitmap off screen brought up to [shown]'s drawing, to add to, or -1
     * when [shown] is not one of these. [margin] covers a line's width and
     * anti-aliasing either side of the strip.
     */
    fun continuing(shown: Bitmap, margin: Float): Int {
        val shownSlot = bitmaps.indexOfFirst { it === shown }
        if (shownSlot < 0) return -1
        val slot = 1 - shownSlot
        val existing = bitmaps[slot]
        val sameDrawing = existing != null && existing.width == shown.width && existing.height == shown.height &&
            frames[slot] == frames[shownSlot]
        val bitmap = ensure(slot, shown.width, shown.height)
        val from = floor(lastX[slot] - margin).toInt().coerceIn(0, shown.width)
        val to = ceil(lastX[shownSlot] + margin).toInt().coerceIn(0, shown.width)
        if (sameDrawing && from <= to) {
            // Everything left of what this bitmap last drew is already the same.
            source.set(from, 0, to, shown.height)
            Canvas(bitmap).drawBitmap(shown, source, source, copyPaint)
        } else {
            Canvas(bitmap).drawBitmap(shown, 0f, 0f, copyPaint)
        }
        frames[slot] = frames[shownSlot]
        lastX[slot] = lastX[shownSlot]
        return slot
    }

    fun bitmap(slot: Int): Bitmap = bitmaps[slot]!!

    /** The last sample drawn into [slot] is at [x]. */
    fun drew(slot: Int, x: Float) {
        lastX[slot] = x
    }

    private fun offScreen(shown: Bitmap?): Int = if (bitmaps[0] != null && bitmaps[0] === shown) 1 else 0

    private fun ensure(slot: Int, width: Int, height: Int): Bitmap {
        val existing = bitmaps[slot]
        if (existing != null && existing.width == width && existing.height == height) return existing
        frames[slot] = -1L
        return Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888).also { bitmaps[slot] = it }
    }
}

/** Strokes and fills [TracePaths] onto a canvas in a line's colour and width. */
private class LinePainter(private val density: Float) {
    val paths = TracePaths()
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeJoin = Paint.Join.ROUND
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }

    fun paint(canvas: Canvas, spec: RasterLine, alpha: Float = 1f, widthDp: Float = spec.style.widthDp) {
        paths.finish()
        if (spec.style.fillBaseline != null) {
            fill.color = spec.color.copy(alpha = FILL_ALPHA * alpha).toArgb()
            canvas.drawPath(paths.fill, fill)
        }
        line.color = spec.color.copy(alpha = alpha).toArgb()
        line.strokeWidth = widthDp * density
        canvas.drawPath(paths.line, line)
    }

    /** A line's width and anti-aliasing either side, in pixels. */
    val margin: Float get() = 4f * density
}

private fun yPixel(value: Double, yMin: Double, yMax: Double, height: Float): Float {
    val span = (yMax - yMin).takeIf { it > 0 } ?: 1.0
    return (height - (value.coerceIn(yMin, yMax) - yMin) / span * height).toFloat()
}

private fun maxOrNaN(a: Double, b: Double): Double = when {
    a.isNaN() -> b
    b.isNaN() -> a
    else -> maxOf(a, b)
}

// ── Time and span charts ────────────────────────────────────────────────────

/** A chart's lines drawn into [bitmap] for x [from]..[to] and y [yMin]..[yMax]. */
internal class TraceRaster(
    val bitmap: Bitmap,
    val from: Double,
    val to: Double,
    val yMin: Double,
    val yMax: Double,
    // What was drawn, so newer samples can be added without drawing it again.
    val request: RasterRequest,
    val firstTime: Double,
    val lastTime: Double,
    val drawnCount: Int,
    val drawnMax: Double,
)

/**
 * Everything a raster depends on. A new one is made whenever the data, the
 * span or the plot changes; equal requests need no new raster.
 */
internal data class RasterRequest(
    /** Compared by identity: a different series is a different raster. */
    val series: SampleSeries,
    /** Changes whenever samples are added or dropped. */
    val version: Int,
    /** Changes when the samples are dropped and start over. */
    val epoch: Int,
    /** The stretch of session time the bitmap covers, wider than the plot shows. */
    val from: Double,
    val to: Double,
    val shown: List<Boolean>,
    val width: Int,
    val height: Int,
    val fuelPeak: Float,
) {
    /**
     * Whether a raster made for [other] covers the same span, lines and range
     * inputs. The fuel peak is compared so NaN (no fuel on the chart) matches itself.
     */
    fun sameFrame(other: RasterRequest): Boolean =
        from == other.from && to == other.to && shown == other.shown && width == other.width &&
            height == other.height && fuelPeak.compareTo(other.fuelPeak) == 0 && epoch == other.epoch
}

/**
 * Draws a time chart's lines off the main thread. A race holds a quarter of
 * a million samples per line; building a Path of them on the main thread and
 * stroking it on the render thread took seconds a frame. Here every sample is
 * drawn once: a bitmap covers a stretch of time wider than the plot, and newer
 * samples are added to it rather than redrawing the rest. Calls must not overlap.
 */
internal class TraceRasterizer(density: Float) {
    private val buffers = BitmapPair()
    private val painter = LinePainter(density)

    /** The raster on screen; written on the main thread when one is shown. */
    @Volatile var displayed: TraceRaster? = null

    /**
     * The raster for [request], adding only the newest samples to the one on
     * screen when nothing else changed. Null when [cancelled] stops it first.
     */
    fun render(
        request: RasterRequest,
        series: SampleSeries,
        lines: List<RasterLine>,
        range: (visibleMax: Double, fuelPeak: Float) -> Pair<Double, Double>,
        gapSeconds: Double,
        cancelled: () -> Boolean,
    ): TraceRaster? {
        val first = max(0, series.indexAtOrBefore(request.from))
        val last = min(series.size - 1, series.indexAtOrBefore(request.to) + 1)
        if (last < first) return null
        val previous = displayed
        if (previous != null && previous.request.sameFrame(request) && appendable(previous, series, first, last)) {
            val drawnLast = first + previous.drawnCount - 1
            val max = maxOrNaN(previous.drawnMax, visibleMax(series, lines, request, drawnLast + 1, last))
            val (yMin, yMax) = range(max, request.fuelPeak)
            if (yMin == previous.yMin && yMax == previous.yMax) {
                val slot = buffers.continuing(previous.bitmap, painter.margin)
                if (slot >= 0) {
                    // From the last sample drawn, so the new lines join the old.
                    if (!drawLines(slot, series, lines, request, yMin, yMax, drawnLast, last, gapSeconds, cancelled)) return null
                    return raster(slot, request, yMin, yMax, series, first, last, max)
                }
            }
        }
        val max = visibleMax(series, lines, request, first, last)
        if (cancelled()) return null
        val (yMin, yMax) = range(max, request.fuelPeak)
        val slot = buffers.fresh(request.width, request.height, previous?.bitmap)
        if (!drawLines(slot, series, lines, request, yMin, yMax, first, last, gapSeconds, cancelled)) return null
        return raster(slot, request, yMin, yMax, series, first, last, max)
    }

    private fun raster(slot: Int, request: RasterRequest, yMin: Double, yMax: Double, series: SampleSeries, first: Int, last: Int, max: Double) =
        TraceRaster(buffers.bitmap(slot), request.from, request.to, yMin, yMax, request, series.time(first), series.time(last), last - first + 1, max)

    // The new samples start where the drawn ones did and still hold the last
    // one drawn at the same place: only samples were added after it.
    private fun appendable(previous: TraceRaster, series: SampleSeries, first: Int, last: Int): Boolean {
        val drawnLast = first + previous.drawnCount - 1
        return drawnLast in first..last && series.time(first) == previous.firstTime &&
            series.time(drawnLast) == previous.lastTime
    }

    private fun visibleMax(series: SampleSeries, lines: List<RasterLine>, request: RasterRequest, first: Int, last: Int): Double {
        var visibleMax = Double.NaN
        if (last < first) return visibleMax
        lines.forEachIndexed { line, spec ->
            if (!request.shown[line]) return@forEachIndexed
            val column = series.column(spec.field)
            for (index in first..last) {
                val value = (column[index] * spec.scale).toDouble()
                if (value.isFinite() && (visibleMax.isNaN() || value > visibleMax)) visibleMax = value
            }
        }
        return visibleMax
    }

    private fun drawLines(
        slot: Int,
        series: SampleSeries,
        lines: List<RasterLine>,
        request: RasterRequest,
        yMin: Double,
        yMax: Double,
        first: Int,
        last: Int,
        gapSeconds: Double,
        cancelled: () -> Boolean,
    ): Boolean {
        val xSpan = (request.to - request.from).takeIf { it > 0 } ?: 1.0
        val width = request.width.toFloat()
        val height = request.height.toFloat()
        val canvas = Canvas(buffers.bitmap(slot))
        val times = series.timeColumn()
        var lastX = 0f
        lines.forEachIndexed { line, spec ->
            if (!request.shown[line]) return@forEachIndexed
            val column = series.column(spec.field)
            val baseline = spec.style.fillBaseline
            painter.paths.reset(spec.style, if (baseline == null) 0f else yPixel(baseline, yMin, yMax, height))
            var previousTime = Double.NaN
            var joined = false
            for (index in first..last) {
                if (index and 0x3fff == 0 && cancelled()) return false
                val value = (column[index] * spec.scale).toDouble()
                if (!value.isFinite()) {
                    joined = false
                    continue
                }
                val time = times[index]
                val x = ((time - request.from) / xSpan * width).toFloat()
                painter.paths.add(x, yPixel(value, yMin, yMax, height), joined && time - previousTime <= gapSeconds)
                joined = true
                previousTime = time
                lastX = x
            }
            painter.paint(canvas, spec)
        }
        buffers.drew(slot, lastX)
        return !cancelled()
    }
}

// ── Lap charts ──────────────────────────────────────────────────────────────

/**
 * What a lap chart draws: the current lap up to [currentLast], over the
 * whole [comparison] lap, across [trackLength] metres.
 */
internal class LapRasterRequest(
    val current: LapTrace?,
    val currentLast: Int,
    val comparison: LapTrace?,
    val trackLength: Double,
    val shown: List<Boolean>,
    val width: Int,
    val height: Int,
    val fuelPeak: Float,
) {
    /** Same comparison, scale and lines, and the current lap is the same lap. */
    fun sameFrame(other: LapRasterRequest): Boolean =
        comparison === other.comparison && trackLength == other.trackLength && shown == other.shown &&
            width == other.width && height == other.height && fuelPeak.compareTo(other.fuelPeak) == 0 &&
            current?.lapNumber == other.current?.lapNumber && current?.startSessionTime == other.current?.startSessionTime

    override fun equals(other: Any?): Boolean =
        other is LapRasterRequest && sameFrame(other) && current === other.current && currentLast == other.currentLast

    override fun hashCode(): Int = currentLast * 31 + width
}

/** A lap chart's lines drawn into [bitmap] across the track, y [yMin]..[yMax]. */
internal class LapRaster(
    val bitmap: Bitmap,
    val yMin: Double,
    val yMax: Double,
    val request: LapRasterRequest,
    // How much of the current lap was drawn, so the rest can be added.
    val drawnCount: Int,
    val lastTime: Float,
    val drawnMax: Double,
)

/**
 * Draws a lap chart's lines off the main thread: the comparison lap once,
 * faded, and the current lap over it, added to as the cursor moves along
 * rather than redrawn. Calls must not overlap.
 */
internal class LapRasterizer(private val density: Float) {
    private val buffers = BitmapPair()
    private val painter = LinePainter(density)

    /** The raster on screen; written on the main thread when one is shown. */
    @Volatile var displayed: LapRaster? = null

    fun render(
        request: LapRasterRequest,
        lines: List<RasterLine>,
        range: (visibleMax: Double, fuelPeak: Float) -> Pair<Double, Double>,
        cancelled: () -> Boolean,
    ): LapRaster? {
        val current = request.current
        val last = if (current == null) -1 else request.currentLast
        val previous = displayed
        if (previous != null && current != null && previous.request.sameFrame(request) &&
            previous.drawnCount in 1..(last + 1) && current.times[previous.drawnCount - 1] == previous.lastTime
        ) {
            val from = previous.drawnCount - 1
            val max = maxOrNaN(previous.drawnMax, visibleMax(current, lines, request, from + 1, last))
            val (yMin, yMax) = range(max, request.fuelPeak)
            if (yMin == previous.yMin && yMax == previous.yMax) {
                val slot = buffers.continuing(previous.bitmap, painter.margin)
                if (slot >= 0) {
                    drawLap(slot, current, from, last, lines, request, yMin, yMax, faded = false)
                    if (cancelled()) return null
                    return LapRaster(buffers.bitmap(slot), yMin, yMax, request, last + 1, current.times[last], max)
                }
            }
        }
        var max = Double.NaN
        if (current != null) max = visibleMax(current, lines, request, 0, last)
        request.comparison?.let { max = maxOrNaN(max, visibleMax(it, lines, request, 0, it.size - 1)) }
        if (cancelled()) return null
        val (yMin, yMax) = range(max, request.fuelPeak)
        val slot = buffers.fresh(request.width, request.height, previous?.bitmap)
        request.comparison?.let { drawLap(slot, it, 0, it.size - 1, lines, request, yMin, yMax, faded = true) }
        if (current != null && last >= 0) drawLap(slot, current, 0, last, lines, request, yMin, yMax, faded = false)
        if (cancelled()) return null
        return LapRaster(
            buffers.bitmap(slot), yMin, yMax, request,
            if (current != null) last + 1 else 0,
            if (current != null && last >= 0) current.times[last] else Float.NaN,
            max,
        )
    }

    private fun visibleMax(trace: LapTrace, lines: List<RasterLine>, request: LapRasterRequest, first: Int, last: Int): Double {
        var visibleMax = Double.NaN
        lines.forEachIndexed { line, spec ->
            if (!request.shown[line]) return@forEachIndexed
            for (index in first..last) {
                val value = (trace.value(spec.field, index) * spec.scale).toDouble()
                if (value.isFinite() && (visibleMax.isNaN() || value > visibleMax)) visibleMax = value
            }
        }
        return visibleMax
    }

    private fun drawLap(
        slot: Int,
        trace: LapTrace,
        first: Int,
        last: Int,
        lines: List<RasterLine>,
        request: LapRasterRequest,
        yMin: Double,
        yMax: Double,
        faded: Boolean,
    ) {
        if (last < first) return
        val width = request.width.toFloat()
        val height = request.height.toFloat()
        val length = request.trackLength.takeIf { it > 0 } ?: 1.0
        val canvas = Canvas(buffers.bitmap(slot))
        var lastX = 0f
        lines.forEachIndexed { line, spec ->
            if (!request.shown[line]) return@forEachIndexed
            val baseline = spec.style.fillBaseline
            painter.paths.reset(spec.style, if (baseline == null) 0f else yPixel(baseline, yMin, yMax, height))
            var joined = false
            var previous = Float.NaN
            for (index in first..last) {
                val metres = trace.distance[index]
                val value = (trace.value(spec.field, index) * spec.scale).toDouble()
                if (!value.isFinite() || metres.isNaN()) {
                    joined = false
                    continue
                }
                val x = (metres / length * width).toFloat()
                // A step back in distance is the line crossing back over the start.
                painter.paths.add(x, yPixel(value, yMin, yMax, height), joined && metres >= previous - LAP_WRAP_METRES)
                joined = true
                previous = metres
                lastX = x
            }
            // A comparison lap is faded and a little thinner, as on the desktop.
            if (faded) painter.paint(canvas, spec, COMPARISON_ALPHA, max(1f, spec.style.widthDp - 0.5f))
            else painter.paint(canvas, spec)
        }
        if (!faded) buffers.drew(slot, lastX)
    }
}
