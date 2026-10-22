package com.nogoat.tracknrace.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.PrimaryTabRow
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.snapshots.Snapshot
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.takeOrElse
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nogoat.tracknrace.DashboardColdState
import com.nogoat.tracknrace.GraphsController
import com.nogoat.tracknrace.PairedTelemetryPage
import com.nogoat.tracknrace.TelemetryStore
import com.nogoat.tracknrace.TyreSetEntry
import kotlin.math.roundToInt
import kotlin.math.sign
import kotlinx.coroutines.launch

private enum class TyreColumn { SET, COMPOUND, STATUS, WEAR, WEAR_PER_LAP, LIFE, RECOMMENDED, DELTA }

/**
 * One set's row as it is shown: rounded, formatted values. Tyre-set rows arrive
 * about once a second with raw values that move in decimals the row does not
 * show, so rows compare on what they display and an unchanged row is skipped.
 */
@androidx.compose.runtime.Immutable
internal data class TyreRow(
    val index: Int,
    val compound: String,
    val visualCompound: Int,
    val status: String,
    /** "16/23 L". */
    val life: String,
    /** The session the set was recommended for, e.g. "Q2". */
    val session: String,
    /** Wear in tenths of a percent, for the bar; the text shows whole percent. */
    val wearTenths: Int,
    val wearPerLap: String,
    val delta: String,
    /** The lap delta's sign: positive slower, negative faster. */
    val deltaSign: Int,
) {
    val wear: Float get() = wearTenths / 10f
}

private fun tyreRow(set: TyreSetEntry, sessionType: Int?, labels: Map<String, String>) = TyreRow(
    index = set.index,
    compound = compoundName(set, labels),
    visualCompound = set.visualCompound,
    status = tyreStatus(set, sessionType),
    life = "${set.lifeSpan}/${set.usableLife} L",
    session = sessionLabel(set.recommendedSession),
    wearTenths = (set.wear * 10f).roundToInt(),
    wearPerLap = avgWearPerLap(set),
    delta = lapDelta(set),
    deltaSign = set.lapDeltaMs.sign,
)

/** What the Tyres page shows: its dry and wet rows, compared whole. */
@androidx.compose.runtime.Immutable
internal data class TyreDisplay(val dry: List<TyreRow> = emptyList(), val wet: List<TyreRow> = emptyList())

/**
 * The page's rows from the cold state. Read through derived state with
 * structural equality, so a tyre-set update that changes nothing shown (they
 * arrive about once a second) recomposes nothing.
 */
internal fun tyreDisplay(cold: DashboardColdState) = TyreDisplay(
    dry = cold.tyreSets.filter { it.actualCompound != 0 && it.actualCompound !in WetCompounds }
        .sortedWith(compareBy({ dryOrder(it.actualCompound, it.visualCompound) }, { it.index }))
        .map { tyreRow(it, cold.sessionType, cold.labels) },
    wet = cold.tyreSets.filter { it.actualCompound in WetCompounds }
        .sortedWith(compareBy({ wetOrder(it.actualCompound) }, { it.index }))
        .map { tyreRow(it, cold.sessionType, cold.labels) },
)

/** One set's row, a state written only when what it shows changes. */
@androidx.compose.runtime.Stable
private class TyreRowState {
    var row by mutableStateOf<TyreRow?>(null)
}

/**
 * The page's rows, kept per set. Each update writes every set's row; an equal
 * row is no change, so only the sets whose shown values changed recompose.
 */
@androidx.compose.runtime.Stable
private class TyreRowStates {
    private val rows = HashMap<Int, TyreRowState>()

    fun of(index: Int): TyreRowState = rows.getOrPut(index) { TyreRowState() }

    fun update(display: TyreDisplay) {
        for (row in display.dry) of(row.index).row = row
        for (row in display.wet) of(row.index).row = row
    }
}

/** The Tyres page's tabs, and the stream each asks the desktop for. */
private enum class TyresTab(val title: String) { ALLOCATION("Allocation"), CHARTS("Charts") }

/**
 * The Tyres page: the tyre-set allocation, and the desktop's Tyres graphs.
 * Each tab asks the desktop for its own data while it is the one shown.
 */
@Composable
internal fun TyresScreen(
    display: () -> TyreDisplay,
    store: TelemetryStore,
    graphs: GraphsController,
    active: Boolean = true,
    onPage: (PairedTelemetryPage) -> Unit = {},
    /** Told whether the Charts tab is the one shown, so the app bar can offer the chart window. */
    onChartsShown: (Boolean) -> Unit = {},
) {
    var saved by rememberSaveable { mutableIntStateOf(0) }
    val pager = rememberPagerState(initialPage = saved) { TyresTab.entries.size }
    val scope = rememberCoroutineScope()
    val current = pager.currentPage
    LaunchedEffect(current, active) {
        saved = current
        onChartsShown(TyresTab.entries[current] == TyresTab.CHARTS)
        // The Charts tab asks for its own stream, which follows its windows.
        if (active && TyresTab.entries[current] == TyresTab.ALLOCATION) onPage(PairedTelemetryPage.TYRES)
    }
    Column(Modifier.fillMaxSize()) {
        PrimaryTabRow(
            selectedTabIndex = current,
            modifier = Modifier.staticLayer(),
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            divider = {},
        ) {
            TyresTab.entries.forEachIndexed { index, tab ->
                Tab(
                    selected = current == index,
                    onClick = { scope.launch { pager.animateScrollToPage(index) } },
                    text = { Text(tab.title, maxLines = 1) },
                    selectedContentColor = MaterialTheme.colorScheme.primary,
                    unselectedContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        HorizontalPager(pager, Modifier.fillMaxWidth().weight(1f), beyondViewportPageCount = 0) { page ->
            when (TyresTab.entries[page]) {
                TyresTab.ALLOCATION -> TyreAllocation(display)
                TyresTab.CHARTS -> TyreChartsTab(store, graphs, active && current == page, onPage)
            }
        }
    }
}

/**
 * Which sets go where, in order: dry then wet as the desktop orders them, and
 * the set on the car. Compared whole, so a tyre update that moves no set
 * recomposes no list.
 */
@androidx.compose.runtime.Immutable
private data class TyreOrder(
    val dry: List<Int> = emptyList(),
    val wet: List<Int> = emptyList(),
    val fitted: List<Int> = emptyList(),
)

private fun tyreOrder(display: TyreDisplay): TyreOrder = TyreOrder(
    dry = display.dry.map { it.index },
    wet = display.wet.map { it.index },
    fitted = (display.dry + display.wet).filter { it.status == "FITTED" }.map { it.index },
)

/**
 * Every tyre set the car has. Portrait shows the set on the car, then a dry
 * and a wet table; landscape is one table, dry then wet.
 */
@Composable
private fun TyreAllocation(display: () -> TyreDisplay) {
    val rows = remember { TyreRowStates().also { it.update(Snapshot.withoutReadObservation { display() }) } }
    LaunchedEffect(rows) {
        snapshotFlow { display() }.collect { rows.update(it) }
    }
    val order by remember { derivedStateOf(structuralEqualityPolicy()) { tyreOrder(display()) } }
    // A plain width check: BoxWithConstraints would re-run the list as a
    // subcomposition on every tyre update.
    val showTable = LocalConfiguration.current.screenWidthDp >= 600
    val empty = order.dry.isEmpty() && order.wet.isEmpty()
    Column(Modifier.fillMaxSize()) {
        if (showTable) {
            TyreTableHeader()
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        }
        if (empty) {
            Text(
                "Waiting for tyre allocation data…",
                modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 28.dp),
                textAlign = TextAlign.Center,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodyMedium,
            )
            return@Column
        }
        LazyColumn(Modifier.fillMaxWidth().weight(1f)) {
            if (showTable) {
                tyreTableSection("dry", "Dry", order.dry, rows)
                tyreTableSection("wet", "Wet", order.wet, rows)
            } else {
                items(order.fitted, key = { "fitted-$it" }) { set -> FittedTyre(rows.of(set)) }
                tyrePortraitSection("dry", "Dry", order.dry, rows)
                tyrePortraitSection("wet", "Wet", order.wet, rows)
            }
        }
    }
}

private fun androidx.compose.foundation.lazy.LazyListScope.tyrePortraitSection(
    key: String,
    title: String,
    sets: List<Int>,
    rows: TyreRowStates,
) {
    if (sets.isEmpty()) return
    item(key = "$key-header") { TyreGroupHeader(title, sets.size, trailing = "Δ lap vs fitted") }
    items(sets, key = { "$key-$it" }) { set -> PortraitTyreRow(rows.of(set)) }
}

private fun androidx.compose.foundation.lazy.LazyListScope.tyreTableSection(
    key: String,
    title: String,
    sets: List<Int>,
    rows: TyreRowStates,
) {
    if (sets.isEmpty()) return
    item(key = "$key-header") { TyreGroupHeader(title, sets.size, height = 28.dp, horizontal = TablePadding) }
    items(sets, key = { "$key-$it" }) { set -> TyreTableRow(rows.of(set)) }
}

private val TablePadding = 24.dp
private val TableGap = 12.dp

/** A group's name and count on a thin line, its rows below. */
@Composable
private fun TyreGroupHeader(
    title: String,
    count: Int,
    trailing: String? = null,
    height: androidx.compose.ui.unit.Dp = 32.dp,
    horizontal: androidx.compose.ui.unit.Dp = 16.dp,
) {
    Column(Modifier.fillMaxWidth().staticLayer()) {
        Row(
            modifier = Modifier.fillMaxWidth().height(height - 1.dp).padding(start = horizontal, end = horizontal, bottom = 6.dp),
            verticalAlignment = Alignment.Bottom,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(title, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(count.toString(), style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.outline)
            if (trailing != null) {
                Spacer(Modifier.weight(1f))
                Text(trailing, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.outline)
            }
        }
        TyreHairline()
    }
}

@Composable
private fun TyreHairline() = HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f))

/** Portrait: the set on the car, larger, with its wear across the width. */
@Composable
private fun FittedTyre(state: TyreRowState) {
    val row = state.row ?: return
    val outline = MaterialTheme.colorScheme.outline
    val strong = MaterialTheme.colorScheme.onSurface
    Column(Modifier.fillMaxWidth().graphicsLayer()) {
        Column(Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 14.dp)) {
            Text("On the car", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Row(
                modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Text(
                    row.compound,
                    modifier = Modifier.alignByBaseline(),
                    style = MaterialTheme.typography.headlineMedium,
                    color = compoundTextColor(row.visualCompound),
                    maxLines = 1,
                )
                Text(
                    "Set ${row.index + 1}",
                    modifier = Modifier.alignByBaseline(),
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary,
                )
                Spacer(Modifier.weight(1f))
                Text(
                    "${row.wear.roundToInt()}%",
                    modifier = Modifier.alignByBaseline(),
                    style = tyreNumberStyle(14).copy(fontWeight = FontWeight.Medium),
                    color = strong,
                )
                Text("worn", modifier = Modifier.alignByBaseline(), style = MaterialTheme.typography.bodySmall, color = outline)
            }
            WearBar(row.wear, tyreWearColor(row.wear), Modifier.fillMaxWidth().padding(top = 8.dp))
            Row(Modifier.padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                FittedDetail("Life ", row.life, "")
                FittedDetail("Wear ", row.wearPerLap, " /lap")
                FittedDetail("Rec ", row.session, "")
            }
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
    }
}

@Composable
private fun FittedDetail(label: String, value: String, suffix: String) {
    val outline = MaterialTheme.colorScheme.outline
    Text(
        buildAnnotatedString {
            withStyle(SpanStyle(color = outline)) { append(label) }
            append(value)
            withStyle(SpanStyle(color = outline)) { append(suffix) }
        },
        style = MaterialTheme.typography.bodySmall.copy(fontFeatureSettings = "tnum"),
        color = MaterialTheme.colorScheme.onSurface,
        maxLines = 1,
    )
}

/**
 * Portrait: one set on one line. The fitted set is tinted across the width;
 * sets out of use are dimmed.
 */
@Composable
private fun PortraitTyreRow(state: TyreRowState) {
    val row = state.row ?: return
    val gone = row.status == "RETURNED" || row.status == "RESERVED"
    val outline = MaterialTheme.colorScheme.outline
    val background = if (row.status == "FITTED") MaterialTheme.colorScheme.primary.copy(alpha = 0.10f) else Color.Transparent
    // Its own drawing list: another row changing does not re-record this one.
    Column(Modifier.fillMaxWidth().graphicsLayer().background(background)) {
        Row(
            modifier = Modifier.fillMaxWidth().height(35.dp).padding(horizontal = 16.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("${row.index + 1}", Modifier.width(20.dp), style = MaterialTheme.typography.labelMedium, color = outline, textAlign = TextAlign.End)
            Text(
                row.compound,
                Modifier.width(34.dp),
                style = MaterialTheme.typography.labelLarge,
                color = compoundTextColor(row.visualCompound).copy(alpha = if (gone) 0.6f else 1f),
                maxLines = 1,
            )
            Text(statusLabel(row.status), Modifier.width(52.dp), style = MaterialTheme.typography.labelSmall, color = tyreStatusColor(row.status), maxLines = 1)
            Text(
                "${row.life} · ${row.session}",
                Modifier.weight(1f),
                style = MaterialTheme.typography.bodySmall.copy(fontFeatureSettings = "tnum"),
                color = outline,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                "${row.wear.roundToInt()}%",
                Modifier.width(30.dp),
                style = tyreNumberStyle(),
                color = if (gone) outline else wearTextColor(row.wear),
                textAlign = TextAlign.End,
            )
            Text(row.delta, Modifier.width(56.dp), style = tyreNumberStyle(), color = tyreDeltaColor(row.deltaSign), textAlign = TextAlign.End, maxLines = 1)
        }
        TyreHairline()
    }
}

@Composable
private fun TyreTableHeader() {
    Row(
        modifier = Modifier.fillMaxWidth().staticLayer().height(28.dp).padding(horizontal = TablePadding),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(TableGap),
    ) {
        TyreHeaderText("#", tyreColumnModifier(TyreColumn.SET), TextAlign.End)
        TyreHeaderText("Tyre", tyreColumnModifier(TyreColumn.COMPOUND))
        TyreHeaderText("Status", tyreColumnModifier(TyreColumn.STATUS))
        TyreHeaderText("Wear", tyreColumnModifier(TyreColumn.WEAR))
        TyreHeaderText("Wear/lap", tyreColumnModifier(TyreColumn.WEAR_PER_LAP), TextAlign.End)
        TyreHeaderText("Life", tyreColumnModifier(TyreColumn.LIFE), TextAlign.End)
        TyreHeaderText("Rec", tyreColumnModifier(TyreColumn.RECOMMENDED))
        TyreHeaderText("Δ lap", tyreColumnModifier(TyreColumn.DELTA), TextAlign.End)
    }
}

@Composable
private fun TyreHeaderText(text: String, modifier: Modifier, align: TextAlign = TextAlign.Start) {
    Text(
        text,
        modifier = modifier,
        style = MaterialTheme.typography.labelSmall,
        color = MaterialTheme.colorScheme.outline,
        textAlign = align,
        maxLines = 1,
    )
}

/** Landscape: one set on one line; the fitted set is tinted across the width. */
@Composable
private fun TyreTableRow(state: TyreRowState) {
    val row = state.row ?: return
    val gone = row.status == "RETURNED" || row.status == "RESERVED"
    val outline = MaterialTheme.colorScheme.outline
    val text = if (gone) outline else MaterialTheme.colorScheme.onSurface
    val background = if (row.status == "FITTED") MaterialTheme.colorScheme.primary.copy(alpha = 0.10f) else Color.Transparent
    // Its own drawing list: another row changing does not re-record this one.
    Column(Modifier.fillMaxWidth().graphicsLayer().background(background)) {
        Row(
            modifier = Modifier.fillMaxWidth().height(31.dp).padding(horizontal = TablePadding),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(TableGap),
        ) {
            Text("${row.index + 1}", tyreColumnModifier(TyreColumn.SET), style = MaterialTheme.typography.labelMedium, color = outline, textAlign = TextAlign.End)
            Text(
                row.compound,
                tyreColumnModifier(TyreColumn.COMPOUND),
                style = MaterialTheme.typography.labelLarge,
                color = compoundTextColor(row.visualCompound).copy(alpha = if (gone) 0.6f else 1f),
                maxLines = 1,
            )
            Text(statusLabel(row.status), tyreColumnModifier(TyreColumn.STATUS), style = MaterialTheme.typography.labelMedium, color = tyreStatusColor(row.status), maxLines = 1)
            Row(
                modifier = tyreColumnModifier(TyreColumn.WEAR),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                WearBar(row.wear, tyreWearColor(row.wear), Modifier.weight(1f))
                Text("${row.wear.roundToInt()}%", Modifier.width(32.dp), style = tyreNumberStyle(), color = wearTextColor(row.wear), textAlign = TextAlign.End)
            }
            Text(row.wearPerLap, tyreColumnModifier(TyreColumn.WEAR_PER_LAP), style = tyreNumberStyle(), color = text, textAlign = TextAlign.End, maxLines = 1)
            Text(row.life, tyreColumnModifier(TyreColumn.LIFE), style = tyreNumberStyle(), color = text, textAlign = TextAlign.End, maxLines = 1)
            Text(row.session, tyreColumnModifier(TyreColumn.RECOMMENDED), style = MaterialTheme.typography.labelMedium, color = text, maxLines = 1)
            Text(row.delta, tyreColumnModifier(TyreColumn.DELTA), style = tyreNumberStyle(), color = tyreDeltaColor(row.deltaSign), textAlign = TextAlign.End, maxLines = 1)
        }
        TyreHairline()
    }
}

/** Wear as a thin rounded bar, painted rather than clipped. */
@Composable
private fun WearBar(wear: Float, color: Color, modifier: Modifier) {
    val track = MaterialTheme.colorScheme.surfaceContainerHighest
    Spacer(
        modifier.height(4.dp).drawBehind {
            val corner = CornerRadius(size.height / 2f)
            drawRoundRect(track, cornerRadius = corner)
            val filled = size.width * (wear / 100f).coerceIn(0f, 1f)
            if (filled > 0f) drawRoundRect(color, size = Size(maxOf(filled, size.height), size.height), cornerRadius = corner)
        },
    )
}

private fun statusLabel(status: String) = status.lowercase().replaceFirstChar { it.titlecase() }

@Composable
private fun tyreStatusColor(status: String): Color = when (status) {
    "FITTED" -> MaterialTheme.colorScheme.primary
    "NEW" -> MaterialTheme.colorScheme.onSurface
    "USED" -> MaterialTheme.colorScheme.onSurfaceVariant
    else -> MaterialTheme.colorScheme.outline
}

@Composable
private fun compoundTextColor(visualCompound: Int): Color =
    compoundColor(visualCompound).takeOrElse { MaterialTheme.colorScheme.primary }

@Composable
private fun wearTextColor(wear: Float): Color =
    if (wear.roundToInt() > 0) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.outline

@Composable
private fun tyreWearColor(wear: Float): Color = when {
    wear >= 75f -> MaterialTheme.colorScheme.error
    wear >= 50f -> MaterialTheme.colorScheme.tertiary
    else -> MaterialTheme.colorScheme.primary
}

@Composable
private fun tyreDeltaColor(sign: Int): Color = when {
    sign > 0 -> MaterialTheme.colorScheme.error
    sign < 0 -> MaterialTheme.colorScheme.primary
    else -> MaterialTheme.colorScheme.outline
}

@Composable
private fun tyreNumberStyle(size: Int = 12): TextStyle = MaterialTheme.typography.bodySmall.copy(
    fontFamily = FontFamily.Monospace,
    fontFeatureSettings = "tnum",
    fontSize = size.sp,
)

private fun RowScope.tyreColumnModifier(column: TyreColumn): Modifier {
    return when (column) {
        TyreColumn.SET -> Modifier.width(24.dp)
        TyreColumn.COMPOUND -> Modifier.width(40.dp)
        TyreColumn.STATUS -> Modifier.width(72.dp)
        TyreColumn.WEAR -> Modifier.weight(1f)
        TyreColumn.WEAR_PER_LAP -> Modifier.width(64.dp)
        TyreColumn.LIFE -> Modifier.width(64.dp)
        TyreColumn.RECOMMENDED -> Modifier.width(48.dp)
        TyreColumn.DELTA -> Modifier.width(72.dp)
    }
}

/** The wet compounds, as the desktop splits them: intermediate, wet, and the classic wet. */
private val WetCompounds = setOf(7, 8, 15)

/** The desktop's dry order: classic compounds by their own order, modern ones soft, medium, hard. */
private fun dryOrder(actual: Int, visual: Int) = when {
    actual in 11..14 -> actual - 11
    else -> when (visual) { 16 -> 0; 17 -> 1; 18 -> 2; else -> 3 }
}
private fun wetOrder(compound: Int) = when (compound) { 7 -> 0; 8 -> 1; else -> 2 }

private fun compoundName(set: TyreSetEntry, labels: Map<String, String>): String =
    labels["tyre.actual.${set.actualCompound}"] ?: compoundLabel(set.actualCompound)

private fun compoundLabel(compound: Int) = when (compound) {
    7 -> "INT"; 8 -> "WET"; 16 -> "C5"; 17 -> "C4"; 18 -> "C3"
    19 -> "C2"; 20 -> "C1"; 21 -> "C0"; 22 -> "C6"; else -> compound.toString()
}

private fun compoundColor(compound: Int) = when (compound) {
    16 -> Color(0xffe53935); 17 -> Color(0xffffc107); 18 -> Color(0xffeceff1)
    7 -> Color(0xff66bb6a); 8 -> Color(0xff42a5f5); else -> Color.Unspecified
}

private fun tyreStatus(set: TyreSetEntry, sessionType: Int?): String = when {
    set.fitted -> "FITTED"
    set.available && set.wear == 0f -> "NEW"
    set.available -> "USED"
    sessionType != null && set.recommendedSession > sessionOrder(sessionType) -> "RESERVED"
    sessionType == null && set.recommendedSession >= 4 -> "RESERVED"
    else -> "RETURNED"
}

private fun sessionOrder(sessionType: Int) = when {
    sessionType in 1..3 -> sessionType
    sessionType == 4 -> 3
    sessionType == 5 || sessionType == 10 -> 4
    sessionType == 6 || sessionType == 11 -> 5
    sessionType in 7..9 || sessionType in 12..14 -> 6
    else -> 7
}

private fun sessionLabel(session: Int) = when (session) {
    1 -> "FP1"; 2 -> "FP2"; 3 -> "FP3"; 4 -> "Q1"; 5 -> "Q2"; 6 -> "Q3"; 7 -> "Race"; else -> "—"
}

private fun avgWearPerLap(set: TyreSetEntry): String =
    set.avgWearPerLap?.let { "%.2f%%".format(it) } ?: "—"

private fun lapDelta(set: TyreSetEntry): String {
    if (set.fitted || !set.available || set.lapDeltaMs == 0) return "—"
    val seconds = set.lapDeltaMs / 1000.0
    return "%+.3fs".format(seconds)
}
