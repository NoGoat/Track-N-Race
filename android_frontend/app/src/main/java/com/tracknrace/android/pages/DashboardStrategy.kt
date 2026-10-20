package com.tracknrace.android.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentHeight
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.tracknrace.android.StrategyPlan
import com.tracknrace.android.StrategyRival
import com.tracknrace.android.StrategySnapshot
import com.tracknrace.android.StrategyWeather
import com.tracknrace.android.TelemetryStore
import java.util.Locale
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.roundToInt

// ── Strategy ────────────────────────────────────────────────────────────────
// The desktop Strategy page as a glance screen, built like Drive: a status
// strip, then what to do, then why.
//
//   status    lap, position, fitted tyre and its wear, the tyre cliff
//   decision  the one thing to act on: a Safety Car or VSC call, the race
//             call, the weather window or a tyre alert, else "hold the plan"
//   plans     Defend and Attack, each led by its next stop
//   timeline  both plans as lanes over the race distance, with the current
//             lap, the cliff and the weather crossover marked; tapping a lane
//             picks the plan the pace chart follows
//   pace      this stint's laps against the picked plan's targets
//   tyres     each corner's remaining life
//   battle    the cars either side of you, with the gap and how it moves
//
// Landscape fits it all on one screen in two columns; portrait scrolls. The
// snapshot changes a few times a lap at most, so cards are cached as textures.

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
                StrategyContent(s, landscape, choice, choose)
            }
        }
    }
}

/** Shown over the previous plans while a seek's rebuild runs. */
@Composable
private fun RecalculatingBar(lap: Int) {
    val c = LocalDashPalette.current
    Row(
        Modifier
            .fillMaxWidth()
            .padding(start = 8.dp, end = 8.dp, top = 8.dp)
            .background(MaterialTheme.colorScheme.surfaceContainerHigh, CellShape)
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
        Text("Recalculating after seek", Modifier.weight(1f), style = Body, color = c.hi, maxLines = 1)
        Text("showing L$lap", style = BodySmall, color = c.med, maxLines = 1)
    }
}

@Composable
private fun StrategyContent(s: StrategySnapshot, landscape: Boolean, choice: PlanChoice, choose: (PlanChoice) -> Unit) {
    when {
        landscape -> Row(
            Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Column(Modifier.weight(1.6f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                StatusStrip(s, Modifier.fillMaxWidth().height(32.dp))
                Row(Modifier.fillMaxWidth().height(86.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    PlanTile(PlanChoice.DEFEND, s, choice, choose, Modifier.weight(1f).fillMaxHeight())
                    PlanTile(PlanChoice.ATTACK, s, choice, choose, Modifier.weight(1f).fillMaxHeight())
                }
                TimelineCard(s, choice, choose, Modifier.fillMaxWidth().height(104.dp))
                PaceCard(s, choice, Modifier.fillMaxWidth().weight(1f))
            }
            Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                DecisionCard(s, Modifier.fillMaxWidth())
                TyreLifeCard(s, Modifier.fillMaxWidth().height(112.dp))
                BattleCard(s, Modifier.fillMaxWidth().weight(1f))
            }
        }
        else -> Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(8.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            StatusStrip(s, Modifier.fillMaxWidth().height(32.dp))
            DecisionCard(s, Modifier.fillMaxWidth())
            // A phone is too narrow for anything but the two plans side by
            // side; each tile stacks its lines instead.
            Row(Modifier.fillMaxWidth().height(116.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                PlanTile(PlanChoice.DEFEND, s, choice, choose, Modifier.weight(1f).fillMaxHeight())
                PlanTile(PlanChoice.ATTACK, s, choice, choose, Modifier.weight(1f).fillMaxHeight())
            }
            TimelineCard(s, choice, choose, Modifier.fillMaxWidth().height(112.dp))
            TyreLifeCard(s, Modifier.fillMaxWidth().height(128.dp))
            BattleCard(s, Modifier.fillMaxWidth(), wrap = true)
            PaceCard(s, choice, Modifier.fillMaxWidth().height(168.dp))
        }
    }
}

@Composable
private fun StrategyMessage(title: String, body: String) {
    val c = LocalDashPalette.current
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(title, style = Title, color = c.hi, textAlign = TextAlign.Center)
        Text(body, Modifier.padding(top = 8.dp), style = Body, color = c.med, textAlign = TextAlign.Center)
    }
}

/** Before the first ready snapshot: the engine replays the race so far first. */
@Composable
private fun StrategyCalculating() {
    val c = LocalDashPalette.current
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        CircularProgressIndicator(Modifier.size(28.dp), strokeWidth = 3.dp)
        Text("Calculating strategy", Modifier.padding(top = 16.dp), style = Title, color = c.hi)
        Text("Plans appear once the race so far has been read.", Modifier.padding(top = 6.dp), style = Body, color = c.med, textAlign = TextAlign.Center)
    }
}

// ── Status strip ────────────────────────────────────────────────────────────

@Composable
private fun StatusStrip(s: StrategySnapshot, modifier: Modifier) {
    val c = LocalDashPalette.current
    val wear = s.averageWear.coerceIn(0f, 100f)
    BoxWithConstraints(modifier) {
    // A phone's width: the same facts in fewer words.
    val narrow = maxWidth < 440.dp
    Row(Modifier.fillMaxSize().staticLayer(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(if (narrow) 6.dp else 8.dp)) {
        if (s.playerPosition > 0) {
            Text("P${s.playerPosition}", style = Headline.copy(fontWeight = FontWeight.Bold, fontSize = 22.sp), color = c.hi, maxLines = 1)
        }
        Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
            Text(if (narrow) "L${s.lapNumber}" else "Lap ${s.lapNumber}", style = Body.copy(fontWeight = FontWeight.Medium), color = c.hi, maxLines = 1)
            Text(" / ${s.totalLaps}", style = Body, color = c.med, maxLines = 1)
        }
        StatusChip(
            "${s.compoundName} · ${s.tyreAgeLaps}L · ${wear.roundToInt()}%",
            Color.Transparent,
            c.hi,
            dot = compoundColor(c, s.actualCompound, s.visualCompound),
            outline = MaterialTheme.colorScheme.outline,
        )
        val cliff = cliffColor(c, s.lapsUntilCliff)
        StatusChip(
            when {
                s.lapsUntilCliff <= 0 -> "Past the cliff"
                narrow -> "Cliff L${s.cliffLap}"
                else -> "Cliff L${s.cliffLap} · ${s.lapsUntilCliff} to go"
            },
            if (cliff == c.hi) MaterialTheme.colorScheme.surfaceContainerHigh else cliff.copy(alpha = 0.18f),
            cliff,
        )
    }
    }
}

@Composable
private fun StatusChip(text: String, background: Color, content: Color, dot: Color? = null, outline: Color? = null) {
    Row(
        Modifier
            .height(28.dp)
            .background(background, CellShape)
            .then(if (outline != null) Modifier.border(1.dp, outline, CellShape) else Modifier)
            .padding(horizontal = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (dot != null) {
            Box(Modifier.size(9.dp).background(dot, CircleShape))
            Spacer(Modifier.width(5.dp))
        }
        Text(text, style = Label, color = content, maxLines = 1)
    }
}

// ── Decision ────────────────────────────────────────────────────────────────

private class Decision(
    val kind: String,
    val headline: String,
    val detail: String,
    val value: String,
    val valueDetail: String,
    val ink: Color,
    val urgent: Boolean,
)

/** The one thing to act on, most urgent first. */
private fun decisionFor(c: DashPalette, s: StrategySnapshot): Decision {
    s.neutralisation?.let { n ->
        val projected = if (n.box) n.projectedBoxPosition else n.projectedStayPosition
        return Decision(
            kind = if (n.safetyCar) "SAFETY CAR" else "VIRTUAL SAFETY CAR",
            headline = if (n.box) "BOX NOW" else "STAY OUT",
            detail = "Box now ${delta(n.boxNowCostMs)}s · wait to L${n.boxLaterLap} ${delta(n.boxLaterCostMs)}s",
            value = "P${n.currentPosition} → P$projected",
            valueDetail = sentence(n.reason),
            ink = if (n.box) c.gain else c.caution,
            urgent = true,
        )
    }
    s.call?.let { call ->
        val crossover = call.crossoverLaps?.let { "$it lap${if (it == 1) "" else "s"} to cross over" }.orEmpty()
        return Decision(
            kind = "RACE CALL",
            headline = "${call.kind.uppercase(Locale.US)} ${call.targetName}",
            detail = sentence(call.reason),
            value = "${fixed(call.gapMs / 1000f, 1)}s",
            valueDetail = crossover,
            ink = if (call.kind == "overcut") c.gain else c.caution,
            urgent = true,
        )
    }
    s.weather?.takeIf(::weatherShown)?.let { w ->
        return Decision(
            kind = "WEATHER",
            headline = sentence(w.recommendation).uppercase(Locale.US),
            detail = weatherDetail(w),
            value = if (w.crossoverLap > 0) "L${w.crossoverLap}" else "",
            valueDetail = if (w.crossoverLap > 0) "${w.rainPercentage}% rain" else "",
            ink = c.cold,
            urgent = false,
        )
    }
    s.warnings.firstOrNull()?.let { warning ->
        val (scope, detail) = wearWarningParts(warning.text)
        return Decision(
            kind = "TYRE ALERT",
            headline = scope.ifEmpty { detail.uppercase(Locale.US) },
            detail = if (scope.isEmpty()) "" else sentence(detail),
            value = "",
            valueDetail = "",
            ink = when (warning.severity) {
                "danger" -> c.loss
                "warning" -> c.tyreBands[3]
                else -> c.caution
            },
            urgent = false,
        )
    }
    return Decision("STRATEGY", "Hold the plan", "No call this lap", "", "", c.med, urgent = false)
}

@Composable
private fun DecisionCard(s: StrategySnapshot, modifier: Modifier) {
    val c = LocalDashPalette.current
    val d = decisionFor(c, s)
    val tinted = d.ink != c.med
    Row(
        modifier
            .staticLayer()
            .height(68.dp)
            .background(MaterialTheme.colorScheme.surfaceContainer, CardShape)
            .then(if (tinted) Modifier.background(d.ink.copy(alpha = if (d.urgent) 0.2f else 0.12f), CardShape) else Modifier)
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            CardLabel(d.kind, color = if (tinted) d.ink else c.med)
            Text(d.headline, style = Title.copy(fontWeight = FontWeight.Bold), color = if (tinted) d.ink else c.hi, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (d.detail.isNotEmpty()) {
                Text(d.detail, style = BodySmall, color = c.med, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        if (d.value.isNotEmpty()) {
            Column(Modifier.padding(start = 12.dp), horizontalAlignment = Alignment.End) {
                Text(d.value, style = Headline.copy(fontWeight = FontWeight.Bold), color = c.hi, maxLines = 1)
                if (d.valueDetail.isNotEmpty()) {
                    Text(d.valueDetail, style = BodySmall, color = c.med, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }
    }
}

// ── Plan tiles ──────────────────────────────────────────────────────────────

@Composable
private fun PlanTile(kind: PlanChoice, s: StrategySnapshot, chosen: PlanChoice, onChoose: (PlanChoice) -> Unit, modifier: Modifier) {
    val c = LocalDashPalette.current
    val plan = s.plan(kind)
    val accent = accentOf(c, kind)
    val selected = kind == chosen
    val stop = plan.nextStopLap(s.lapNumber)
    val (headline, detail) = when {
        stop == null -> "NO STOP" to "to the flag"
        stop <= s.lapNumber -> "BOX NOW" to "this lap"
        else -> "BOX L$stop" to "in ${stop - s.lapNumber} lap${if (stop - s.lapNumber == 1) "" else "s"}"
    }
    val target = when {
        plan.targetIndex < 0 -> "Tyre life"
        kind == PlanChoice.DEFEND -> "Cover ${plan.targetName}"
        else -> "Chase ${plan.targetName}"
    }
    Box(
        modifier
            .background(MaterialTheme.colorScheme.surfaceContainer, CardShape)
            .border(if (selected) 2.dp else 1.dp, if (selected) accent else MaterialTheme.colorScheme.outlineVariant, CardShape)
            .clickable { onChoose(kind) },
    ) {
        BoxWithConstraints(Modifier.fillMaxSize()) {
        // Under about 260 dp the stop, its countdown, the target and the tyres
        // do not share lines: each gets its own.
        val narrow = maxWidth < 260.dp
        Column(Modifier.fillMaxSize().padding(horizontal = if (narrow) 10.dp else 12.dp, vertical = 8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(kind.title.uppercase(Locale.US), style = Label.copy(fontWeight = FontWeight.Bold, letterSpacing = 1.sp), color = accent, maxLines = 1)
                Text(
                    "  ${plan.stops} stop${if (plan.stops == 1) "" else "s"} · ${(plan.confidence * 100).roundToInt()}%",
                    style = Label,
                    color = c.med,
                    maxLines = 1,
                )
                Spacer(Modifier.weight(1f))
                if (!plan.legal) Text("NOT LEGAL", style = Label.copy(fontSize = 10.sp, fontWeight = FontWeight.Bold), color = c.loss, maxLines = 1)
            }
            val tyres: @Composable () -> Unit = {
                plan.stints.forEachIndexed { index, stint ->
                    if (index > 0) Text(" › ", style = BodySmall, color = c.dis)
                    TyreGlyph(stint.actualCompound, stint.visualCompound, stint.compoundName, Modifier.size(18.dp))
                }
            }
            if (narrow) {
                Text(
                    headline,
                    Modifier.weight(1f).wrapContentHeight(Alignment.CenterVertically),
                    style = Headline.copy(fontWeight = FontWeight.Bold, fontSize = 26.sp),
                    color = if (selected) accent else c.hi,
                    maxLines = 1,
                )
                Text("$detail · $target", style = BodySmall, color = c.med, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Row(Modifier.padding(top = 4.dp), verticalAlignment = Alignment.CenterVertically) { tyres() }
            } else {
                Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
                    Text(headline, style = Headline.copy(fontWeight = FontWeight.Bold, fontSize = 26.sp), color = if (selected) accent else c.hi, maxLines = 1)
                    Spacer(Modifier.width(8.dp))
                    Text(detail, style = Body, color = c.med, maxLines = 1)
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    tyres()
                    Spacer(Modifier.weight(1f).width(8.dp))
                    Text(target, style = BodySmall, color = c.med, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }
        }
    }
}

/** A tyre as the broadcast draws it: a compound-coloured ring round its letter. */
@Composable
private fun TyreGlyph(actual: Int, visual: Int, name: String, modifier: Modifier) {
    val c = LocalDashPalette.current
    val ink = compoundColor(c, actual, visual)
    Box(
        modifier.background(MaterialTheme.colorScheme.surfaceContainerHighest, CircleShape).border(2.dp, ink, CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Text(compoundLetter(actual, visual, name), style = Label.copy(fontSize = 9.sp, fontWeight = FontWeight.Black, lineHeight = 9.sp), color = ink, maxLines = 1)
    }
}

// ── Race timeline ───────────────────────────────────────────────────────────

/**
 * Both plans over the race distance: a lane each, its stints as bars that fill
 * as the laps are run, each led by its tyre. The current lap, the tyre cliff
 * and the weather crossover cross both lanes. Tapping a lane picks its plan.
 */
@Composable
private fun TimelineCard(s: StrategySnapshot, chosen: PlanChoice, onChoose: (PlanChoice) -> Unit, modifier: Modifier) {
    val c = LocalDashPalette.current
    val scheme = MaterialTheme.colorScheme
    val density = LocalDensity.current
    val labelPaint = remember(density) { textPaint(with(density) { 11.sp.toPx() }, 700) }
    val smallPaint = remember(density) { textPaint(with(density) { 10.sp.toPx() }, 500) }
    val glyphPaint = remember(density) { textPaint(with(density) { 9.sp.toPx() }, 900) }
    val choose by rememberUpdatedState(onChoose)
    val total = s.totalLaps.takeIf { it > 0 }
        ?: max(s.defensive.stints.maxOfOrNull { it.endLap } ?: 0, s.attacking.stints.maxOfOrNull { it.endLap } ?: 0)
    val weather = s.weather?.takeIf { it.crossoverLap > 0 }
    DashCard(modifier, cached = true) {
        Spacer(
            Modifier
                .fillMaxSize()
                .semantics { contentDescription = "Both plans over the race distance" }
                .pointerInput(Unit) {
                    detectTapGestures { offset -> choose(if (offset.y < size.height / 2f) PlanChoice.DEFEND else PlanChoice.ATTACK) }
                }
                .drawBehind {
                    if (total <= 0) return@drawBehind
                    val gutter = 62.dp.toPx()
                    val axis = 16.dp.toPx()
                    val footer = 14.dp.toPx()
                    val laneGap = 6.dp.toPx()
                    val lane = (size.height - axis - footer - laneGap) / 2f
                    val width = size.width - gutter
                    fun x(lap: Float) = gutter + ((lap - 1f) / total).coerceIn(0f, 1f) * width
                    val canvas = drawContext.canvas.nativeCanvas
                    val nowX = x(s.lapNumber + 0.5f)

                    // Lap axis, clear of the current lap's tag.
                    smallPaint.color = c.dis.toArgb()
                    val step = when {
                        total <= 20 -> 5
                        total <= 60 -> 10
                        else -> 20
                    }
                    // A tick too close to the race length gives way to it.
                    val ticks = (listOf(1) + (step..total step step).filter { total - it >= step / 2 } + total).distinct()
                    var lastRight = Float.NEGATIVE_INFINITY
                    for (lap in ticks) {
                        val label = "$lap"
                        val width = smallPaint.measureText(label)
                        val tx = (x(lap.toFloat()) - width / 2f).coerceIn(gutter, size.width - width)
                        if (abs(tx + width / 2f - nowX) < 22.dp.toPx() || tx < lastRight + 6.dp.toPx()) continue
                        canvas.drawText(label, tx, axis - 5.dp.toPx(), smallPaint)
                        lastRight = tx + width
                    }

                    // Lane backgrounds, then the markers, then the bars: a
                    // marker passes behind the stints and their tyres.
                    PlanChoice.entries.forEachIndexed { index, kind ->
                        val top = axis + index * (lane + laneGap)
                        val accent = accentOf(c, kind)
                        val selected = kind == chosen
                        if (selected) {
                            drawRoundRect(scheme.surfaceContainerHigh, Offset(0f, top), Size(size.width, lane), CornerRadius(8.dp.toPx()))
                        }
                        labelPaint.color = (if (selected) accent else accent.copy(alpha = 0.6f)).toArgb()
                        val metrics = labelPaint.fontMetrics
                        canvas.drawText(kind.title.uppercase(Locale.US), 8.dp.toPx(), top + lane / 2f - (metrics.ascent + metrics.descent) / 2f, labelPaint)
                    }

                    val bottom = axis + lane * 2 + laneGap
                    val dash = PathEffect.dashPathEffect(floatArrayOf(4.dp.toPx(), 3.dp.toPx()))
                    fun marker(lap: Int, color: Color, label: String) {
                        if (lap <= 0 || lap > total) return
                        val mx = x(lap.toFloat())
                        drawLine(color, Offset(mx, axis), Offset(mx, bottom), 1.5.dp.toPx(), pathEffect = dash)
                        smallPaint.color = color.toArgb()
                        val tw = smallPaint.measureText(label)
                        canvas.drawText(label, (mx - tw / 2f).coerceIn(gutter, size.width - tw), size.height - 2.dp.toPx(), smallPaint)
                    }
                    marker(s.cliffLap, c.loss, "CLIFF L${s.cliffLap}")
                    weather?.let { marker(it.crossoverLap, c.cold, "${weatherWord(it)} L${it.crossoverLap}") }

                    PlanChoice.entries.forEachIndexed { index, kind ->
                        val top = axis + index * (lane + laneGap)
                        drawPlanLane(s.plan(kind), s.lapNumber, top, lane, ::x, c, scheme.surfaceContainerHighest, glyphPaint, smallPaint)
                    }

                    // The current lap, tagged on the axis.
                    drawLine(c.hi, Offset(nowX, axis), Offset(nowX, bottom), 2.dp.toPx())
                    val tag = "L${s.lapNumber}"
                    labelPaint.color = c.onFill.toArgb()
                    val tagW = labelPaint.measureText(tag) + 10.dp.toPx()
                    val tagX = (nowX - tagW / 2f).coerceIn(gutter, size.width - tagW)
                    drawRoundRect(c.hi, Offset(tagX, 0f), Size(tagW, axis - 2.dp.toPx()), CornerRadius(4.dp.toPx()))
                    val tm = labelPaint.fontMetrics
                    canvas.drawText(tag, tagX + 5.dp.toPx(), (axis - 2.dp.toPx()) / 2f - (tm.ascent + tm.descent) / 2f, labelPaint)
                },
        )
    }
}

private fun DrawScope.drawPlanLane(
    plan: StrategyPlan,
    lap: Int,
    top: Float,
    lane: Float,
    x: (Float) -> Float,
    c: DashPalette,
    glyphFill: Color,
    glyphPaint: android.graphics.Paint,
    lapsPaint: android.graphics.Paint,
) {
    val canvas = drawContext.canvas.nativeCanvas
    val barH = lane * 0.64f
    val barTop = top + (lane - barH) / 2f
    val radius = CornerRadius(barH / 2f)
    val gap = 3.dp.toPx()
    for (stint in plan.stints) {
        val ink = compoundColor(c, stint.actualCompound, stint.visualCompound)
        val left = x(stint.startLap.toFloat())
        val right = x(stint.endLap + 1f) - gap
        if (right <= left) continue
        // Run laps solid, laps to come faint.
        drawRoundRect(ink.copy(alpha = 0.22f), Offset(left, barTop), Size(right - left, barH), radius)
        val run = (x(lap.toFloat()).coerceIn(left, right))
        if (run > left) drawRoundRect(ink.copy(alpha = 0.55f), Offset(left, barTop), Size(run - left, barH), radius)

        // A stint shorter than its tyre glyph keeps only its bar.
        if (right - left < barH + 2.dp.toPx()) continue
        val r = barH / 2f
        val center = Offset(left + r, barTop + r)
        drawCircle(glyphFill, r, center)
        drawCircle(ink, r - 1.dp.toPx(), center, style = Stroke(2.dp.toPx()))
        glyphPaint.color = ink.toArgb()
        val letter = compoundLetter(stint.actualCompound, stint.visualCompound, stint.compoundName)
        val gm = glyphPaint.fontMetrics
        canvas.drawText(letter, center.x - glyphPaint.measureText(letter) / 2f, center.y - (gm.ascent + gm.descent) / 2f, glyphPaint)

        val laps = "${stint.endLap - stint.startLap + 1} laps"
        val textX = left + barH + 4.dp.toPx()
        if (textX + lapsPaint.measureText(laps) < right - 4.dp.toPx()) {
            lapsPaint.color = c.hi.toArgb()
            val lm = lapsPaint.fontMetrics
            canvas.drawText(laps, textX, center.y - (lm.ascent + lm.descent) / 2f, lapsPaint)
        }
    }
}

// ── Pace against the plan ───────────────────────────────────────────────────

/**
 * The stint the car is in, lap by lap, against the picked plan's required
 * times: a bar per lap, below the line when faster; the next lap's target and
 * the time banked over the stint beside it.
 */
@Composable
private fun PaceCard(s: StrategySnapshot, choice: PlanChoice, modifier: Modifier) {
    val c = LocalDashPalette.current
    val plan = s.plan(choice)
    val accent = accentOf(c, choice)
    val stint = plan.stints.firstOrNull { s.lapNumber in it.startLap..it.endLap } ?: plan.stints.lastOrNull()
    val rows = stint?.rows.orEmpty()
    val run = rows.filter { it.hasActual && it.requiredMs > 0 }
    val next = rows.firstOrNull { !it.hasActual && it.lapNumber >= s.lapNumber && it.requiredMs > 0 }
    val banked = run.lastOrNull()?.deltaStintMs
    val race = run.lastOrNull()?.deltaTotalMs
    val track = MaterialTheme.colorScheme.outlineVariant
    val density = LocalDensity.current
    val lapPaint = remember(density) { textPaint(with(density) { 10.sp.toPx() }, 500) }
    DashCard(modifier, cached = true) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column {
                CardLabel("PACE VS ${choice.title.uppercase(Locale.US)}", color = accent)
                if (stint != null) {
                    Text(
                        "Stint ${stint.number} · ${stint.compoundName} · L${stint.startLap}–${stint.endLap}",
                        style = BodySmall,
                        color = c.dis,
                        maxLines = 1,
                    )
                }
            }
            Spacer(Modifier.weight(1f).width(8.dp))
            PaceFigure("NEXT LAP", next?.let { lapTime(it.requiredMs) } ?: "—", if (next != null) c.hi else c.dis)
            PaceFigure("STINT", banked?.let { "${delta(it)}s" } ?: "—", banked?.let { deltaColor(c, it) } ?: c.dis)
            PaceFigure("RACE", race?.let { "${delta(it)}s" } ?: "—", race?.let { deltaColor(c, it) } ?: c.dis)
        }
        Row(Modifier.fillMaxWidth().weight(1f).padding(top = 6.dp)) {
            Spacer(
                Modifier.weight(1f).fillMaxHeight().semantics { contentDescription = "Lap times against the plan" }.drawBehind {
                    if (rows.isEmpty()) return@drawBehind
                    val labels = 12.dp.toPx()
                    val chartH = size.height - labels
                    val mid = chartH / 2f
                    val scale = max(500, run.maxOfOrNull { abs(it.deltaLapMs) } ?: 0).toFloat()
                    val slot = size.width / rows.size
                    val bar = (slot * 0.62f).coerceAtMost(18.dp.toPx())
                    drawLine(track, Offset(0f, mid), Offset(size.width, mid), 1.dp.toPx())
                    val canvas = drawContext.canvas.nativeCanvas
                    val every = max(1, (rows.size + 7) / 8)
                    rows.forEachIndexed { index, row ->
                        val cx = slot * index + slot / 2f
                        val left = cx - bar / 2f
                        if (row.hasActual && row.requiredMs > 0) {
                            val h = (abs(row.deltaLapMs) / scale).coerceAtMost(1f) * (mid - 2.dp.toPx())
                            val color = deltaColor(c, row.deltaLapMs)
                            // Faster laps hang below the line, slower ones rise above it.
                            val y = if (row.deltaLapMs > 0) mid - h else mid
                            drawRoundRect(color, Offset(left, y), Size(bar, max(h, 1.5.dp.toPx())), CornerRadius(2.dp.toPx()))
                        } else {
                            drawCircle(track, 2.dp.toPx(), Offset(cx, mid))
                        }
                        if (row.lapNumber == s.lapNumber) {
                            drawRoundRect(c.hi, Offset(left - 2.dp.toPx(), 0f), Size(bar + 4.dp.toPx(), chartH), CornerRadius(3.dp.toPx()), style = Stroke(1.dp.toPx()))
                        }
                        if (index % every == 0 || row.lapNumber == s.lapNumber) {
                            val label = "${row.lapNumber}"
                            lapPaint.color = (if (row.lapNumber == s.lapNumber) c.hi else c.dis).toArgb()
                            canvas.drawText(label, cx - lapPaint.measureText(label) / 2f, size.height - 1.dp.toPx(), lapPaint)
                        }
                    }
                },
            )
        }
    }
}

@Composable
private fun PaceFigure(label: String, value: String, color: Color) {
    val c = LocalDashPalette.current
    Column(Modifier.padding(start = 16.dp), horizontalAlignment = Alignment.End) {
        Text(label, style = Label.copy(fontSize = 10.sp, letterSpacing = 1.sp), color = c.dis, maxLines = 1)
        Text(value, style = Title.copy(fontWeight = FontWeight.Bold), color = color, maxLines = 1)
    }
}

// ── Tyre life ───────────────────────────────────────────────────────────────

/** Each corner's remaining life as a gauge that empties, laid out as on the car. */
@Composable
private fun TyreLifeCard(s: StrategySnapshot, modifier: Modifier) {
    val c = LocalDashPalette.current
    val names = listOf("FL", "FR", "RL", "RR")
    DashCard(modifier, cached = true) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            CardLabel("TYRE LIFE")
            Text(
                "${fixed(s.wearPerLap, 1)}%/lap worn",
                Modifier.weight(1f).padding(start = 8.dp),
                style = BodySmall,
                color = c.med,
                textAlign = TextAlign.End,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Column(Modifier.fillMaxWidth().weight(1f).padding(top = 6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            for (axle in 0..1) {
                Row(Modifier.fillMaxWidth().weight(1f), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (side in 0..1) {
                        val corner = axle * 2 + side
                        TyreGauge(
                            names[corner],
                            s.cornerWear.getOrElse(corner) { 0f },
                            limiting = s.limitingCorner.equals(names[corner], ignoreCase = true),
                            modifier = Modifier.weight(1f).fillMaxHeight(),
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun TyreGauge(name: String, wear: Float, limiting: Boolean, modifier: Modifier) {
    val c = LocalDashPalette.current
    val ink = tyreBandColor(c, wear.roundToInt())
    val life = (1f - wear / 100f).coerceIn(0f, 1f)
    val track = MaterialTheme.colorScheme.surfaceContainerHighest
    Row(
        modifier
            .background(MaterialTheme.colorScheme.surfaceContainerHigh, CellShape)
            .then(if (limiting) Modifier.border(1.dp, ink, CellShape) else Modifier)
            .padding(horizontal = 8.dp, vertical = 5.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Spacer(
            Modifier.width(8.dp).fillMaxHeight().drawBehind {
                val corner = CornerRadius(size.width / 2f)
                drawRoundRect(track, cornerRadius = corner)
                val fill = life * size.height
                if (fill > 0f) drawRoundRect(ink, Offset(0f, size.height - fill), Size(size.width, fill), corner)
            },
        )
        Spacer(Modifier.width(8.dp))
        Text(name, style = Label.copy(fontWeight = FontWeight.Bold), color = if (limiting) ink else c.med, maxLines = 1)
        if (limiting) Text(" LIMITS", style = Label.copy(fontSize = 9.sp), color = ink, maxLines = 1)
        Spacer(Modifier.weight(1f))
        Row {
            Text("${wear.roundToInt()}", Modifier.alignByBaseline(), style = Title.copy(fontWeight = FontWeight.Bold), color = ink, maxLines = 1)
            Text("%", Modifier.alignByBaseline(), style = BodySmall, color = c.dis, maxLines = 1)
        }
    }
}

// ── Battle ──────────────────────────────────────────────────────────────────

/** You and the cars either side, in race order, with each gap and how it is moving. */
@Composable
private fun BattleCard(s: StrategySnapshot, modifier: Modifier, wrap: Boolean = false) {
    val c = LocalDashPalette.current
    val ahead = s.rivals.filter { it.ahead }.sortedBy { it.position }
    val behind = s.rivals.filter { !it.ahead }.sortedBy { it.position }
    DashCard(modifier, cached = true) {
        CardLabel("BATTLE")
        Column(
            Modifier.fillMaxWidth().then(if (wrap) Modifier else Modifier.weight(1f)).padding(top = 4.dp),
            verticalArrangement = if (wrap) Arrangement.spacedBy(2.dp) else Arrangement.SpaceEvenly,
        ) {
            if (ahead.isEmpty()) LadderNote(if (s.playerPosition == 1) "Leading" else "No car within range ahead")
            for (rival in ahead) RivalLine(rival, s.lapNumber)
            YouLine(s)
            for (rival in behind) RivalLine(rival, s.lapNumber)
            if (behind.isEmpty()) LadderNote("No car within range behind")
        }
    }
}

@Composable
private fun LadderNote(text: String) {
    Text(text, Modifier.padding(start = 40.dp), style = BodySmall, color = LocalDashPalette.current.dis, maxLines = 1)
}

@Composable
private fun YouLine(s: StrategySnapshot) {
    val c = LocalDashPalette.current
    Row(
        Modifier.fillMaxWidth().height(30.dp).background(MaterialTheme.colorScheme.primary.copy(alpha = 0.14f), CellShape).padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(if (s.playerPosition > 0) "P${s.playerPosition}" else "P–", Modifier.width(34.dp), style = Label.copy(fontWeight = FontWeight.Bold), color = c.hi)
        Box(Modifier.size(8.dp).background(compoundColor(c, s.actualCompound, s.visualCompound), CircleShape))
        Spacer(Modifier.width(8.dp))
        Text("YOU", Modifier.weight(1f), style = Body.copy(fontWeight = FontWeight.Bold), color = MaterialTheme.colorScheme.primary, maxLines = 1)
        Text("${s.tyreAgeLaps}L tyres", style = BodySmall, color = c.med, maxLines = 1)
    }
}

@Composable
private fun RivalLine(rival: StrategyRival, lap: Int) {
    val c = LocalDashPalette.current
    val pitted = rival.lastPitLap > 0 && rival.lastPitLap >= lap - 1
    // A slower car ahead is being caught; a slower car behind is dropping back.
    val slower = rival.paceDeltaMs > 50
    val faster = rival.paceDeltaMs < -50
    val closing = if (rival.ahead) slower else faster
    val trend = when {
        pitted -> "PIT L${rival.lastPitLap}"
        !slower && !faster -> ""
        else -> "${if (closing) "▼" else "▲"} ${fixed(abs(rival.paceDeltaMs) / 1000f, 2)}/L"
    }
    val good = rival.ahead == closing
    val trendInk = when {
        pitted -> c.caution
        !slower && !faster -> c.dis
        good -> c.gain
        else -> c.loss
    }
    Row(Modifier.fillMaxWidth().height(28.dp).padding(horizontal = 6.dp), verticalAlignment = Alignment.CenterVertically) {
        Text("P${rival.position}", Modifier.width(34.dp), style = Label, color = c.med, maxLines = 1)
        Box(Modifier.size(8.dp).background(compoundColor(c, rival.actualCompound, rival.visualCompound), CircleShape))
        Spacer(Modifier.width(8.dp))
        Text(rival.name, Modifier.weight(1f), style = Body.copy(fontWeight = FontWeight.Medium), color = c.hi, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Text(trend, Modifier.padding(horizontal = 8.dp), style = BodySmall, color = trendInk, maxLines = 1)
        Text(
            "${if (rival.ahead) "−" else "+"}${fixed(rival.gapMs / 1000f, 1)}",
            Modifier.width(48.dp),
            style = Body.copy(fontWeight = FontWeight.Bold),
            color = c.hi,
            textAlign = TextAlign.End,
            maxLines = 1,
        )
    }
}

// ── Helpers ─────────────────────────────────────────────────────────────────

private fun StrategySnapshot.plan(kind: PlanChoice): StrategyPlan =
    if (kind == PlanChoice.DEFEND) defensive else attacking

private fun accentOf(c: DashPalette, kind: PlanChoice): Color =
    if (kind == PlanChoice.DEFEND) c.cold else c.caution

private fun cliffColor(c: DashPalette, lapsToGo: Int): Color = when {
    lapsToGo <= 5 -> c.loss
    lapsToGo <= 10 -> c.caution
    else -> c.hi
}

/** Slower than the plan is a loss, faster a gain. */
private fun deltaColor(c: DashPalette, milliseconds: Int): Color = if (milliseconds > 0) c.loss else c.gain

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

private fun compoundLetter(actual: Int, visual: Int, name: String): String = when (actual) {
    11 -> "SS"
    12 -> "S"
    13 -> "M"
    14 -> "H"
    15 -> "W"
    else -> when (visual) {
        16 -> "S"
        17 -> "M"
        18 -> "H"
        7 -> "I"
        8 -> "W"
        else -> name.take(1).uppercase(Locale.US).ifEmpty { "?" }
    }
}

/** The desktop shows the window only once it has a crossover or a set to compare. */
private fun weatherShown(w: StrategyWeather) = w.crossoverLap > 0 || w.lapDeltaMs != null

private fun weatherWord(w: StrategyWeather): String = when (w.recommendation) {
    "prepare_slicks" -> "SLICKS"
    "prepare_intermediates" -> "INTERS"
    "prepare_wets" -> "WETS"
    else -> "WEATHER"
}

private fun weatherDetail(w: StrategyWeather): String {
    val timing = if (w.crossoverLap > 0) {
        if (w.minutesUntilChange > 0) "About ${w.minutesUntilChange} min" else "Now"
    } else {
        sentence(w.reason)
    }
    val set = w.lapDeltaMs?.let { deltaMs ->
        val wear = when (w.setWear) {
            null -> ""
            0 -> ", new set"
            else -> ", ${w.setWear}% worn"
        }
        " · ${w.targetCompound} ${fixed(abs(deltaMs) / 1000f, 1)}s/L ${if (deltaMs < 0) "faster" else "slower"}$wear"
    }.orEmpty()
    return timing + set
}

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
