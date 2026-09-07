package com.tracknrace.android.pages

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
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
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.tracknrace.android.DashboardColdState
import com.tracknrace.android.TyreSetEntry
import kotlin.math.roundToInt

private enum class TyreColumn { SET, COMPOUND, STATUS, WEAR, LIFE, RECOMMENDED, DELTA }

@Composable
internal fun TyresScreen(cold: DashboardColdState) {
    val dry = cold.tyreSets.filter { it.actualCompound !in setOf(7, 8) }
        .sortedWith(compareBy({ dryOrder(it.visualCompound) }, { it.index }))
    val wet = cold.tyreSets.filter { it.actualCompound in setOf(7, 8) }
        .sortedWith(compareBy({ wetOrder(it.actualCompound) }, { it.index }))
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val showTable = maxWidth >= 600.dp
        LazyColumn(
            Modifier.fillMaxSize(),
            contentPadding = PaddingValues(
                horizontal = if (showTable) 16.dp else 12.dp,
                vertical = 12.dp,
            ),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            item {
                TyreTableSection(
                    title = "Dry sets (slicks)",
                    sets = dry,
                    cold = cold,
                    showTable = showTable,
                )
            }
            item {
                TyreTableSection(
                    title = "Wet / intermediate sets",
                    sets = wet,
                    cold = cold,
                    showTable = showTable,
                )
            }
        }
    }
}

@Composable
private fun TyreTableSection(
    title: String,
    sets: List<TyreSetEntry>,
    cold: DashboardColdState,
    showTable: Boolean,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        ),
    ) {
        Column {
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
                        sets.size.toString(),
                        modifier = Modifier.padding(horizontal = 9.dp, vertical = 3.dp),
                        style = MaterialTheme.typography.labelMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
            }
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
                sets.forEach { set ->
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    TyreTableRow(set, cold)
                }
            } else {
                sets.forEachIndexed { index, set ->
                    if (index > 0) {
                        HorizontalDivider(
                            color = MaterialTheme.colorScheme.outlineVariant,
                        )
                    }
                    CompactTyreRow(set, cold)
                }
            }
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
        TyreTableText("Life", tyreColumnModifier(TyreColumn.LIFE), header = true)
        TyreTableText("Rec", tyreColumnModifier(TyreColumn.RECOMMENDED), header = true)
        TyreTableText("Δ lap", tyreColumnModifier(TyreColumn.DELTA), header = true, align = TextAlign.End)
    }
}

@Composable
private fun CompactTyreRow(set: TyreSetEntry, cold: DashboardColdState) {
    val status = tyreStatus(set, cold.sessionType)
    val compound = cold.labels["tyre.actual.${set.actualCompound}"] ?: compoundLabel(set.actualCompound)
    val wearColor = tyreWearColor(set.wear)
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
                label = compound,
                accent = compoundColor(set.visualCompound),
                setNumber = set.index + 1,
                status = status,
                numberColor = secondaryText,
            )
            Column(modifier = Modifier.weight(1f)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        "${set.lifeSpan}/${set.usableLife} L  ·  ${sessionLabel(set.recommendedSession)}",
                        modifier = Modifier.weight(1f),
                        color = secondaryText,
                        style = MaterialTheme.typography.bodySmall,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(
                        lapDelta(set),
                        color = tyreDeltaColor(set),
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
                Spacer(Modifier.height(5.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("Wear", color = secondaryText, style = MaterialTheme.typography.labelSmall)
                    Spacer(Modifier.weight(1f))
                    Text(
                        "${set.wear.roundToInt()}%",
                        color = wearColor,
                        style = MaterialTheme.typography.labelMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
                Spacer(Modifier.height(4.dp))
                LinearProgressIndicator(
                    progress = { (set.wear / 100f).coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth().height(6.dp).clip(MaterialTheme.shapes.extraLarge),
                    color = wearColor,
                    trackColor = MaterialTheme.colorScheme.surfaceContainerHighest,
                    strokeCap = StrokeCap.Round,
                )
            }
        }
    }
}

@Composable
private fun TyreTableRow(set: TyreSetEntry, cold: DashboardColdState) {
    val status = tyreStatus(set, cold.sessionType)
    val compound = cold.labels["tyre.actual.${set.actualCompound}"] ?: compoundLabel(set.actualCompound)
    val wearColor = tyreWearColor(set.wear)
    val rowTextColor = MaterialTheme.colorScheme.onSurface
    Surface(color = Color.Transparent, contentColor = rowTextColor) {
        Row(
            modifier = Modifier.fillMaxWidth().height(48.dp).padding(horizontal = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TyreTableText("${set.index + 1}", tyreColumnModifier(TyreColumn.SET), color = rowTextColor)
            TyreTableText(
                compound,
                tyreColumnModifier(TyreColumn.COMPOUND),
                color = compoundColor(set.visualCompound),
                weight = FontWeight.Black,
            )
            Box(tyreColumnModifier(TyreColumn.STATUS), contentAlignment = Alignment.Center) {
                TyreStatusPill(status)
            }
            Row(
                modifier = tyreColumnModifier(TyreColumn.WEAR).padding(horizontal = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                LinearProgressIndicator(
                    progress = { (set.wear / 100f).coerceIn(0f, 1f) },
                    modifier = Modifier.weight(1f).height(6.dp).clip(MaterialTheme.shapes.extraLarge),
                    color = wearColor,
                    trackColor = MaterialTheme.colorScheme.surfaceContainerHighest,
                    strokeCap = StrokeCap.Round,
                )
                Text(
                    "${set.wear.roundToInt()}%",
                    modifier = Modifier.width(40.dp),
                    color = wearColor,
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold,
                    textAlign = TextAlign.End,
                )
            }
            TyreTableText("${set.lifeSpan}/${set.usableLife} L", tyreColumnModifier(TyreColumn.LIFE), color = rowTextColor)
            TyreTableText(sessionLabel(set.recommendedSession), tyreColumnModifier(TyreColumn.RECOMMENDED), color = rowTextColor)
            TyreTableText(
                lapDelta(set),
                tyreColumnModifier(TyreColumn.DELTA),
                color = tyreDeltaColor(set),
                weight = FontWeight.SemiBold,
                align = TextAlign.End,
            )
        }
    }
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
private fun tyreDeltaColor(set: TyreSetEntry): Color = when {
    set.lapDeltaMs > 0 -> MaterialTheme.colorScheme.error
    set.lapDeltaMs < 0 -> MaterialTheme.colorScheme.primary
    else -> MaterialTheme.colorScheme.onSurfaceVariant
}

private fun RowScope.tyreColumnModifier(column: TyreColumn): Modifier {
    return when (column) {
        TyreColumn.SET -> Modifier.width(40.dp)
        TyreColumn.COMPOUND -> Modifier.width(50.dp)
        TyreColumn.STATUS -> Modifier.width(104.dp)
        TyreColumn.WEAR -> Modifier.weight(1f)
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

private fun lapDelta(set: TyreSetEntry): String {
    if (set.fitted || !set.available || set.lapDeltaMs == 0) return "—"
    val seconds = set.lapDeltaMs / 1000.0
    return "%+.3fs".format(seconds)
}
