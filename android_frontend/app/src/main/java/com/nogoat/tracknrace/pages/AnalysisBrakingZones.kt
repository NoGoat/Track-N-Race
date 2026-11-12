package com.nogoat.tracknrace.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nogoat.tracknrace.BRAKING_LEVEL_S
import com.nogoat.tracknrace.BrakingApex
import com.nogoat.tracknrace.BrakingObservation
import com.nogoat.tracknrace.BrakingPoint
import com.nogoat.tracknrace.BrakingZone
import com.nogoat.tracknrace.BrakingZones
import com.nogoat.tracknrace.R
import com.nogoat.tracknrace.formatDelta
import java.util.Locale
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/** What the Map view shows of the braking zones: only the chip, the zone list, or one zone. */
internal enum class BrakingPanel { CLOSED, LIST, ZONE }

/** The laps' names and colours, as the zone list and drawer label them. */
internal class BrakingLaps(
    val currentLabel: String,
    val currentColor: Color,
    val comparisonLabel: String,
    val comparisonColor: Color,
)

// An exit that spun shows in this colour instead of its lap's, as on the desktop.
private val SpinWarningColor = Color(0xfff59e0b)

/** A zone's colour on the map and in the list: the lap quicker through it, else neutral. */
internal fun brakingZoneColor(zone: BrakingZone, laps: BrakingLaps, neutral: Color): Color = when (zone.quicker) {
    0 -> laps.currentColor
    1 -> laps.comparisonColor
    else -> neutral
}

/**
 * The drawer the zone list and a zone's head to head sit in. Below the map in
 * portrait it has a handle, and a swipe down closes it; beside the map in
 * landscape it is a plain panel.
 */
@Composable
private fun BrakingDrawer(
    docked: Boolean,
    onSwipeDown: () -> Unit,
    modifier: Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    Surface(
        modifier,
        shape = if (docked) RoundedCornerShape(0.dp) else RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp),
        color = MaterialTheme.colorScheme.surfaceContainer,
    ) {
        Column {
            if (!docked) {
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height(20.dp)
                        .pointerInput(Unit) {
                            var travel = 0f
                            detectVerticalDragGestures(
                                onDragStart = { travel = 0f },
                                onDragEnd = { if (travel > 32.dp.toPx()) onSwipeDown() },
                            ) { change, amount ->
                                travel += amount
                                change.consume()
                            }
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    Box(Modifier.size(width = 32.dp, height = 4.dp).background(MaterialTheme.colorScheme.outline, RoundedCornerShape(2.dp)))
                }
            }
            content()
        }
    }
}

/**
 * Every braking zone of the laps on show, as the desktop's Braking Zones
 * panel lists them: where each starts and the gap through it, in the quicker
 * lap's colour; with one lap, the entry speed instead. Tapping one opens its
 * head to head.
 */
@Composable
internal fun BrakingZoneList(
    zones: BrakingZones?,
    loading: Boolean,
    unavailable: Boolean,
    laps: BrakingLaps,
    docked: Boolean,
    onZone: (Int) -> Unit,
    onClose: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val numberStyle = MaterialTheme.typography.bodyMedium.copy(fontFeatureSettings = "tnum")
    BrakingDrawer(docked, onClose, modifier) {
        Row(Modifier.fillMaxWidth().padding(start = 20.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("Braking zones", modifier = Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
            IconButton(onClick = onClose) {
                Icon(painterResource(R.drawable.ic_close), contentDescription = "Close braking zones", modifier = Modifier.size(20.dp), tint = colors.onSurfaceVariant)
            }
        }
        val rows = zones?.zones.orEmpty()
        if (zones == null || rows.isEmpty()) {
            val message = when {
                zones != null -> "No braking zones found on this lap."
                unavailable -> "This desktop cannot send braking zones yet. Update Track N Race on the desktop."
                else -> null
            }
            Column(
                Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 16.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                if (message == null || loading) CircularProgressIndicator(Modifier.size(32.dp))
                Text(
                    message ?: "Finding braking zones",
                    style = MaterialTheme.typography.bodyMedium,
                    color = colors.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                )
            }
            return@BrakingDrawer
        }
        val paired = zones.paired
        Text(
            buildAnnotatedString {
                append("${rows.size} zones · ")
                if (paired) {
                    withStyle(SpanStyle(color = laps.currentColor, fontWeight = FontWeight.Bold)) { append(laps.currentLabel) }
                    append(" quicker in ${rows.count { it.quicker == 0 }}, ")
                    withStyle(SpanStyle(color = laps.comparisonColor, fontWeight = FontWeight.Bold)) { append(laps.comparisonLabel) }
                    append(" in ${rows.count { it.quicker == 1 }}")
                } else {
                    append("choose a lap to compare for the gaps")
                }
            },
            modifier = Modifier.padding(start = 20.dp, end = 20.dp, bottom = 8.dp),
            style = MaterialTheme.typography.bodySmall,
            color = colors.onSurfaceVariant,
        )
        val headerStyle = MaterialTheme.typography.labelSmall.copy(letterSpacing = 0.6.sp)
        Row(Modifier.fillMaxWidth().height(24.dp).padding(start = 20.dp, end = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("ZONE", modifier = Modifier.width(48.dp), style = headerStyle, color = colors.onSurfaceVariant)
            Text("AT", modifier = Modifier.weight(1f), style = headerStyle, color = colors.onSurfaceVariant)
            Text(if (paired) "GAP" else "ENTRY", style = headerStyle, color = colors.onSurfaceVariant)
            Box(Modifier.width(26.dp))
        }
        HorizontalDivider(color = colors.outlineVariant.copy(alpha = 0.5f))
        LazyColumn(Modifier.weight(1f)) {
            itemsIndexed(rows) { index, zone ->
                val color = brakingZoneColor(zone, laps, colors.onSurfaceVariant)
                Row(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(min = 40.dp)
                        .clickable(onClickLabel = "Show zone ${zone.number}") { onZone(index) }
                        .padding(start = 20.dp, end = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("${zone.number}", modifier = Modifier.width(48.dp), style = numberStyle, fontWeight = FontWeight.Medium)
                    Text(formatMetres(zone.fromM), modifier = Modifier.weight(1f), style = numberStyle, color = colors.onSurfaceVariant)
                    Text(
                        when {
                            !paired -> zone.current?.let { "${it.speedKph.roundToInt()} km/h" } ?: "—"
                            zone.gapS != null -> formatDelta(zone.gapS)
                            else -> "—"
                        },
                        style = numberStyle,
                        color = if (zone.quicker != null) color else colors.onSurfaceVariant,
                        fontWeight = if (zone.quicker != null) FontWeight.Bold else FontWeight.Normal,
                    )
                    Box(Modifier.width(26.dp), contentAlignment = Alignment.CenterEnd) {
                        Icon(painterResource(R.drawable.ic_chevron_right), contentDescription = null, modifier = Modifier.size(16.dp), tint = colors.onSurfaceVariant)
                    }
                }
                HorizontalDivider(color = colors.outlineVariant.copy(alpha = 0.5f))
            }
        }
    }
}

/**
 * One braking zone head to head, as the desktop's card shows it: which lap
 * was quicker through it, then a row per measure with each lap's value either
 * side and the better one in bold in its lap's colour, then libtnrp's
 * observations. Previous and next step through the zones; closing goes back
 * to the list.
 */
@Composable
internal fun BrakingZoneDrawer(
    zone: BrakingZone,
    index: Int,
    count: Int,
    laps: BrakingLaps,
    docked: Boolean,
    onStep: (Int) -> Unit,
    onBack: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val verdict = brakingVerdict(zone, laps)
    val rows = headToHead(zone)
    val observations = zone.pair?.observations.orEmpty().map { observationText(it, laps) }
    BrakingDrawer(docked, onBack, modifier) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = { onStep(index - 1) }, enabled = index > 0) {
                Icon(painterResource(R.drawable.ic_chevron_left), contentDescription = "Previous zone", modifier = Modifier.size(20.dp))
            }
            Column(Modifier.weight(1f), horizontalAlignment = Alignment.CenterHorizontally) {
                Text(
                    verdict?.first ?: "Zone ${zone.number}",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold,
                    color = verdict?.second ?: colors.onSurface,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    "Zone ${zone.number} · ${formatMetres(zone.fromM)} – ${formatMetres(zone.toM)}".uppercase(Locale.ROOT),
                    style = MaterialTheme.typography.labelSmall.copy(letterSpacing = 0.6.sp, fontFeatureSettings = "tnum"),
                    color = colors.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            IconButton(onClick = { onStep(index + 1) }, enabled = index < count - 1) {
                Icon(painterResource(R.drawable.ic_chevron_right), contentDescription = "Next zone", modifier = Modifier.size(20.dp))
            }
            IconButton(onClick = onBack) {
                Icon(painterResource(R.drawable.ic_close), contentDescription = "Back to all zones", modifier = Modifier.size(20.dp), tint = colors.onSurfaceVariant)
            }
        }
        Column(
            Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState())
                .padding(start = 20.dp, end = 20.dp, top = 6.dp, bottom = 16.dp),
        ) {
            val showCurrent = zone.current != null
            val showComparison = zone.comparison != null
            val cellStyle = MaterialTheme.typography.bodyMedium.copy(fontFeatureSettings = "tnum")
            val measureStyle = MaterialTheme.typography.labelSmall
            Row(Modifier.fillMaxWidth().height(20.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(if (showCurrent) laps.currentLabel else "", modifier = Modifier.weight(1f), style = MaterialTheme.typography.labelMedium, fontWeight = FontWeight.Bold, color = laps.currentColor)
                Text(
                    if (showComparison) laps.comparisonLabel else "",
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.Bold,
                    color = laps.comparisonColor,
                    textAlign = TextAlign.End,
                )
            }
            @Composable
            fun RowScope.Cell(value: String?, wins: Boolean, warn: Boolean, color: Color, shown: Boolean, align: TextAlign) {
                Text(
                    if (shown) value ?: "—" else "",
                    modifier = Modifier.weight(1f),
                    style = cellStyle,
                    color = when {
                        warn -> SpinWarningColor
                        wins -> color
                        else -> colors.onSurface
                    },
                    fontWeight = if (wins || warn) FontWeight.Bold else FontWeight.Normal,
                    textAlign = align,
                    maxLines = 1,
                )
            }
            for (row in rows) {
                HorizontalDivider(color = colors.outlineVariant.copy(alpha = 0.5f))
                Row(Modifier.fillMaxWidth().heightIn(min = 26.dp), verticalAlignment = Alignment.CenterVertically) {
                    Cell(row.current, row.better == 0, row.warnCurrent, laps.currentColor, showCurrent, TextAlign.Start)
                    Text(row.label, modifier = Modifier.padding(horizontal = 6.dp), style = measureStyle, color = colors.onSurfaceVariant, maxLines = 1)
                    Cell(row.comparison, row.better == 1, row.warnComparison, laps.comparisonColor, showComparison, TextAlign.End)
                }
            }
            if (observations.isNotEmpty()) {
                HorizontalDivider(color = colors.outlineVariant)
                Column(Modifier.padding(top = 8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    for (text in observations) {
                        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            Text("▸", style = MaterialTheme.typography.bodySmall, color = colors.outline)
                            Text(text, style = MaterialTheme.typography.bodySmall, color = colors.onSurfaceVariant)
                        }
                    }
                }
            }
        }
    }
}

/** Which lap was quicker through the zone, and its colour; null without two timed laps. */
private fun brakingVerdict(zone: BrakingZone, laps: BrakingLaps): Pair<String, Color?>? {
    val pair = zone.pair ?: return null
    if (pair.currentTimeS < 0 || pair.comparisonTimeS < 0) return null
    val quicker = pair.comparisonTimeS - pair.currentTimeS
    return when {
        abs(quicker) < BRAKING_LEVEL_S -> "Level through the zone" to null
        quicker > 0 -> "${laps.currentLabel} ${seconds(abs(quicker))} s quicker" to laps.currentColor
        else -> "${laps.comparisonLabel} ${seconds(abs(quicker))} s quicker" to laps.comparisonColor
    }
}

/**
 * One measure of the head to head: each lap's value (null where it has none),
 * which lap did better (0, 1 or null), and which values are warnings.
 */
private class HeadToHeadRow(
    val label: String,
    val current: String?,
    val comparison: String?,
    val better: Int?,
    val warnCurrent: Boolean = false,
    val warnComparison: Boolean = false,
)

/** The desktop's head-to-head rows for a whole zone, braking half and exit, as its BrakingTooltip builds them. */
private fun headToHead(zone: BrakingZone): List<HeadToHeadRow> {
    val current = zone.current
    val comparison = zone.comparison
    val pair = zone.pair
    val both = current != null && comparison != null
    // Braking points compare only when both laps first brake for the same
    // corner; one lap can take it on a lift.
    val sameBrake = both && (pair?.sameBrakingPoint ?: true)
    fun pick(a: Double, b: Double, higher: Boolean, margin: Double = 0.0): Int? =
        if (abs(a - b) <= margin) null else if ((a > b) == higher) 0 else 1
    fun known(value: Double) = value >= 0
    val rows = ArrayList<HeadToHeadRow>()
    rows += HeadToHeadRow(
        "Brakes at", current?.let(::brakeAtText), comparison?.let(::brakeAtText),
        if (sameBrake) pick(current!!.distanceM, comparison!!.distanceM, true, 0.5) else null,
    )
    rows += HeadToHeadRow(
        "Braking", current?.let { metresText(it.brakingDistanceM) }, comparison?.let { metresText(it.brakingDistanceM) },
        if (sameBrake && current!!.brakingDistanceM >= 0 && comparison!!.brakingDistanceM >= 0) {
            pick(current.brakingDistanceM, comparison.brakingDistanceM, false, 0.5)
        } else {
            null
        },
    )
    rows += HeadToHeadRow(
        "Entry", current?.let { "${it.speedKph.roundToInt()} km/h" }, comparison?.let { "${it.speedKph.roundToInt()} km/h" },
        if (sameBrake) pick(current!!.speedKph, comparison!!.speedKph, true, 0.5) else null,
    )
    rows += HeadToHeadRow("Coast", current?.let(::coastText), comparison?.let(::coastText), null)
    // One apex row per corner, flat-out ones included, each with which lap
    // ran further inside at the apex when both recorded their positions.
    val corners = alignCorners(current?.apexes.orEmpty(), comparison?.apexes.orEmpty())
    corners.forEachIndexed { index, (a, b) ->
        val side = sideText((a ?: b)!!.side)
        val name = if (corners.size > 1) "Apex ${index + 1}" else "Apex"
        rows += HeadToHeadRow(
            "$name$side",
            current?.let { a?.let { "${it.speedKph.roundToInt()} km/h" } ?: "—" },
            comparison?.let { b?.let { "${it.speedKph.roundToInt()} km/h" } ?: "—" },
            if (a != null && b != null) pick(a.speedKph, b.speedKph, true, 0.5) else null,
        )
        if (a?.hasLine == true && b?.hasLine == true) {
            rows += HeadToHeadRow(if (corners.size > 1) "Line ${index + 1}" else "Line", lineText(a.lineGapM), lineText(b.lineGapM), null)
        }
    }
    rows += HeadToHeadRow(
        "ERS",
        pair?.currentHarvestKj?.takeIf { it >= 0 }?.let(::harvestText),
        pair?.comparisonHarvestKj?.takeIf { it >= 0 }?.let(::harvestText),
        if (pair != null && known(pair.currentHarvestKj) && known(pair.comparisonHarvestKj)) {
            pick(pair.currentHarvestKj, pair.comparisonHarvestKj, true, 0.5)
        } else {
            null
        },
    )
    // Corner exit. Back on the throttle earlier wins unless it spins the
    // rears, which hands it to the other lap; flat out sooner only counts
    // without wheelspin.
    rows += HeadToHeadRow(
        "Throttle on", current?.let { metresText(it.throttleOnM) }, comparison?.let { metresText(it.throttleOnM) },
        if (both) exitBetter(current!!.throttleOnM, comparison!!.throttleOnM, current.wheelspin, comparison.wheelspin) else null,
        warnCurrent = current?.let { it.wheelspin && it.throttleOnM >= 0 } == true,
        warnComparison = comparison?.let { it.wheelspin && it.throttleOnM >= 0 } == true,
    )
    rows += HeadToHeadRow(
        "Full throttle", current?.let { metresText(it.fullThrottleM) }, comparison?.let { metresText(it.fullThrottleM) },
        if (both) exitBetter(current!!.fullThrottleM, comparison!!.fullThrottleM, current.wheelspin, comparison.wheelspin) else null,
    )
    rows += HeadToHeadRow(
        "0 → 100%", current?.let { metresText(it.throttleToFullM) }, comparison?.let { metresText(it.throttleToFullM) },
        if (both) exitBetter(current!!.throttleToFullM, comparison!!.throttleToFullM, current.wheelspin, comparison.wheelspin) else null,
    )
    rows += HeadToHeadRow(
        "Wheelspin", current?.let(::spinText), comparison?.let(::spinText),
        if (both && (current!!.wheelspin || comparison!!.wheelspin)) {
            when {
                current.wheelspin && comparison.wheelspin -> pick(current.wheelspinPct, comparison.wheelspinPct, false, 0.5)
                current.wheelspin -> 1
                else -> 0
            }
        } else {
            null
        },
    )
    val timed = pair != null && pair.currentTimeS >= 0 && pair.comparisonTimeS >= 0
    if (timed) {
        rows += HeadToHeadRow(
            "Time", "${seconds(pair!!.currentTimeS)} s", "${seconds(pair.comparisonTimeS)} s",
            pick(pair.currentTimeS, pair.comparisonTimeS, false, BRAKING_LEVEL_S / 2),
        )
    }
    return rows.filter { it.current != null || it.comparison != null }
}

// Apexes this close on two laps are the same corner.
private const val CORNER_MATCH_M = 80.0

/**
 * Two laps' corners lined up: each current-lap apex with the comparison
 * lap's nearest within [CORNER_MATCH_M], and any left over on their own, in
 * order along the lap.
 */
private fun alignCorners(a: List<BrakingApex>, b: List<BrakingApex>): List<Pair<BrakingApex?, BrakingApex?>> {
    val taken = HashSet<Int>()
    val rows = ArrayList<Pair<BrakingApex?, BrakingApex?>>()
    for (apex in a) {
        var best = -1
        var bestGap = CORNER_MATCH_M
        b.forEachIndexed { index, other ->
            val gap = abs(other.distanceM - apex.distanceM)
            if (index !in taken && gap <= bestGap) {
                best = index
                bestGap = gap
            }
        }
        if (best >= 0) taken += best
        rows += apex to b.getOrNull(best)
    }
    b.forEachIndexed { index, apex -> if (index !in taken) rows += null to apex }
    return rows.sortedBy { (x, y) -> (x ?: y)!!.distanceM }
}

/**
 * The better corner exit point (0 current, 1 comparison): the earlier one
 * when neither lap spun, the clean lap when only one did, and none when both
 * did, since an exit that spins cannot count as the better one.
 */
private fun exitBetter(a: Double, b: Double, aSpun: Boolean, bSpun: Boolean): Int? = when {
    a < 0 || b < 0 || (aSpun && bSpun) -> null
    aSpun != bSpun -> if (aSpun) 1 else 0
    abs(a - b) < 0.5 -> null
    a < b -> 0
    else -> 1
}

/** Words one of libtnrp's observations with the laps' names, as the desktop does. */
private fun observationText(observation: BrakingObservation, laps: BrakingLaps): String {
    val labels = listOf(laps.currentLabel, laps.comparisonLabel)
    val lap = labels.getOrElse(observation.lap) { labels[0] }
    val other = labels.getOrElse(1 - observation.lap) { labels[1] }
    val value = observation.value
    val second = observation.other
    fun tenth(x: Double) = String.format(Locale.US, "%.1f", x)
    return when (observation.kind) {
        "quicker_despite_coast" -> "$lap is ${seconds(value)} s quicker despite lifting ${tenth(second)} s earlier"
        "apex_speed" -> "$lap carries ${(value - second).roundToInt()} km/h more through the apex (${value.roundToInt()} vs ${second.roundToInt()})"
        "later_but_slower" -> "$lap brakes ${value.roundToInt()} m later but is ${seconds(second)} s slower"
        "more_harvest" -> "$lap harvests ${(value - second).roundToInt()} kJ more than $other"
        "low_battery" -> "$lap starts the zone with a near-empty battery (${String.format(Locale.US, "%.2f", value)} MJ)"
        "wheelspin" -> "$lap spins the rears on exit (${value.roundToInt()}% for ${tenth(second)} s)"
        "earlier_throttle_spin" -> "$lap is back on throttle ${value.roundToInt()} m earlier but spins the rears (${second.roundToInt()}%)"
        "earlier_throttle" -> "$lap is back on throttle ${value.roundToInt()} m earlier than $other"
        "faster_to_full" -> "$lap is flat out ${value.roundToInt()} m sooner than $other"
        "lift_only" -> "$lap takes it on a lift where $other brakes"
        "tighter_track_limits" -> "$lap runs ${tenth(value)} m further inside at the apex but breaks track limits"
        "straighter_line" -> "$lap runs a wider arc, ${tenth(value)} m further inside at the apex" + when {
            second > 0 -> ", and is ${seconds(second)} s quicker"
            second < 0 -> ", but is ${seconds(-second)} s slower"
            else -> ""
        }
        "tighter_helped" -> "$lap takes a ${tenth(value)} m tighter line and is ${seconds(second)} s quicker"
        "tighter_no_gain" -> "$lap's ${tenth(value)} m tighter line gains no time"
        "tighter_lost_speed" -> "$lap takes a ${tenth(value)} m tighter line but loses ${second.roundToInt()} km/h through the apex"
        "tighter_wheelspin" -> "$lap takes a ${tenth(value)} m tighter line but spins the rears on exit (${second.roundToInt()}%)"
        "tighter_slower_exit" -> "$lap takes a ${tenth(value)} m tighter line but is on the power ${second.roundToInt()} m later"
        "tighter_slower" -> "$lap takes a ${tenth(value)} m tighter line but is ${seconds(second)} s slower"
        "track_limits" -> "$lap breaks track limits here"
        else -> observation.kind.replace('_', ' ')
    }
}

/** A lap distance: "1,930 m". */
internal fun formatMetres(metres: Double): String = String.format(Locale.US, "%,d m", metres.roundToLong())

private fun seconds(value: Double) = String.format(Locale.US, "%.3f", value)

/** Where a lap braked, or that it only lifted (and where). */
private fun brakeAtText(point: BrakingPoint) =
    if (point.liftOnly) "lift ${formatMetres(point.distanceM)}" else formatMetres(point.distanceM)

private fun metresText(metres: Double) = if (metres < 0) "—" else formatMetres(metres)

private fun coastText(point: BrakingPoint) =
    if (point.coastS > 0) "${String.format(Locale.US, "%.1f", point.coastS)} s · ${point.coastM.roundToInt()} m" else "none"

private fun spinText(point: BrakingPoint) =
    if (point.wheelspin) "${point.wheelspinPct.roundToInt()}% · ${String.format(Locale.US, "%.1f", point.wheelspinS)} s" else "none"

private fun harvestText(kj: Double) = String.format(Locale.US, "+%,d kJ", kj.roundToLong())

/** A lap's line against the other's at a corner's apex, named on the lap further inside. */
private fun lineText(gapM: Double) = when {
    gapM == 0.0 -> "level"
    gapM > 0 -> "${String.format(Locale.US, "%.1f", gapM)} m inside"
    else -> "—"
}

/** Which way a corner turns, after its apex label. */
private fun sideText(side: Int) = when {
    side < 0 -> " · L"
    side > 0 -> " · R"
    else -> ""
}
