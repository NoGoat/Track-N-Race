package com.tracknrace.android.pages

import android.graphics.Paint
import android.graphics.Typeface
import android.os.Build
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.isSpecified

/**
 * Renders static content into a texture once, so the frames that redraw a
 * moving cursor composite one image instead of re-rasterising every glyph of
 * the labels, tabs and titles around it. Only for content that rarely changes:
 * each change costs an extra render pass.
 */
internal fun Modifier.staticLayer(): Modifier = graphicsLayer(compositingStrategy = CompositingStrategy.Offscreen)

/**
 * A paint for text drawn straight onto a canvas. No 'tnum': Roboto's digits
 * are tabular already, and the feature adds a substitution pass to every
 * shape of a changing value.
 */
internal fun textPaint(sizePx: Float, weight: Int, monospace: Boolean = false): Paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    textSize = sizePx
    val family = if (monospace) Typeface.MONOSPACE else Typeface.DEFAULT
    typeface = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        Typeface.create(family, weight, false)
    } else {
        Typeface.create(family, if (weight >= 600) Typeface.BOLD else Typeface.NORMAL)
    }
}

/** One run of a readout: its text, colour, and whether it is set in the bold weight. */
@Immutable
internal data class ReadoutRun(
    val text: String,
    val color: Color,
    val bold: Boolean = true,
    /** Size relative to the readout's style, for a unit beside its number. */
    val scale: Float = 1f,
)

/**
 * A single line of live numbers drawn straight onto the canvas. A Compose
 * Text re-measures, re-breaks and re-shapes its whole paragraph whenever its
 * string changes, which on a low-end phone costs several milliseconds a time;
 * these values change many times a second, so they are read only while
 * drawing and painted with a ready-made [Paint]. A change redraws this line
 * and nothing else.
 */
@Composable
internal fun Readout(
    runs: () -> List<ReadoutRun>,
    style: TextStyle,
    modifier: Modifier = Modifier,
    alignEnd: Boolean = false,
    center: Boolean = false,
) {
    val density = LocalDensity.current
    val sizePx = with(density) { style.fontSize.toPx() }
    val paints = remember(sizePx, style.fontWeight, style.fontFamily) { readoutPaints(style, sizePx) }
    val lineHeight = with(density) {
        if (style.lineHeight.isSpecified) style.lineHeight.toDp() else (sizePx * 1.35f).toDp()
    }
    Spacer(
        // Its own layer, so a new value repaints this line and nothing around it.
        modifier.height(lineHeight).graphicsLayer().drawBehind {
            paintReadout(runs(), paints, sizePx, Rect(Offset.Zero, size), alignEnd, center)
        },
    )
}

/** Bold and regular paints for a readout in [style], at [sizePx]. */
internal fun readoutPaints(style: TextStyle, sizePx: Float): Pair<Paint, Paint> {
    val monospace = style.fontFamily == FontFamily.Monospace
    val bold = (style.fontWeight ?: FontWeight.SemiBold).weight.coerceAtLeast(600)
    return textPaint(sizePx, bold, monospace) to textPaint(sizePx, FontWeight.Normal.weight, monospace)
}

/**
 * Paints one readout line of [list] into [bounds], aligned to its end, its
 * centre or its start; too wide for it, it shrinks about the aligned edge
 * rather than spill over a neighbour.
 */
internal fun DrawScope.paintReadout(
    list: List<ReadoutRun>,
    paints: Pair<Paint, Paint>,
    sizePx: Float,
    bounds: Rect,
    alignEnd: Boolean,
    center: Boolean = false,
) {
    if (list.isEmpty() || bounds.width <= 0f) return
    val (bold, regular) = paints
    fun paintOf(run: ReadoutRun): Paint = (if (run.bold) bold else regular).also { it.textSize = sizePx * run.scale }
    val widths = FloatArray(list.size) { paintOf(list[it]).measureText(list[it].text) }
    val total = widths.sum()
    var x = bounds.left + when {
        alignEnd -> bounds.width - total
        center -> (bounds.width - total) / 2f
        else -> 0f
    }
    bold.textSize = sizePx
    val metrics = bold.fontMetrics
    val middle = bounds.top + bounds.height / 2f
    val baseline = middle - (metrics.ascent + metrics.descent) / 2f
    val canvas = drawContext.canvas.nativeCanvas
    val save = canvas.save()
    if (total > bounds.width && total > 0f) {
        val scale = bounds.width / total
        val pivot = when {
            alignEnd -> bounds.right
            center -> bounds.left + bounds.width / 2f
            else -> bounds.left
        }
        canvas.scale(scale, scale, pivot, middle)
        x = if (alignEnd) bounds.right - total else if (center) pivot - total / 2f else bounds.left
    }
    list.forEachIndexed { index, run ->
        val paint = paintOf(run)
        paint.color = run.color.toArgb()
        canvas.drawText(run.text, x, baseline, paint)
        x += widths[index]
    }
    canvas.restoreToCount(save)
}
