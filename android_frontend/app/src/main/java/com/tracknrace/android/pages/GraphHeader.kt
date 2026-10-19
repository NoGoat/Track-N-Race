package com.tracknrace.android.pages

import android.graphics.Paint
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.isSpecified
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.tracknrace.android.GraphWindow
import com.tracknrace.android.R
import kotlin.math.max

/** A chart header's height: one row that never wraps, so every plot on a tab lines up. */
internal const val HEADER_HEIGHT_DP = 30
/** Below this width a header's window pill shows the window's short id. */
internal const val NARROW_HEADER_DP = 300
/** A token's name, relative to its value. */
internal const val TOKEN_LABEL_SCALE = 0.64f
private const val UNIT_SCALE = 0.7f
private const val TOKEN_GAP_DP = 10f
private const val UNIT_GAP_DP = 1.5f
private const val DETAIL_GAP_DP = 3f
private const val LINE_GAP_DP = 3.5f
private const val RULE_INSET_DP = 5f
private const val LABEL_TRACKING = 0.06f
private const val HIDDEN_ALPHA = 0.4f
// Shrinking the tokens this far keeps their names; any further drops them first.
private const val MIN_TOKEN_SCALE = 0.85f

/**
 * One value in a chart's header: the number in its series' colour, with the
 * series' name in small caps under it. It is the series' legend entry too:
 * tapping it hides the series' line.
 */
@Immutable
internal data class HeaderToken(
    val value: String,
    /** Unspecified for the header's text colour, as a total's. */
    val color: Color,
    /** Drawn as given, in capitals; null for a chart's only value, which its title already names. */
    val name: String? = null,
    /** Smaller, after the value. */
    val unit: String? = null,
    /** After the name: a comparison lap's value, or context such as ERS Store's energy. */
    val detail: String? = null,
    /** Unspecified for the name's colour. */
    val detailColor: Color = Color.Unspecified,
    /** The series label a tap on this token hides; null for one that hides nothing. */
    val key: String? = null,
    /** Set apart from the token before it by a hairline, as a total. */
    val separated: Boolean = false,
    val hidden: Boolean = false,
)

/** The paints a header's tokens are drawn with: made once per text size. */
internal class HeaderPaints(valuePx: Float, labelPx: Float, density: Float) {
    val value: Paint = textPaint(valuePx, FontWeight.SemiBold.weight)
    val unit: Paint = textPaint(valuePx * UNIT_SCALE, FontWeight.Normal.weight)
    val label: Paint = textPaint(labelPx, FontWeight.Medium.weight).apply { letterSpacing = LABEL_TRACKING }
    val detail: Paint = textPaint(labelPx, FontWeight.SemiBold.weight)
    val valueCap = capHeight(value)
    val labelCap = capHeight(label)
    val lineGap = LINE_GAP_DP * density
}

/** The theme colours a header's tokens are drawn in. */
internal class HeaderColors(val text: Color, val unit: Color, val label: Color, val rule: Color)

private fun capHeight(paint: Paint): Float {
    val bounds = android.graphics.Rect()
    paint.getTextBounds("8", 0, 1, bounds)
    return bounds.height().toFloat()
}

/**
 * Where each token was last painted, across its slot, so a tap can find the
 * series under it. Written while drawing, read on a tap; main thread only.
 */
internal class TokenHits {
    private var starts = FloatArray(8)
    private var ends = FloatArray(8)
    private var keys = arrayOfNulls<String>(8)
    private var size = 0

    fun clear() {
        size = 0
    }

    fun add(start: Float, end: Float, key: String?) {
        if (size == starts.size) {
            starts = starts.copyOf(size * 2)
            ends = ends.copyOf(size * 2)
            keys = keys.copyOf(size * 2)
        }
        starts[size] = start
        ends[size] = end
        keys[size] = key
        size++
    }

    fun keyAt(x: Float): String? {
        for (i in 0 until size) if (x >= starts[i] && x <= ends[i]) return keys[i]
        return null
    }
}

private fun lineWidth(token: HeaderToken, paints: HeaderPaints, detailGap: Float): Float {
    val name = token.name?.let { paints.label.measureText(it) } ?: 0f
    val detail = token.detail?.let { paints.detail.measureText(it) } ?: 0f
    return name + detail + if (name > 0f && detail > 0f) detailGap else 0f
}

/**
 * Paints [tokens] into [bounds], against its end, each a value over its name.
 * Too wide, they shrink a little, then drop their names, then shrink again,
 * rather than wrap or spill over the title. Records where each landed in [hits].
 */
internal fun DrawScope.paintTokens(
    tokens: List<HeaderToken>,
    paints: HeaderPaints,
    colors: HeaderColors,
    bounds: Rect,
    hits: TokenHits,
) {
    hits.clear()
    if (tokens.isEmpty() || bounds.width <= 0f) return
    val gap = TOKEN_GAP_DP * density
    val unitGap = UNIT_GAP_DP * density
    val detailGap = DETAIL_GAP_DP * density
    val count = tokens.size
    val textWidths = FloatArray(count)
    val valueWidths = FloatArray(count)
    val lineWidths = FloatArray(count)
    var anyLine = false
    for (i in 0 until count) {
        val token = tokens[i]
        textWidths[i] = paints.value.measureText(token.value)
        valueWidths[i] = textWidths[i] + (token.unit?.let { unitGap + paints.unit.measureText(it) } ?: 0f)
        lineWidths[i] = lineWidth(token, paints, detailGap)
        if (lineWidths[i] > 0f) anyLine = true
    }
    fun spanOf(lines: Boolean): Float {
        var span = 0f
        for (i in 0 until count) {
            if (i > 0) span += if (tokens[i].separated) gap * 2 else gap
            span += if (lines) max(valueWidths[i], lineWidths[i]) else valueWidths[i]
        }
        return span
    }
    var lines = anyLine
    var span = spanOf(lines)
    if (lines && span * MIN_TOKEN_SCALE > bounds.width) {
        lines = false
        span = spanOf(false)
    }
    val scale = if (span > bounds.width) bounds.width / span else 1f

    val middle = bounds.top + bounds.height / 2f
    val valueBaseline = middle - (paints.valueCap + paints.lineGap + paints.labelCap) / 2f + paints.valueCap
    val labelBaseline = valueBaseline + paints.lineGap + paints.labelCap
    val centredBaseline = middle + paints.valueCap / 2f
    val pivot = bounds.right
    fun onScreen(x: Float) = pivot + (x - pivot) * scale - bounds.left

    val canvas = drawContext.canvas.nativeCanvas
    val save = canvas.save()
    if (scale < 1f) canvas.scale(scale, scale, pivot, middle)
    var right = bounds.right
    for (i in count - 1 downTo 0) {
        val token = tokens[i]
        val alpha = if (token.hidden) HIDDEN_ALPHA else 1f
        fun tint(color: Color, fallback: Color): Int {
            val base = if (color.isSpecified) color else fallback
            return base.copy(alpha = base.alpha * alpha).toArgb()
        }
        val hasLine = lines && lineWidths[i] > 0f
        val width = if (lines) max(valueWidths[i], lineWidths[i]) else valueWidths[i]
        val baseline = if (hasLine) valueBaseline else centredBaseline

        var x = right - valueWidths[i]
        paints.value.color = tint(token.color, colors.text)
        canvas.drawText(token.value, x, baseline, paints.value)
        token.unit?.let { unit ->
            x += textWidths[i] + unitGap
            paints.unit.color = tint(colors.unit, colors.unit)
            canvas.drawText(unit, x, baseline, paints.unit)
        }
        if (hasLine) {
            var lineX = right - lineWidths[i]
            token.name?.let { name ->
                paints.label.color = tint(colors.label, colors.label)
                paints.label.isStrikeThruText = token.hidden
                canvas.drawText(name, lineX, labelBaseline, paints.label)
                paints.label.isStrikeThruText = false
                lineX += paints.label.measureText(name) + detailGap
            }
            token.detail?.let { detail ->
                paints.detail.color = tint(token.detailColor, colors.label)
                canvas.drawText(detail, lineX, labelBaseline, paints.detail)
            }
        }

        val start = right - width
        hits.add(onScreen(start - gap / 2f), onScreen(right + gap / 2f), token.key)
        right = start - gap
        if (token.separated && i > 0) {
            val inset = RULE_INSET_DP * density
            drawLine(colors.rule, Offset(right, bounds.top + inset), Offset(right, bounds.bottom - inset), 1f)
            right -= gap
        }
    }
    canvas.restoreToCount(save)
}

/** What a chart's window pill reads: the window, its short id when [narrow], and a Selected chart's reference lap. */
internal fun windowPillText(window: GraphWindow, narrow: Boolean, referenceLap: Int = 0): String {
    val name = if (narrow && window.isLap) window.id else window.label
    return if (window == GraphWindow.SELECTED && referenceLap > 0) "$name · L$referenceLap" else name
}

/** A chart's window, as a tonal pill beside its title; tinted when the chart picked its own. */
@Composable
internal fun WindowPill(text: String, emphasised: Boolean, description: String, onClick: () -> Unit) {
    val colors = MaterialTheme.colorScheme
    val container = if (emphasised) colors.primaryContainer else colors.surfaceContainerHigh
    val content = if (emphasised) colors.onPrimaryContainer else colors.onSurfaceVariant
    Row(
        Modifier
            .height(20.dp)
            .clip(CircleShape)
            .background(container)
            .clickable(onClick = onClick)
            .padding(start = 8.dp, end = 3.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(text, style = MaterialTheme.typography.labelMedium, color = content, maxLines = 1)
        Icon(
            painterResource(R.drawable.ic_expand_more),
            contentDescription = description,
            modifier = Modifier.size(16.dp),
            tint = content,
        )
    }
}
