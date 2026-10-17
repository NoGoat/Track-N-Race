package com.tracknrace.android.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.snapshots.Snapshot
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.tracknrace.android.DashboardColdState
import com.tracknrace.android.TyreSetEntry
import kotlin.math.roundToInt
import kotlin.math.sign

private enum class TyreColumn { SET, COMPOUND, STATUS, WEAR, WEAR_PER_LAP, LIFE, RECOMMENDED, DELTA }

// A section's card, drawn by its first and last items so each tyre set can be
// its own list item.
private val SectionTopShape = RoundedCornerShape(topStart = 16.dp, topEnd = 16.dp)
private val SectionBottomShape = RoundedCornerShape(bottomStart = 16.dp, bottomEnd = 16.dp)
private val SectionShape = RoundedCornerShape(16.dp)

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
    dry = cold.tyreSets.filter { it.actualCompound !in setOf(7, 8) }
        .sortedWith(compareBy({ dryOrder(it.visualCompound) }, { it.index }))
        .map { tyreRow(it, cold.sessionType, cold.labels) },
    wet = cold.tyreSets.filter { it.actualCompound in setOf(7, 8) }
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

/**
 * The dry and the wet sets, each a card of rows. The list holds only which
 * sets there are, in order; each row reads its own state. A change to one set
 * recomposes that row alone, and each row is cached as a texture.
 */
@Composable
internal fun TyresScreen(display: () -> TyreDisplay) {
    val rows = remember { TyreRowStates().also { it.update(Snapshot.withoutReadObservation { display() }) } }
    LaunchedEffect(rows) {
        snapshotFlow { display() }.collect { rows.update(it) }
    }
    val order by remember {
        derivedStateOf(structuralEqualityPolicy()) {
            display().let { shown -> shown.dry.map { it.index } to shown.wet.map { it.index } }
        }
    }
    val (dry, wet) = order
    // A plain width check: BoxWithConstraints would re-run the list as a
    // subcomposition on every tyre update.
    val showTable = LocalConfiguration.current.screenWidthDp >= 600
    val card = MaterialTheme.colorScheme.surfaceContainerLow
    Box(Modifier.fillMaxSize()) {
        LazyColumn(
            Modifier.fillMaxSize(),
            contentPadding = PaddingValues(
                horizontal = if (showTable) 16.dp else 12.dp,
                vertical = 12.dp,
            ),
        ) {
            tyreSection("dry", "Dry sets (slicks)", dry, rows, showTable, card)
            item(key = "gap") { Spacer(Modifier.height(16.dp)) }
            tyreSection("wet", "Wet / intermediate sets", wet, rows, showTable, card)
        }
    }
}

private fun androidx.compose.foundation.lazy.LazyListScope.tyreSection(
    key: String,
    title: String,
    sets: List<Int>,
    rows: TyreRowStates,
    showTable: Boolean,
    card: Color,
) {
    item(key = "$key-header") {
        Column(Modifier.fillMaxWidth().background(card, if (sets.isEmpty()) SectionShape else SectionTopShape)) {
            TyreSectionHeader(title, sets.size)
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            if (sets.isEmpty()) {
                Text(
                    "Waiting for tyre allocation data…",
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 28.dp),
                    textAlign = TextAlign.Center,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodyMedium,
                )
            } else if (showTable) {
                TyreTableHeader()
            }
        }
    }
    val last = sets.lastIndex
    itemsIndexed(sets, key = { _, set -> "$key-$set" }) { index, set ->
        TyreRowItem(rows.of(set), divider = index > 0 || showTable, bottom = index == last, showTable = showTable, card = card)
    }
}

/** One set's row in its card: recomposed only when its own shown values change. */
@Composable
private fun TyreRowItem(state: TyreRowState, divider: Boolean, bottom: Boolean, showTable: Boolean, card: Color) {
    val row = state.row ?: return
    Column(Modifier.fillMaxWidth().background(card, if (bottom) SectionBottomShape else RectangleShape)) {
        if (divider) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Box {
            if (showTable) TyreTableRow(row) else CompactTyreRow(row)
        }
    }
}

@Composable
private fun TyreSectionHeader(title: String, count: Int) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            title,
            modifier = Modifier.weight(1f),
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.onSurface,
            fontWeight = FontWeight.SemiBold,
        )
        Surface(
            shape = MaterialTheme.shapes.extraLarge,
            color = MaterialTheme.colorScheme.secondaryContainer,
            contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
        ) {
            Text(
                count.toString(),
                modifier = Modifier.padding(horizontal = 9.dp, vertical = 3.dp),
                style = MaterialTheme.typography.labelMedium,
                fontWeight = FontWeight.SemiBold,
            )
        }
    }
}

@Composable
private fun TyreTableHeader() {
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        TyreTableText("#", tyreColumnModifier(TyreColumn.SET), header = true)
        TyreTableText("Tyre", tyreColumnModifier(TyreColumn.COMPOUND), header = true)
        TyreTableText("Status", tyreColumnModifier(TyreColumn.STATUS), header = true)
        TyreTableText("Wear", tyreColumnModifier(TyreColumn.WEAR), header = true)
        TyreTableText("Wear/lap", tyreColumnModifier(TyreColumn.WEAR_PER_LAP), header = true)
        TyreTableText("Life", tyreColumnModifier(TyreColumn.LIFE), header = true)
        TyreTableText("Rec", tyreColumnModifier(TyreColumn.RECOMMENDED), header = true)
        TyreTableText("Δ lap", tyreColumnModifier(TyreColumn.DELTA), header = true, align = TextAlign.End)
    }
}

@Composable
private fun CompactTyreRow(row: TyreRow) {
    val wearColor = tyreWearColor(row.wear)
    val primaryText = MaterialTheme.colorScheme.onSurface
    val secondaryText = MaterialTheme.colorScheme.onSurfaceVariant
    Surface(
        color = Color.Transparent,
        contentColor = primaryText,
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 13.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            CompactTyreIdentity(
                label = row.compound,
                accent = compoundColor(row.visualCompound),
                setNumber = row.index + 1,
                status = row.status,
                numberColor = secondaryText,
            )
            Column(modifier = Modifier.weight(1f)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        "${row.life}  ·  ${row.session}",
                        modifier = Modifier.weight(1f),
                        color = secondaryText,
                        style = MaterialTheme.typography.bodySmall,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(
                        row.delta,
                        color = tyreDeltaColor(row.deltaSign),
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
                Spacer(Modifier.height(5.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        "Wear  ·  ${row.wearPerLap} per lap",
                        color = secondaryText,
                        style = MaterialTheme.typography.labelSmall,
                    )
                    Spacer(Modifier.weight(1f))
                    Text(
                        "${row.wear.roundToInt()}%",
                        color = wearColor,
                        style = MaterialTheme.typography.labelMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
                Spacer(Modifier.height(4.dp))
                WearBar(row.wear, wearColor, Modifier.fillMaxWidth())
            }
        }
    }
}

@Composable
private fun TyreTableRow(row: TyreRow) {
    val wearColor = tyreWearColor(row.wear)
    val rowTextColor = MaterialTheme.colorScheme.onSurface
    Surface(color = Color.Transparent, contentColor = rowTextColor) {
        Row(
            modifier = Modifier.fillMaxWidth().height(48.dp).padding(horizontal = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TyreTableText("${row.index + 1}", tyreColumnModifier(TyreColumn.SET), color = rowTextColor)
            TyreTableText(
                row.compound,
                tyreColumnModifier(TyreColumn.COMPOUND),
                color = compoundColor(row.visualCompound),
                weight = FontWeight.Black,
            )
            Box(tyreColumnModifier(TyreColumn.STATUS), contentAlignment = Alignment.Center) {
                TyreStatusPill(row.status)
            }
            Row(
                modifier = tyreColumnModifier(TyreColumn.WEAR).padding(horizontal = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                WearBar(row.wear, wearColor, Modifier.weight(1f))
                Text(
                    "${row.wear.roundToInt()}%",
                    modifier = Modifier.width(40.dp),
                    color = wearColor,
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold,
                    textAlign = TextAlign.End,
                )
            }
            TyreTableText(row.wearPerLap, tyreColumnModifier(TyreColumn.WEAR_PER_LAP), color = rowTextColor)
            TyreTableText(row.life, tyreColumnModifier(TyreColumn.LIFE), color = rowTextColor)
            TyreTableText(row.session, tyreColumnModifier(TyreColumn.RECOMMENDED), color = rowTextColor)
            TyreTableText(
                row.delta,
                tyreColumnModifier(TyreColumn.DELTA),
                color = tyreDeltaColor(row.deltaSign),
                weight = FontWeight.SemiBold,
                align = TextAlign.End,
            )
        }
    }
}

/** Wear as a rounded bar, painted rather than clipped. */
@Composable
private fun WearBar(wear: Float, color: Color, modifier: Modifier) {
    val track = MaterialTheme.colorScheme.surfaceContainerHighest
    Spacer(
        modifier.height(6.dp).drawBehind {
            val corner = CornerRadius(size.height / 2f)
            drawRoundRect(track, cornerRadius = corner)
            val filled = size.width * (wear / 100f).coerceIn(0f, 1f)
            if (filled > 0f) drawRoundRect(color, size = Size(maxOf(filled, size.height), size.height), cornerRadius = corner)
        },
    )
}

@Composable
private fun CompactTyreIdentity(
    label: String,
    accent: Color,
    setNumber: Int,
    status: String,
    numberColor: Color,
) {
    val compoundTextColor = when (accent) {
        Color.Unspecified -> MaterialTheme.colorScheme.primary
        Color(0xffeceff1) -> MaterialTheme.colorScheme.outline
        else -> accent
    }
    Column(
        modifier = Modifier.width(88.dp),
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(
                setNumber.toString(),
                color = numberColor,
                style = MaterialTheme.typography.labelMedium,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                label,
                color = compoundTextColor,
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
        Spacer(Modifier.height(2.dp))
        TyreStatusPill(status)
    }
}

@Composable
private fun TyreStatusPill(status: String) {
    val (containerColor, contentColor) = when (status) {
        "FITTED" -> MaterialTheme.colorScheme.primary to MaterialTheme.colorScheme.onPrimary
        "NEW" -> MaterialTheme.colorScheme.secondaryContainer to MaterialTheme.colorScheme.onSecondaryContainer
        "USED" -> MaterialTheme.colorScheme.primaryContainer to MaterialTheme.colorScheme.onPrimaryContainer
        else -> MaterialTheme.colorScheme.surfaceContainerHighest to MaterialTheme.colorScheme.onSurfaceVariant
    }
    Surface(
        modifier = Modifier.width(78.dp),
        shape = MaterialTheme.shapes.small,
        color = containerColor,
        contentColor = contentColor,
    ) {
        Text(
            status.lowercase().replaceFirstChar { it.titlecase() },
            modifier = Modifier.fillMaxWidth().padding(horizontal = 6.dp, vertical = 3.dp),
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.SemiBold,
            textAlign = TextAlign.Center,
            maxLines = 1,
        )
    }
}

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
    else -> MaterialTheme.colorScheme.onSurfaceVariant
}

private fun RowScope.tyreColumnModifier(column: TyreColumn): Modifier {
    return when (column) {
        TyreColumn.SET -> Modifier.width(40.dp)
        TyreColumn.COMPOUND -> Modifier.width(50.dp)
        TyreColumn.STATUS -> Modifier.width(104.dp)
        TyreColumn.WEAR -> Modifier.weight(1f)
        TyreColumn.WEAR_PER_LAP -> Modifier.width(72.dp)
        TyreColumn.LIFE -> Modifier.width(72.dp)
        TyreColumn.RECOMMENDED -> Modifier.width(58.dp)
        TyreColumn.DELTA -> Modifier.width(84.dp)
    }
}

@Composable
private fun TyreTableText(
    text: String,
    modifier: Modifier,
    header: Boolean = false,
    color: Color = Color.Unspecified,
    weight: FontWeight = FontWeight.Normal,
    align: TextAlign = TextAlign.Center,
) {
    Text(
        text,
        modifier = modifier.padding(horizontal = 2.dp),
        color = if (color == Color.Unspecified) {
            if (header) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.onSurface
        } else {
            color
        },
        style = (if (header) MaterialTheme.typography.labelSmall else MaterialTheme.typography.bodySmall).copy(
            fontFamily = FontFamily.SansSerif,
            fontFeatureSettings = "tnum",
        ),
        fontWeight = if (header) FontWeight.Bold else weight,
        textAlign = align,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
    )
}

private fun dryOrder(compound: Int) = when (compound) { 16 -> 0; 17 -> 1; 18 -> 2; else -> 3 }
private fun wetOrder(compound: Int) = when (compound) { 7 -> 0; 8 -> 1; else -> 2 }

private fun compoundName(set: TyreSetEntry, labels: Map<String, String>): String =
    labels["tyre.actual.${set.actualCompound}"] ?: compoundLabel(set.actualCompound)

private fun compoundLabel(compound: Int) = when (compound) {
    7 -> "INT"; 8 -> "WET"; 16 -> "C5"; 17 -> "C4"; 18 -> "C3"
    19 -> "C2"; 20 -> "C1"; 21 -> "C0"; 22 -> "C6"; else -> compound.toString()
}

private fun compoundColor(compound: Int) = when (compound) {
    16 -> Color(0xffe53935); 17 -> Color(0xffffc107); 18 -> Color(0xffeceff1)
    7 -> Color(0xff43a047); 8 -> Color(0xff1e88e5); else -> Color.Unspecified
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
