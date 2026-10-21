package com.tracknrace.android.pages

import android.graphics.Paint
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.isSpecified
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.tracknrace.android.GraphWindow
import com.tracknrace.android.R

/** A chart header's height: one row that never wraps, so every plot on a tab lines up. */
internal const val HEADER_HEIGHT_DP = 28
/** Below this width a header's window selector shows the window's short id. */
internal const val NARROW_HEADER_DP = 300
/** A token's name and unit, relative to its value. */
internal const val TOKEN_LABEL_SCALE = 0.69f
private const val TOKEN_GAP_DP = 12f
private const val UNIT_GAP_DP = 2f
private const val PART_GAP_DP = 3f
private const val LABEL_TRACKING = 0.06f
private const val HIDDEN_ALPHA = 0.4f
// Shrinking the tokens this far keeps their names; any further drops them first.
private const val MIN_TOKEN_SCALE = 0.85f

/**
 * One value in a chart's header: the number in its series' colour, then its
 * unit and the series' name, small, on the same line. It is the series'
 * legend entry too: tapping it hides the series' line.
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
    /** Set further apart from the token before it, as a total. */
    val separated: Boolean = false,
    val hidden: Boolean = false,
)

/** The paints a header's tokens are drawn with: made once per text size. */
internal class HeaderPaints(valuePx: Float, labelPx: Float) {
    val value: Paint = textPaint(valuePx, FontWeight.Light.weight)
    val unit: Paint = textPaint(labelPx, FontWeight.Normal.weight)
    val label: Paint = textPaint(labelPx, FontWeight.Medium.weight).apply { letterSpacing = LABEL_TRACKING }
    val detail: Paint = textPaint(labelPx, FontWeight.Medium.weight)
    val valueCap = capHeight(value)
}

/** The theme colours a header's tokens are drawn in. */
internal class HeaderColors(val text: Color, val unit: Color, val label: Color)

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

/**
 * Paints [tokens] into [bounds], against its end, each on one line: the value,
 * its unit, its name, then any detail. Too wide, they shrink a little, then
 * drop their names, then shrink again, rather than wrap or spill over the
 * title. Records where each landed in [hits].
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
    val partGap = PART_GAP_DP * density
    val count = tokens.size
    val valueWidths = FloatArray(count)
    val unitWidths = FloatArray(count)
    val nameWidths = FloatArray(count)
    val detailWidths = FloatArray(count)
    for (i in 0 until count) {
        val token = tokens[i]
        valueWidths[i] = paints.value.measureText(token.value)
        unitWidths[i] = token.unit?.let { paints.unit.measureText(it) } ?: 0f
        nameWidths[i] = token.name?.let { paints.label.measureText(it) } ?: 0f
        detailWidths[i] = token.detail?.let { paints.detail.measureText(it) } ?: 0f
    }
    fun widthOf(i: Int, names: Boolean): Float =
        valueWidths[i] +
            (if (unitWidths[i] > 0f) unitGap + unitWidths[i] else 0f) +
            (if (names && nameWidths[i] > 0f) partGap + nameWidths[i] else 0f) +
            (if (detailWidths[i] > 0f) partGap + detailWidths[i] else 0f)
    fun spanOf(names: Boolean): Float {
        var span = 0f
        for (i in 0 until count) {
            if (i > 0) span += if (tokens[i].separated) gap * 2 else gap
            span += widthOf(i, names)
        }
        return span
    }
    var names = true
    var span = spanOf(true)
    if (span * MIN_TOKEN_SCALE > bounds.width) {
        names = false
        span = spanOf(false)
    }
    val scale = if (span > bounds.width) bounds.width / span else 1f

    val middle = bounds.top + bounds.height / 2f
    val baseline = middle + paints.valueCap / 2f
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
        val width = widthOf(i, names)
        var x = right - width
        paints.value.color = tint(token.color, colors.text)
        canvas.drawText(token.value, x, baseline, paints.value)
        x += valueWidths[i]
        token.unit?.let { unit ->
            x += unitGap
            paints.unit.color = tint(colors.unit, colors.unit)
            canvas.drawText(unit, x, baseline, paints.unit)
            x += unitWidths[i]
        }
        if (names) token.name?.let { name ->
            x += partGap
            paints.label.color = tint(colors.label, colors.label)
            paints.label.isStrikeThruText = token.hidden
            canvas.drawText(name, x, baseline, paints.label)
            paints.label.isStrikeThruText = false
            x += nameWidths[i]
        }
        token.detail?.let { detail ->
            x += partGap
            paints.detail.color = tint(token.detailColor, colors.label)
            canvas.drawText(detail, x, baseline, paints.detail)
        }

        val start = right - width
        hits.add(onScreen(start - gap / 2f), onScreen(right + gap / 2f), token.key)
        right = start - gap
        if (token.separated && i > 0) right -= gap
    }
    canvas.restoreToCount(save)
}

/** What a chart's window pill reads: the window, its short id when [narrow], and a Selected chart's reference lap. */
internal fun windowPillText(window: GraphWindow, narrow: Boolean, referenceLap: Int = 0): String {
    val name = if (narrow && window.isLap) window.id else window.label
    return if (window == GraphWindow.SELECTED && referenceLap > 0) "$name · L$referenceLap" else name
}

/**
 * A chart's window beside its title: the window's name and a caret in the
 * accent colour. A chart that picked its own window shows it in the
 * contrasting tertiary colour, with a dot before it.
 */
@Composable
internal fun WindowPill(text: String, emphasised: Boolean, description: String, onClick: () -> Unit) {
    val accent = if (emphasised) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.primary
    Row(
        Modifier
            .height(HEADER_HEIGHT_DP.dp)
            .clip(RoundedCornerShape(6.dp))
            .clickable(role = Role.DropdownList, onClick = onClick)
            .padding(start = 4.dp, end = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (emphasised) {
            Box(Modifier.padding(end = 5.dp).size(4.dp).clip(CircleShape).background(accent))
        }
        Text(text, style = MaterialTheme.typography.labelMedium, color = accent, maxLines = 1)
        Icon(
            painterResource(R.drawable.ic_expand_more),
            contentDescription = description,
            modifier = Modifier.size(14.dp),
            tint = accent,
        )
    }
}
