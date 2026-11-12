package com.nogoat.tracknrace.pages

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nogoat.tracknrace.DriverLap
import com.nogoat.tracknrace.DriverLapHistory
import com.nogoat.tracknrace.R
import com.nogoat.tracknrace.TimingTowerState
import com.nogoat.tracknrace.formatLapMs
import com.nogoat.tracknrace.formatSectorMs
import java.util.Locale
import kotlin.math.ceil
import kotlin.math.floor
import kotlinx.coroutines.delay

// The Timing page's Driver laps sheet (the desktop's Driver laps dialog, Laps 4
// in the design): a chart of every lap time over a list that switches between
// times and sectors. The engine pushes nothing for it; the sheet asks for its
// car's laps on open and again every few seconds while it stays open.

private const val LAP_HISTORY_REFRESH_MS = 3_000L

private enum class LapListMode(val title: String) { TIMES("Times"), SECTORS("Sectors") }

private val ChartHeight = 172.dp
/** Room under the plot for the first and last lap numbers. */
private val ChartLapLabels = 18.dp

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun DriverLapsSheet(
    carIndex: Int,
    tower: () -> TimingTowerState,
    history: () -> DriverLapHistory?,
    /** The car that holds the session's fastest lap, when known. */
    fastestLapCar: () -> Int?,
    labels: Map<String, String>,
    onRequest: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    val request by rememberUpdatedState(onRequest)
    LaunchedEffect(carIndex) {
        while (true) {
            request(carIndex)
            delay(LAP_HISTORY_REFRESH_MS)
        }
    }
    val laps by remember(carIndex) {
        derivedStateOf(structuralEqualityPolicy()) { history()?.takeIf { it.carIndex == carIndex } }
    }
    val driver by remember(carIndex) { derivedStateOf(structuralEqualityPolicy()) { tower().drivers[carIndex] } }
    val position by remember(carIndex) {
        derivedStateOf { tower().cars.firstOrNull { it.index == carIndex }?.position ?: 0 }
    }
    val tyre by remember(carIndex) { derivedStateOf(structuralEqualityPolicy()) { tower().tyreStatuses[carIndex] } }
    val holdsFastest by remember(carIndex) { derivedStateOf { fastestLapCar() == carIndex } }
    var mode by rememberSaveable { mutableStateOf(LapListMode.TIMES) }

    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = false)
    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        val h = laps
        val summary = remember(h) { h?.let(::lapSummary) }
        Column(Modifier.fillMaxWidth()) {
            SheetHeader(
                name = displayName(driver?.name, carIndex),
                position = position,
                summary = summary,
                holdsFastest = holdsFastest,
                onClose = onDismiss,
            )
            when {
                h == null -> SheetMessage("Loading laps…")
                h.laps.isEmpty() -> SheetMessage("No completed laps yet")
                else -> {
                    val pitLabel = tyre?.let { compoundText(it, labels) }
                    val pitColor = tyre?.let { tyreTextColor(it.visualCompound) } ?: MaterialTheme.colorScheme.onSurfaceVariant
                    val lapSummary = summary!!
                    LapChart(h, lapSummary, holdsFastest, pitLabel, pitColor, Modifier.fillMaxWidth().height(ChartHeight).padding(horizontal = 16.dp))
                    ListHeader(mode) { mode = it }
                    LapList(h, lapSummary, mode, holdsFastest, pitLabel, pitColor)
                }
            }
        }
    }
}

// ── Summary ─────────────────────────────────────────────────────────────────

@Immutable
private data class LapSummary(
    val best: DriverLap?,
    val last: DriverLap,
    val averageMs: Int,
    /** The in and out laps of the stop that began the current stint, when known. */
    val inLap: Int,
    val outLap: Int,
    /** The driver's own fastest valid sectors. */
    val ownBest: IntArray,
)

private fun lapSummary(h: DriverLapHistory): LapSummary? {
    if (h.laps.isEmpty()) return null
    val outLap = if (h.stintStartLap > 1) h.stintStartLap else 0
    val inLap = if (outLap > 0) outLap - 1 else 0
    // The average leaves out the standing start and the stop's in and out laps.
    val racing = h.laps.filter { it.valid && it.lapNumber != 1 && it.lapNumber != inLap && it.lapNumber != outLap }
    val ownBest = IntArray(3) { sector ->
        h.laps.mapNotNull { lap ->
            val (ms, valid) = lap.sector(sector)
            ms.takeIf { valid && it > 0 }
        }.minOrNull() ?: 0
    }
    return LapSummary(
        best = h.laps.firstOrNull { it.lapNumber == h.bestLapNumber },
        last = h.laps.last(),
        averageMs = if (racing.isEmpty()) 0 else racing.sumOf { it.timeMs.toLong() }.div(racing.size).toInt(),
        inLap = inLap,
        outLap = outLap,
        ownBest = ownBest,
    )
}

private fun DriverLap.sector(index: Int): Pair<Int, Boolean> = when (index) {
    0 -> s1Ms to s1Valid
    1 -> s2Ms to s2Valid
    else -> s3Ms to s3Valid
}

private fun DriverLapHistory.overallBest(index: Int): Int = when (index) {
    0 -> overallBestS1Ms
    1 -> overallBestS2Ms
    else -> overallBestS3Ms
}

/** The roster name's last word, as "Antonelli". */
private fun displayName(name: String?, index: Int): String {
    val last = name?.trim()?.split(Regex("\\s+"))?.lastOrNull()?.takeIf { it.isNotEmpty() } ?: return "Car ${index + 1}"
    return last.lowercase(Locale.ROOT).replaceFirstChar { it.titlecase(Locale.ROOT) }
}

/** A driver's own best, in the dashboard's green; darker on a light surface. */
@Composable
private fun personalBestColor(): Color =
    if (MaterialTheme.colorScheme.surface.luminance() >= 0.5f) Color(0xff0d6b2f) else Color(0xff7adca7)

/** The best lap's colour: purple when it is the session's fastest, else the driver's own green. */
@Composable
private fun bestLapColor(holdsFastest: Boolean): Color = if (holdsFastest) fastestLapColor() else personalBestColor()

private val Numbers = TextStyle(fontFamily = FontFamily.Monospace, fontFeatureSettings = "tnum", fontSize = 13.sp)
private val Caption = TextStyle(fontSize = 10.sp, letterSpacing = 1.sp)

// ── Header ──────────────────────────────────────────────────────────────────

@Composable
private fun SheetHeader(name: String, position: Int, summary: LapSummary?, holdsFastest: Boolean, onClose: () -> Unit) {
    val scheme = MaterialTheme.colorScheme
    Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 4.dp, bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.Bottom) {
                Text(name, style = MaterialTheme.typography.titleMedium.copy(fontSize = 18.sp), color = scheme.onSurface, maxLines = 1)
                val detail = buildString {
                    if (position > 0) append(" · P").append(position)
                    if (summary != null) append(" · ").append(summary.last.lapNumber).append(if (summary.last.lapNumber == 1) " lap" else " laps")
                }
                Text(detail, Modifier.padding(bottom = 2.dp), style = MaterialTheme.typography.bodySmall, color = scheme.onSurfaceVariant, maxLines = 1)
            }
            if (summary != null) {
                Row(Modifier.padding(top = 2.dp)) {
                    val small = MaterialTheme.typography.bodySmall
                    summary.best?.let { best ->
                        Text("Best ", style = small, color = scheme.onSurfaceVariant)
                        Text(formatLapMs(best.timeMs), style = small.copy(fontWeight = FontWeight.Medium), color = bestLapColor(holdsFastest))
                        Text(" L${best.lapNumber} · ", style = small, color = scheme.onSurfaceVariant)
                    }
                    Text("Last ${formatLapMs(summary.last.timeMs)}", style = small, color = scheme.onSurfaceVariant)
                    if (summary.averageMs > 0) Text(" · Avg ${formatLapMs(summary.averageMs)}", style = small, color = scheme.onSurfaceVariant)
                }
            }
        }
        IconButton(onClick = onClose) {
            Icon(painterResource(R.drawable.ic_close), contentDescription = "Close")
        }
    }
}

@Composable
private fun SheetMessage(text: String) {
    Text(
        text,
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 32.dp),
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        textAlign = TextAlign.Center,
    )
}

// ── Chart ───────────────────────────────────────────────────────────────────

/**
 * Every lap's time against its lap number. The scale spans the racing laps
 * (all but lap 1 and the stop's in and out laps), so the line runs unbroken
 * through slow stints; only those excluded laps can fall outside it, and they
 * sit hollow on the edge they passed. Invalid laps are hollow; the stop that
 * began the stint is a dashed line, named under the plot.
 */
@Composable
private fun LapChart(
    h: DriverLapHistory,
    summary: LapSummary,
    holdsFastest: Boolean,
    pitLabel: String?,
    pitColor: Color,
    modifier: Modifier,
) {
    val scheme = MaterialTheme.colorScheme
    val line = scheme.primary
    val grid = scheme.outlineVariant
    val hollowFill = scheme.surfaceContainerLow
    val offScale = scheme.outline
    val best = bestLapColor(holdsFastest)
    val pit = pitColor.takeIf { it != Color.Unspecified } ?: scheme.tertiary
    val fastestMs = h.laps.minOf { it.timeMs }
    // The scale covers the racing laps: every lap but the standing start and
    // the stop's in and out laps, however slow (wear, traffic, a safety car,
    // rain). Only those few laps can fall outside it.
    val racing = h.laps.filter { it.lapNumber != 1 && it.lapNumber != summary.inLap && it.lapNumber != summary.outLap }
    val scaled = racing.ifEmpty { h.laps }
    val scaleMin = scaled.minOf { it.timeMs }
    val scaleMax = maxOf(scaled.maxOf { it.timeMs }, scaleMin + 1200)
    val lo = (scaleMin - 300) / 1000.0
    val hi = (scaleMax + 300) / 1000.0
    val step = when {
        hi - lo > 12.0 -> 5
        hi - lo > 4.0 -> 2
        else -> 1
    }
    val ticks = (ceil(lo).toInt()..floor(hi).toInt()).filter { it % step == 0 }
    val firstLap = h.laps.first().lapNumber
    val lastLap = maxOf(h.laps.last().lapNumber, firstLap + 1)
    val pitAfter = if (h.stintStartLap > 1) h.stintStartLap - 1 else 0
    val axis = 40.dp
    val description = "Lap times for ${h.laps.size} laps, best ${formatLapMs(fastestMs)}"

    Box(modifier.semantics { contentDescription = description }) {
        Canvas(Modifier.fillMaxSize().padding(bottom = ChartLapLabels)) {
            val left = axis.toPx()
            val top = 6.dp.toPx()
            val plotW = size.width - left - 4.dp.toPx()
            val plotH = size.height - top - 6.dp.toPx()
            fun x(lap: Int) = left + (lap - firstLap).toFloat() / (lastLap - firstLap) * plotW
            fun y(seconds: Double) = top + ((hi - seconds.coerceIn(lo, hi)) / (hi - lo) * plotH).toFloat()

            for (tick in ticks) drawLine(grid, Offset(left, y(tick.toDouble())), Offset(left + plotW, y(tick.toDouble())), 1.dp.toPx())
            if (pitAfter in firstLap until lastLap) {
                val px = (x(pitAfter) + x(pitAfter + 1)) / 2
                drawLine(pit, Offset(px, top - 4.dp.toPx()), Offset(px, top + plotH), 1.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(6f, 6f)))
            }
            val path = Path()
            var pen = false
            for (lap in h.laps) {
                val seconds = lap.timeMs / 1000.0
                if (seconds > hi || seconds < lo) { pen = false; continue }
                if (pen) path.lineTo(x(lap.lapNumber), y(seconds)) else path.moveTo(x(lap.lapNumber), y(seconds))
                pen = true
            }
            drawPath(path, line, style = Stroke(1.5.dp.toPx()))
            for (lap in h.laps) {
                val seconds = lap.timeMs / 1000.0
                val off = seconds > hi || seconds < lo
                val isBest = lap.lapNumber == h.bestLapNumber
                // y() clamps, so an off-scale lap sits on the edge it passed.
                val center = Offset(x(lap.lapNumber), y(seconds))
                val radius = (if (isBest) 4.5f else 3f).dp.toPx()
                val tone = when {
                    off -> offScale
                    isBest -> best
                    else -> line
                }
                if (off || !lap.valid) {
                    drawCircle(hollowFill, radius, center)
                    drawCircle(tone, radius, center, style = Stroke(1.5.dp.toPx()))
                } else {
                    drawCircle(tone, radius, center)
                }
            }
        }
        // Axis labels over the canvas, in the theme's text style.
        val labelStyle = MaterialTheme.typography.labelSmall.copy(fontWeight = FontWeight.Normal)
        // The canvas plots between 6dp from its top and 6dp above the lap labels.
        val plotHeight = ChartHeight - ChartLapLabels - 12.dp
        for (tick in ticks) {
            val fraction = ((hi - tick) / (hi - lo)).toFloat()
            Text(
                "${tick / 60}:${"%02d".format(Locale.ROOT, tick % 60)}",
                Modifier.width(36.dp).offset(y = 6.dp + plotHeight * fraction - 7.dp),
                style = labelStyle,
                color = scheme.onSurfaceVariant,
            )
        }
        // The stop is named in the lap row under the plot, where no point can sit on it.
        Row(Modifier.fillMaxWidth().align(Alignment.BottomStart).padding(start = axis)) {
            Text("L$firstLap", style = labelStyle, color = scheme.onSurfaceVariant)
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                if (pitAfter in firstLap until lastLap) {
                    Text(
                        if (pitLabel != null) "Pit after L$pitAfter · now $pitLabel" else "Pit after L$pitAfter",
                        style = labelStyle,
                        color = pit,
                        maxLines = 1,
                    )
                }
            }
            Text("L${h.laps.last().lapNumber}", style = labelStyle, color = scheme.onSurfaceVariant)
        }
    }
}

// ── List ────────────────────────────────────────────────────────────────────

@Composable
private fun ListHeader(mode: LapListMode, onMode: (LapListMode) -> Unit) {
    val scheme = MaterialTheme.colorScheme
    HorizontalDivider(Modifier.padding(top = 8.dp), color = scheme.outlineVariant)
    Row(
        Modifier.fillMaxWidth().height(56.dp).padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("LAPS · NEWEST FIRST", Modifier.weight(1f), style = Caption, color = scheme.onSurfaceVariant)
        SingleChoiceSegmentedButtonRow(Modifier.width(184.dp)) {
            LapListMode.entries.forEachIndexed { index, entry ->
                SegmentedButton(
                    selected = entry == mode,
                    onClick = { onMode(entry) },
                    shape = SegmentedButtonDefaults.itemShape(index, LapListMode.entries.size),
                ) { Text(entry.title, maxLines = 1) }
            }
        }
    }
}

private sealed interface LapListItem {
    val key: String

    data class Lap(val lap: DriverLap) : LapListItem {
        override val key get() = "lap-${lap.lapNumber}"
    }

    data class Pit(val afterLap: Int) : LapListItem {
        override val key get() = "pit-$afterLap"
    }
}

@Composable
private fun LapList(
    h: DriverLapHistory,
    summary: LapSummary,
    mode: LapListMode,
    holdsFastest: Boolean,
    pitLabel: String?,
    pitColor: Color,
) {
    val items = remember(h, summary) {
        buildList {
            for (lap in h.laps.asReversed()) {
                if (summary.inLap > 0 && lap.lapNumber == summary.inLap) add(LapListItem.Pit(summary.inLap))
                add(LapListItem.Lap(lap))
            }
        }
    }
    LazyColumn(Modifier.fillMaxWidth(), contentPadding = PaddingValues(bottom = 24.dp)) {
        items(items, key = { it.key }) { item ->
            when (item) {
                is LapListItem.Pit -> PitRow(item.afterLap, pitLabel, pitColor)
                is LapListItem.Lap -> when (mode) {
                    LapListMode.TIMES -> TimeRow(item.lap, h, summary, holdsFastest)
                    LapListMode.SECTORS -> SectorRow(item.lap, h, summary)
                }
            }
        }
    }
}

@Composable
private fun PitRow(afterLap: Int, pitLabel: String?, pitColor: Color) {
    val scheme = MaterialTheme.colorScheme
    Row(
        Modifier.fillMaxWidth().height(28.dp).background(scheme.surfaceContainerHigh).padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("Pit stop after lap $afterLap", Modifier.weight(1f), style = MaterialTheme.typography.bodySmall, color = scheme.onSurfaceVariant)
        if (pitLabel != null) {
            Text("now ", style = MaterialTheme.typography.bodySmall, color = scheme.onSurfaceVariant)
            Text(pitLabel, style = MaterialTheme.typography.bodySmall.copy(fontWeight = FontWeight.Medium), color = pitColor)
        }
    }
}

@Composable
private fun LapRow(lapNumber: Int, content: @Composable RowScope.() -> Unit) {
    val scheme = MaterialTheme.colorScheme
    Column {
        Row(
            Modifier.fillMaxWidth().height(32.dp).padding(horizontal = 16.dp).semantics(mergeDescendants = true) {},
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(lapNumber.toString(), Modifier.width(32.dp), style = Numbers, color = scheme.onSurfaceVariant)
            content()
        }
        HorizontalDivider(color = scheme.outlineVariant.copy(alpha = 0.4f))
    }
}

@Composable
private fun TimeRow(lap: DriverLap, h: DriverLapHistory, summary: LapSummary, holdsFastest: Boolean) {
    val scheme = MaterialTheme.colorScheme
    val isBest = lap.lapNumber == h.bestLapNumber
    val bestMs = summary.best?.timeMs ?: 0
    val gap = lap.timeMs - bestMs
    val tag = when {
        isBest -> "Best"
        !lap.valid -> "Invalid"
        lap.lapNumber == summary.inLap -> "In lap"
        lap.lapNumber == summary.outLap -> "Out lap"
        bestMs > 0 -> "+" + String.format(Locale.ROOT, "%.3f", gap / 1000.0)
        else -> ""
    }
    val tagColor = when {
        isBest -> bestLapColor(holdsFastest)
        !lap.valid || lap.lapNumber == summary.inLap || lap.lapNumber == summary.outLap -> scheme.outline
        gap in 0..499 -> personalBestColor()
        else -> scheme.onSurfaceVariant
    }
    LapRow(lap.lapNumber) {
        Text(
            formatLapMs(lap.timeMs),
            Modifier.weight(1f),
            style = Numbers.copy(
                fontWeight = FontWeight.Medium,
                textDecoration = if (lap.valid) TextDecoration.None else TextDecoration.LineThrough,
            ),
            color = when {
                !lap.valid -> scheme.outline
                isBest -> bestLapColor(holdsFastest)
                else -> scheme.onSurface
            },
            textAlign = TextAlign.End,
        )
        Text(tag, Modifier.weight(1f), style = Numbers.copy(fontSize = 12.sp), color = tagColor, textAlign = TextAlign.End)
        Box(Modifier.weight(0.6f))
    }
}

@Composable
private fun SectorRow(lap: DriverLap, h: DriverLapHistory, summary: LapSummary) {
    val scheme = MaterialTheme.colorScheme
    val purple = fastestLapColor()
    val green = personalBestColor()
    LapRow(lap.lapNumber) {
        for (sector in 0..2) {
            val (ms, valid) = lap.sector(sector)
            val color = when {
                !valid -> scheme.outline
                ms > 0 && ms == h.overallBest(sector) -> purple
                ms > 0 && ms == summary.ownBest[sector] -> green
                else -> scheme.onSurfaceVariant
            }
            Text(
                formatSectorMs(ms),
                Modifier.weight(1f),
                style = Numbers.copy(fontWeight = if (color == purple || color == green) FontWeight.Bold else FontWeight.Normal),
                color = color,
                textAlign = TextAlign.End,
            )
        }
    }
}
