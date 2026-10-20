package com.tracknrace.android.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentWidth
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.tracknrace.android.StrategyPlan
import com.tracknrace.android.StrategyRival
import com.tracknrace.android.StrategySnapshot
import com.tracknrace.android.StrategyStint
import com.tracknrace.android.StrategyWeather
import com.tracknrace.android.TelemetryStore
import java.util.Locale
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.max

// ── Strategy ────────────────────────────────────────────────────────────────
// Material 3, no cards, one screen in either orientation (design "1, race
// strip"):
//
//   plan       Defend or Attack (a segmented button); the picked plan's next
//              stop and the next lap's target time, each a title; then what
//              goes on and who it covers or chases, and a call when there is
//              one (Safety Car or VSC, race call, weather, tyre alert)
//   race plan  both plans as thin rails over the race distance, each stop a
//              tick, the current lap a short line across the two rails
//   pace       the picked plan's current stint, lap by lap, as a line against
//              its targets: above zero losing time (error tone), below
//              gaining; seconds up the side, laps along the bottom
//   stats      position, stint and race delta, the cars ahead and behind
//
// Portrait stacks them; landscape puts the plan in one header row. Nothing
// scrolls. The snapshot changes a few times a lap at most.

private enum class PlanChoice(val title: String) { DEFEND("Defend"), ATTACK("Attack") }

@Composable
internal fun StrategyTab(store: TelemetryStore, landscape: Boolean) {
    val strategy by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.strategy } }
    val sessionType by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.cold.sessionType } }
    val stale by remember(store) { derivedStateOf { store.strategyStale } }
    var chosen by rememberSaveable { mutableIntStateOf(PlanChoice.DEFEND.ordinal) }
    val choice = PlanChoice.entries[chosen]
    val choose = { plan: PlanChoice -> chosen = plan.ordinal }
    val s = strategy
    when {
        s?.state == "non_race" || (s == null && sessionType != null && sessionType !in RaceSessionTypes) ->
            StrategyMessage("Race sessions only", "Strategy suggestions are available during Race, Race 2 and Race 3 sessions.")
        s == null || !s.ready -> StrategyCalculating()
        else -> Column(Modifier.fillMaxSize()) {
            if (stale) RecalculatingBar(s.lapNumber)
            // The previous cursor's plans stay readable but dimmed until the
            // rebuilt snapshot replaces them.
            Box(Modifier.weight(1f).graphicsLayer { alpha = if (stale) 0.45f else 1f }) {
                if (landscape) LandscapeStrategy(s, choice, choose) else PortraitStrategy(s, choice, choose)
            }
        }
    }
}

/** Shown over the previous plans while a seek's rebuild runs. */
@Composable
private fun RecalculatingBar(lap: Int) {
    val scheme = MaterialTheme.colorScheme
    Row(
        Modifier
            .fillMaxWidth()
            .padding(start = 16.dp, end = 16.dp, top = 12.dp)
            .background(scheme.surfaceContainerHigh, RoundedCornerShape(12.dp))
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
        Text("Recalculating after seek", Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium, color = scheme.onSurface, maxLines = 1)
        Text("Showing lap $lap", style = MaterialTheme.typography.bodySmall, color = scheme.onSurfaceVariant, maxLines = 1)
    }
}

@Composable
private fun PortraitStrategy(s: StrategySnapshot, choice: PlanChoice, choose: (PlanChoice) -> Unit) {
    val pace = paceFacts(s, choice)
    Column(Modifier.fillMaxSize().padding(start = 16.dp, end = 16.dp, top = 16.dp, bottom = 12.dp)) {
        PlanSwitcher(choice, choose, Modifier.fillMaxWidth())
        Row(Modifier.fillMaxWidth().padding(top = 16.dp), verticalAlignment = Alignment.Bottom) {
            Column(Modifier.weight(1f)) {
                Text("Next stop", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(stopHeadline(s, choice), style = MaterialTheme.typography.titleLarge, color = MaterialTheme.colorScheme.onSurface, maxLines = 1)
            }
            NextLapTarget(pace)
        }
        PlanDetail(s, choice, Modifier.padding(top = 2.dp))
        Column(Modifier.fillMaxWidth().weight(1f), verticalArrangement = Arrangement.Center) {
            RaceStrip(s, choice, gutter = 56.dp)
            PaceChart(s, choice, pace, Modifier.fillMaxWidth().padding(top = 16.dp).weight(1f, fill = false).heightIn(min = 72.dp, max = 132.dp))
        }
        HorizontalDivider(Modifier.padding(horizontal = 0.dp), color = MaterialTheme.colorScheme.outlineVariant)
        PortraitStats(s, pace, Modifier.fillMaxWidth().padding(top = 12.dp))
    }
}

@Composable
private fun LandscapeStrategy(s: StrategySnapshot, choice: PlanChoice, choose: (PlanChoice) -> Unit) {
    val pace = paceFacts(s, choice)
    Column(Modifier.fillMaxSize().padding(horizontal = 24.dp, vertical = 12.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(24.dp)) {
            PlanSwitcher(choice, choose, Modifier.width(220.dp))
            Column(Modifier.weight(1f)) {
                Text(stopHeadline(s, choice), style = MaterialTheme.typography.titleLarge, color = MaterialTheme.colorScheme.onSurface, maxLines = 1)
                PlanDetail(s, choice)
            }
            NextLapTarget(pace)
        }
        Column(Modifier.fillMaxWidth().weight(1f).padding(vertical = 8.dp), verticalArrangement = Arrangement.Center) {
            RaceStrip(s, choice, gutter = 64.dp)
            PaceChart(s, choice, pace, Modifier.fillMaxWidth().padding(top = 12.dp).weight(1f, fill = false).heightIn(min = 48.dp, max = 96.dp))
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        LandscapeStats(s, pace, Modifier.fillMaxWidth().padding(top = 10.dp))
    }
}

@Composable
private fun StrategyMessage(title: String, body: String) {
    val scheme = MaterialTheme.colorScheme
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(title, style = MaterialTheme.typography.titleLarge, color = scheme.onSurface, textAlign = TextAlign.Center)
        Text(body, Modifier.padding(top = 8.dp), style = MaterialTheme.typography.bodyMedium, color = scheme.onSurfaceVariant, textAlign = TextAlign.Center)
    }
}

/** Before the first ready snapshot: the engine replays the race so far first. */
@Composable
private fun StrategyCalculating() {
    val scheme = MaterialTheme.colorScheme
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        CircularProgressIndicator(Modifier.size(32.dp), strokeWidth = 3.dp)
        Text("Calculating strategy", Modifier.padding(top = 20.dp), style = MaterialTheme.typography.titleLarge, color = scheme.onSurface)
        Text("Plans appear once the race so far has been read.", Modifier.padding(top = 8.dp), style = MaterialTheme.typography.bodyMedium, color = scheme.onSurfaceVariant, textAlign = TextAlign.Center)
    }
}

// ── Plan ────────────────────────────────────────────────────────────────────

@Composable
private fun PlanSwitcher(choice: PlanChoice, onChoose: (PlanChoice) -> Unit, modifier: Modifier) {
    SingleChoiceSegmentedButtonRow(modifier) {
        PlanChoice.entries.forEachIndexed { index, plan ->
            SegmentedButton(
                selected = plan == choice,
                onClick = { onChoose(plan) },
                shape = SegmentedButtonDefaults.itemShape(index, PlanChoice.entries.size),
            ) {
                Text(plan.title, maxLines = 1)
            }
        }
    }
}

private fun stopHeadline(s: StrategySnapshot, choice: PlanChoice): String {
    val stop = s.plan(choice).nextStopLap(s.lapNumber)
    return when {
        stop == null -> "No more stops"
        stop <= s.lapNumber -> "Box this lap"
        else -> "Box lap $stop"
    }
}

/** What the stop leads to and why, then the current call when there is one. */
@Composable
private fun PlanDetail(s: StrategySnapshot, choice: PlanChoice, modifier: Modifier = Modifier) {
    val c = LocalDashPalette.current
    val scheme = MaterialTheme.colorScheme
    val plan = s.plan(choice)
    val stop = plan.nextStopLap(s.lapNumber)
    val next = plan.stints.firstOrNull { it.startLap > s.lapNumber }
    val text = buildList {
        when {
            stop == null -> add("To the flag")
            stop > s.lapNumber -> add("In ${stop - s.lapNumber} lap${if (stop - s.lapNumber == 1) "" else "s"}")
        }
        if (stop != null && next != null) add("onto ${tyreName(next.visualCompound, next.compoundName)}")
        add(
            when {
                plan.targetIndex < 0 -> "managing tyre life"
                choice == PlanChoice.DEFEND -> "covering ${titleCase(plan.targetName)}"
                else -> "chasing ${titleCase(plan.targetName)}"
            },
        )
        if (!plan.legal) add("no legal tyre path")
    }.joinToString(" · ").replaceFirstChar { it.titlecase(Locale.US) }
    Column(modifier) {
        Text(text, style = MaterialTheme.typography.bodyMedium, color = scheme.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
        decisionFor(c, s)?.let { d ->
            val line = listOf(d.kind, d.headline, d.value).filter { it.isNotEmpty() }.joinToString(" · ")
            Text(line, style = MaterialTheme.typography.labelLarge, color = d.ink, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun NextLapTarget(pace: PaceFacts) {
    Column(horizontalAlignment = Alignment.End) {
        Text("Next lap target", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
        Text(
            pace.nextMs?.let { lapTime(it) } ?: "—",
            style = MaterialTheme.typography.titleLarge,
            color = if (pace.nextMs != null) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.outline,
            maxLines = 1,
        )
    }
}

// ── Call ────────────────────────────────────────────────────────────────────

private class Decision(
    val kind: String,
    val headline: String,
    val value: String,
    val ink: Color,
)

/** The one thing to act on, most urgent first; null when there is none. */
private fun decisionFor(c: DashPalette, s: StrategySnapshot): Decision? {
    s.neutralisation?.let { n ->
        val projected = if (n.box) n.projectedBoxPosition else n.projectedStayPosition
        return Decision(
            kind = if (n.safetyCar) "Safety car" else "Virtual safety car",
            headline = if (n.box) "Box now" else "Stay out",
            value = "P${n.currentPosition} → P$projected",
            ink = if (n.box) c.gain else c.caution,
        )
    }
    s.call?.let { call ->
        return Decision(
            kind = "Race call",
            headline = "${sentence(call.kind)} ${titleCase(call.targetName)}",
            value = "${fixed(call.gapMs / 1000f, 1)} s",
            ink = if (call.kind == "overcut") c.gain else c.caution,
        )
    }
    s.weather?.takeIf(::weatherShown)?.let { w ->
        return Decision(
            kind = "Weather",
            headline = sentence(w.recommendation),
            value = if (w.crossoverLap > 0) "lap ${w.crossoverLap} · ${w.rainPercentage}% rain" else "",
            ink = c.cold,
        )
    }
    s.warnings.firstOrNull()?.let { warning ->
        val (scope, detail) = wearWarningParts(warning.text)
        return Decision(
            kind = "Tyre alert",
            headline = if (scope.isEmpty()) sentence(detail) else "$scope ${detail.lowercase(Locale.US)}",
            value = "",
            ink = when (warning.severity) {
                "danger" -> c.loss
                "warning" -> c.tyreBands[3]
                else -> c.caution
            },
        )
    }
    return null
}

// ── Race strip ──────────────────────────────────────────────────────────────

/**
 * Both plans as thin rails over the race distance, one stint per segment in
 * its compound's colour; each stop still to come a tick; the current lap a
 * short line across the two rails. The plan not picked is faded.
 */
@Composable
private fun RaceStrip(s: StrategySnapshot, choice: PlanChoice, gutter: Dp) {
    val scheme = MaterialTheme.colorScheme
    val tones = rememberStrategyTones()
    val total = s.totalLaps.takeIf { it > 0 }
        ?: max(s.defensive.stints.maxOfOrNull { it.endLap } ?: 0, s.attacking.stints.maxOfOrNull { it.endLap } ?: 0)
    if (total <= 0) return
    fun fraction(lap: Float) = ((lap - 1f) / total).coerceIn(0f, 1f)
    Column {
        // Lap axis: the start, every ten laps, the race length; the current
        // lap in the primary tone, and no label crowding it.
        BoxWithConstraints(Modifier.fillMaxWidth().padding(start = gutter).height(16.dp)) {
            val ticks = (listOf(1) + (10..total step 10).filter { total - it >= 5 } + total).distinct()
            val now = s.lapNumber
            val crowd = 4f / total * 52f
            for (lap in ticks) {
                if (abs(lap - now) < crowd) continue
                AxisLabel("$lap", maxWidth * fraction(lap.toFloat()), scheme.outline, lap == total)
            }
            AxisLabel("$now", maxWidth * fraction(now.toFloat()), scheme.primary, false)
        }
        Box {
            Column {
                PlanChoice.entries.forEach { kind ->
                    val picked = kind == choice
                    Row(Modifier.height(20.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            kind.title,
                            Modifier.width(gutter),
                            style = MaterialTheme.typography.labelMedium,
                            color = if (picked) scheme.onSurface else scheme.onSurfaceVariant,
                            maxLines = 1,
                        )
                        val plan = s.plan(kind)
                        val stopInk = scheme.onSurface
                        Spacer(
                            Modifier.weight(1f).fillMaxHeight().graphicsLayer { alpha = if (picked) 1f else 0.38f }.drawBehind {
                                val gap = 4.dp.toPx()
                                val rail = 4.dp.toPx()
                                val top = (size.height - rail) / 2f
                                for (stint in plan.stints) {
                                    val left = fraction(stint.startLap.toFloat()) * size.width + if (stint.startLap > 1) gap / 2f else 0f
                                    val right = fraction(stint.endLap + 1f) * size.width - if (stint.endLap < total) gap / 2f else 0f
                                    if (right <= left) continue
                                    drawRoundRect(
                                        tones.compound(stint.actualCompound, stint.visualCompound),
                                        Offset(left, top), Size(right - left, rail), CornerRadius(rail / 2f),
                                    )
                                }
                                // Stops still to come.
                                for (stint in plan.stints) {
                                    if (stint.isLast || stint.endLap < s.lapNumber) continue
                                    val x = fraction(stint.endLap + 1f) * size.width
                                    drawRoundRect(
                                        stopInk, Offset(x - 1.dp.toPx(), (size.height - 16.dp.toPx()) / 2f),
                                        Size(2.dp.toPx(), 16.dp.toPx()), CornerRadius(1.dp.toPx()),
                                    )
                                }
                            },
                        )
                    }
                }
            }
            // The current lap, across the two rails only.
            val primary = scheme.primary
            Spacer(
                Modifier.matchParentSize().padding(start = gutter, top = 2.dp, bottom = 2.dp).drawBehind {
                    val x = fraction(s.lapNumber + 0.5f) * size.width
                    drawRoundRect(primary, Offset(x - 1.dp.toPx(), 0f), Size(2.dp.toPx(), size.height), CornerRadius(1.dp.toPx()))
                },
            )
        }
    }
}

/** A label centred on [x], kept inside its row; [end] aligns it to the right edge. */
@Composable
private fun AxisLabel(text: String, x: Dp, color: Color, end: Boolean) {
    Box(Modifier.fillMaxWidth()) {
        Text(
            text,
            Modifier
                .then(if (end) Modifier.align(Alignment.CenterEnd) else Modifier.offset(x = x - 6.dp))
                .wrapContentWidth(unbounded = true),
            style = MaterialTheme.typography.labelSmall,
            color = color,
            maxLines = 1,
        )
    }
}

// ── Pace against the plan ───────────────────────────────────────────────────

/** The picked plan's stint the car is in, and how the laps run so far compare. */
private class PaceFacts(
    val stint: StrategyStint?,
    val nextMs: Int?,
    val stintMs: Int?,
    val raceMs: Int?,
)

private fun paceFacts(s: StrategySnapshot, choice: PlanChoice): PaceFacts {
    val plan = s.plan(choice)
    val stint = plan.stints.firstOrNull { s.lapNumber in it.startLap..it.endLap } ?: plan.stints.lastOrNull()
    val rows = stint?.rows.orEmpty()
    val run = rows.filter { it.hasActual && it.requiredMs > 0 }
    return PaceFacts(
        stint = stint,
        nextMs = rows.firstOrNull { !it.hasActual && it.lapNumber >= s.lapNumber && it.requiredMs > 0 }?.requiredMs,
        stintMs = run.lastOrNull()?.deltaStintMs,
        raceMs = run.lastOrNull()?.deltaTotalMs,
    )
}

/**
 * Each lap of the stint run so far against the picked plan's target, as a
 * line: above zero losing time, below gaining, the colour changing where it
 * crosses. Seconds up the side, the stint's laps along the bottom.
 */
@Composable
private fun PaceChart(s: StrategySnapshot, choice: PlanChoice, pace: PaceFacts, modifier: Modifier) {
    val scheme = MaterialTheme.colorScheme
    val tones = rememberStrategyTones()
    val stint = pace.stint
    val points = stint?.rows.orEmpty().filter { it.hasActual && it.requiredMs > 0 }
    // Whole seconds, at least two, enough for the largest lap delta.
    val range = max(2, ceil((points.maxOfOrNull { abs(it.deltaLapMs) } ?: 0) / 1000.0).toInt())
    // The stint's laps along the bottom, each labelled lap a vertical gridline.
    val xLaps = stint?.let { st ->
        val span = max(1, st.endLap - st.startLap)
        val step = when {
            span <= 12 -> 2
            span <= 30 -> 4
            else -> 8
        }
        ((st.startLap until st.endLap step step).toList() + st.endLap).distinct()
            .filterIndexed { index, lap -> index == 0 || lap == st.endLap || st.endLap - lap >= step / 2 }
    }.orEmpty()
    Column(modifier) {
        Row(verticalAlignment = Alignment.Bottom) {
            Text("Pace vs ${choice.title}", Modifier.weight(1f), style = MaterialTheme.typography.titleSmall, color = scheme.onSurface, maxLines = 1)
            if (stint != null) {
                Text(
                    "${tyreName(stint.visualCompound, stint.compoundName)} · laps ${stint.startLap}–${stint.endLap}",
                    style = MaterialTheme.typography.labelSmall,
                    color = scheme.outline,
                    maxLines = 1,
                )
            }
        }
        Row(Modifier.fillMaxWidth().weight(1f, fill = false).padding(top = 4.dp)) {
            // Seconds, aligned to the gridlines drawn beside them.
            BoxWithConstraints(Modifier.width(32.dp).fillMaxHeight()) {
                val half = maxHeight / 2
                val inset = 8.dp
                val axis = half - inset
                for ((value, label) in listOf(range to "+$range s", 0 to "0", -range to "−$range s")) {
                    Text(
                        label,
                        Modifier.align(Alignment.TopEnd).offset(y = half - axis * (value.toFloat() / range) - 8.dp),
                        style = MaterialTheme.typography.labelSmall,
                        color = scheme.outline,
                        maxLines = 1,
                    )
                }
            }
            Column(Modifier.weight(1f).padding(start = 8.dp)) {
                val grid = scheme.surfaceContainerHigh
                val zeroLine = scheme.outlineVariant
                Spacer(
                    Modifier.fillMaxWidth().weight(1f).semantics { contentDescription = "Lap times against the plan" }.drawBehind {
                        if (stint == null) return@drawBehind
                        val inset = 8.dp.toPx()
                        val zero = size.height / 2f
                        val scale = (zero - inset) / (range * 1000f)
                        val span = max(1, stint.endLap - stint.startLap).toFloat()
                        fun x(lap: Int) = (lap - stint.startLap) / span * size.width
                        fun y(ms: Int) = zero - ms * scale
                        val hair = 1.dp.toPx()
                        drawLine(grid, Offset(0f, inset), Offset(size.width, inset), hair)
                        drawLine(grid, Offset(0f, size.height - inset), Offset(size.width, size.height - inset), hair)
                        for (lap in xLaps) {
                            val gx = x(lap)
                            drawLine(grid, Offset(gx, inset), Offset(gx, size.height - inset), hair)
                        }
                        drawLine(zeroLine, Offset(0f, zero), Offset(size.width, zero), hair)
                        if (points.isEmpty()) return@drawBehind
                        val stroke = 2.dp.toPx()
                        fun ink(ms: Int) = if (ms > 0) tones.losing else tones.gaining
                        // Segment by segment, split where the line crosses zero.
                        for (i in 1 until points.size) {
                            val a = points[i - 1]
                            val b = points[i]
                            val start = Offset(x(a.lapNumber), y(a.deltaLapMs))
                            val end = Offset(x(b.lapNumber), y(b.deltaLapMs))
                            if ((a.deltaLapMs > 0) != (b.deltaLapMs > 0) && a.deltaLapMs != b.deltaLapMs) {
                                val t = a.deltaLapMs / (a.deltaLapMs - b.deltaLapMs).toFloat()
                                val cross = Offset(start.x + (end.x - start.x) * t, zero)
                                drawLine(ink(a.deltaLapMs), start, cross, stroke, StrokeCap.Round)
                                drawLine(ink(b.deltaLapMs), cross, end, stroke, StrokeCap.Round)
                            } else {
                                drawLine(ink(b.deltaLapMs), start, end, stroke, StrokeCap.Round)
                            }
                        }
                        val last = points.last()
                        drawCircle(ink(last.deltaLapMs), 4.dp.toPx(), Offset(x(last.lapNumber), y(last.deltaLapMs)))
                    },
                )
                if (stint != null) {
                    BoxWithConstraints(Modifier.fillMaxWidth().height(16.dp)) {
                        val span = max(1, stint.endLap - stint.startLap).toFloat()
                        for (lap in xLaps) {
                            AxisLabel("$lap", maxWidth * ((lap - stint.startLap) / span), scheme.outline, lap == stint.endLap)
                        }
                    }
                }
            }
        }
    }
}

// ── Stats ───────────────────────────────────────────────────────────────────

@Composable
private fun Stat(label: String, value: String, color: Color = MaterialTheme.colorScheme.onSurface, modifier: Modifier = Modifier) {
    Column(modifier) {
        Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
        Text(value, style = MaterialTheme.typography.titleMedium, color = color, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

private class StatValues(s: StrategySnapshot, pace: PaceFacts, tones: StrategyTones, scheme: ColorScheme) {
    val position = if (s.playerPosition > 0) "P${s.playerPosition}" else "P–"
    val stint = pace.stintMs?.let { "${delta(it)} s" } ?: "—"
    val stintInk = pace.stintMs?.let { if (it > 0) tones.losing else tones.gaining } ?: scheme.outline
    val race = pace.raceMs?.let { "${delta(it)} s" } ?: "—"
    val raceInk = pace.raceMs?.let { if (it > 0) tones.losing else tones.gaining } ?: scheme.outline
    val ahead = s.rivals.filter { it.ahead }.minByOrNull { it.gapMs }
    val behind = s.rivals.filter { !it.ahead }.minByOrNull { it.gapMs }
}

private fun rivalText(rival: StrategyRival?, sign: String) =
    rival?.let { "${titleCase(it.name)} $sign${fixed(it.gapMs / 1000f, 1)} s" } ?: "—"

@Composable
private fun PortraitStats(s: StrategySnapshot, pace: PaceFacts, modifier: Modifier) {
    val v = StatValues(s, pace, rememberStrategyTones(), MaterialTheme.colorScheme)
    Column(modifier, verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Stat("Position", "${v.position} · L${s.lapNumber}/${s.totalLaps}", modifier = Modifier.weight(1f))
            Stat("Stint", v.stint, v.stintInk, Modifier.weight(1f))
            Stat("Race", v.race, v.raceInk, Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Stat(v.ahead?.let { "Ahead · P${it.position}" } ?: "Ahead", rivalText(v.ahead, "−"), modifier = Modifier.weight(1f))
            Stat(v.behind?.let { "Behind · P${it.position}" } ?: "Behind", rivalText(v.behind, "+"), modifier = Modifier.weight(1f))
        }
    }
}

@Composable
private fun LandscapeStats(s: StrategySnapshot, pace: PaceFacts, modifier: Modifier) {
    val v = StatValues(s, pace, rememberStrategyTones(), MaterialTheme.colorScheme)
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(32.dp)) {
        Stat("Position", "${v.position} · Lap ${s.lapNumber} of ${s.totalLaps}")
        Stat("Stint", v.stint, v.stintInk)
        Stat("Race", v.race, v.raceInk)
        Spacer(Modifier.weight(1f))
        Stat(v.ahead?.let { "Ahead · P${it.position}" } ?: "Ahead", rivalText(v.ahead, "−"))
        Stat(v.behind?.let { "Behind · P${it.position}" } ?: "Behind", rivalText(v.behind, "+"))
    }
}

// ── Tones ───────────────────────────────────────────────────────────────────

/**
 * Muted compound and delta colours for the dark theme, so the rails and the
 * pace line read as information rather than paint; the light theme keeps the
 * dashboard's own, which are already dark enough to sit on a light surface.
 */
@Immutable
private class StrategyTones(private val c: DashPalette, private val dark: Boolean, val losing: Color, val gaining: Color) {
    fun compound(actual: Int, visual: Int): Color {
        if (!dark) return compoundColor(c, actual, visual)
        return when (actual) {
            11 -> Color(0xffb69ae0)
            12 -> Color(0xffe0857c)
            13 -> Color(0xffe3cd7a)
            14 -> Color(0xffd0d0d0)
            15 -> Color(0xff8fb4e8)
            else -> when (visual) {
                16 -> Color(0xffe0857c)
                17 -> Color(0xffe3cd7a)
                18 -> Color(0xffd0d0d0)
                7 -> Color(0xff8cc9a0)
                8 -> Color(0xff8fb4e8)
                else -> c.med
            }
        }
    }
}

@Composable
private fun rememberStrategyTones(): StrategyTones {
    val c = LocalDashPalette.current
    val scheme = MaterialTheme.colorScheme
    return remember(c, scheme) {
        val dark = scheme.surface.luminance() < 0.5f
        StrategyTones(c, dark, losing = scheme.error, gaining = if (dark) Color(0xff8cc9a0) else c.gain)
    }
}

// ── Helpers ─────────────────────────────────────────────────────────────────

private fun StrategySnapshot.plan(kind: PlanChoice): StrategyPlan =
    if (kind == PlanChoice.DEFEND) defensive else attacking

/** The tyre as a driver says it: Soft, Medium, Hard, Inters, Wets. */
private fun tyreName(visual: Int, fallback: String): String = when (visual) {
    16 -> "Softs"
    17 -> "Mediums"
    18 -> "Hards"
    7 -> "Inters"
    8 -> "Wets"
    else -> fallback
}

/** "VERSTAPPEN" as "Verstappen". */
private fun titleCase(name: String): String =
    name.lowercase(Locale.US).split(' ').joinToString(" ") { part -> part.replaceFirstChar { it.titlecase(Locale.US) } }

/**
 * F2 colours follow the actual compound, F1's the visual one, as on the
 * desktop. Supersoft has no F1 counterpart, so it takes the purple.
 */
private fun compoundColor(c: DashPalette, actual: Int, visual: Int): Color = when (actual) {
    11 -> c.best
    12 -> c.loss
    13 -> c.caution
    14 -> c.hard
    15 -> c.cold
    else -> when (visual) {
        16 -> c.loss
        17 -> c.caution
        18 -> c.hard
        7 -> c.gain
        8 -> c.cold
        else -> c.med
    }
}

/** The desktop shows the window only once it has a crossover or a set to compare. */
private fun weatherShown(w: StrategyWeather) = w.crossoverLap > 0 || w.lapDeltaMs != null

private fun fixed(value: Float, digits: Int) = "%.${digits}f".format(Locale.US, value)

private fun lapTime(milliseconds: Int): String {
    if (milliseconds <= 0) return "—"
    val seconds = milliseconds / 1000.0
    return "%d:%04.1f".format(Locale.US, (seconds / 60).toInt(), seconds % 60)
}

private fun delta(milliseconds: Int): String {
    val sign = when {
        milliseconds > 0 -> "+"
        milliseconds < 0 -> "−"
        else -> ""
    }
    return sign + "%.1f".format(Locale.US, abs(milliseconds) / 1000.0)
}

/** "box_under_safety_car" as "Box under safety car". */
private fun sentence(value: String): String =
    value.replace('_', ' ').replaceFirstChar { it.titlecase(Locale.US) }

private fun shortTyreScope(value: String): String = value
    .replace(Regex("front-left", RegexOption.IGNORE_CASE), "FL")
    .replace(Regex("front-right", RegexOption.IGNORE_CASE), "FR")
    .replace(Regex("rear-left", RegexOption.IGNORE_CASE), "RL")
    .replace(Regex("rear-right", RegexOption.IGNORE_CASE), "RR")
    .replace(Regex("\\s+tyres?$", RegexOption.IGNORE_CASE), "")
    .replace(Regex("\\s+and\\s+", RegexOption.IGNORE_CASE), " + ")
    .uppercase(Locale.US)

private fun tyreScopeLabel(value: String): String {
    val scope = shortTyreScope(value)
    val plural = Regex("tyres", RegexOption.IGNORE_CASE).containsMatchIn(value) || scope.contains('+')
    return "$scope ${if (plural) "TYRES" else "TYRE"}"
}

/** Splits a wear warning into the tyres it names and what it says, as the desktop does. */
private fun wearWarningParts(text: String): Pair<String, String> {
    fun match(pattern: String) = Regex(pattern, RegexOption.IGNORE_CASE).find(text)?.groupValues
    match("^(.+?) is the limiting tyre$")?.let { return tyreScopeLabel(it[1]) to "is limiting" }
    match("^(.+?) wear far above average$")?.let { return tyreScopeLabel(it[1]) to "wear far above average" }
    match("^(.+?) wearing faster than (.+)$")?.let {
        return tyreScopeLabel(it[1]) to "High wear rate vs ${shortTyreScope(it[2])}"
    }
    match("^(.+?) wearing faster$")?.let { return tyreScopeLabel(it[1]) to "High wear rate" }
    return "" to text
}
