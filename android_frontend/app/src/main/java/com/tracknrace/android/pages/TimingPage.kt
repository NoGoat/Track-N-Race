package com.tracknrace.android.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.State
import androidx.compose.runtime.produceState
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.snapshots.Snapshot
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.tracknrace.android.TimingCarEntry
import com.tracknrace.android.TimingDriver
import com.tracknrace.android.TimingTowerState
import com.tracknrace.android.TimingTyreStatus
import com.tracknrace.android.needsParticipantsRefresh
import java.util.Locale
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.conflate

internal const val PARTICIPANTS_RETRY_DELAY_MS = 3_000L

// Portrait: one line per car. Each column's width is shared by the header and the rows.
private val CompactRowHeight = 36.dp
private val CompactPadding = 12.dp
private val CompactGapBetween = 8.dp
private val CompactPos = 24.dp
private val CompactCode = 40.dp
private val CompactLap = 24.dp
private val CompactLast = 72.dp
private val CompactTyre = 52.dp

// Landscape: the full table.
private val TableRowHeight = 36.dp
private val TablePadding = 24.dp
private val TableGapBetween = 12.dp
private val TablePos = 28.dp
private val TableLap = 36.dp
private val TableLast = 92.dp
private val TableGap = 104.dp
private val TableTyre = 112.dp
private val TableStatus = 96.dp

private val TeamBarWidth = 3.dp

@Composable
internal fun TimingScreen(
    state: () -> TimingTowerState,
    labels: Map<String, String>,
    onRequestParticipants: () -> Unit,
    active: Boolean = true,
) {
    // Timing rows arrive several times a second and nearly every row's gap
    // changes each time. The newest state is shown as it arrives, at most once
    // a frame (snapshot changes are applied per frame). An outgoing page stops
    // collecting, which freezes it during the switch.
    val latest by rememberUpdatedState(state)
    val rows = remember { TimingRows().also { it.update(Snapshot.withoutReadObservation { state() }) } }
    val shown = produceState(Snapshot.withoutReadObservation { state() }, active) {
        if (!active) return@produceState
        snapshotFlow { latest() }.conflate().collect {
            rows.update(it)
            value = it
        }
    }
    TimingTower(shown, rows, labels, onRequestParticipants, active)
}

/** One car's row values, each a state written only when it changes. */
@Stable
private class TimingRowState {
    var car by mutableStateOf<TimingCarEntry?>(null)
    var driver by mutableStateOf<TimingDriver?>(null)
    var tyre by mutableStateOf<TimingTyreStatus?>(null)
}

/**
 * The tower's rows, kept per car. Each update writes every car's values into
 * its row; a write of an equal value is no change, so only the rows whose
 * values changed recompose. The gap is left out: it changes on nearly every
 * update and is painted from the newest state while drawing.
 */
@Stable
private class TimingRows {
    private val rows = HashMap<Int, TimingRowState>()

    fun of(index: Int): TimingRowState = rows.getOrPut(index) { TimingRowState() }

    fun update(tower: TimingTowerState) {
        for (car in tower.cars) {
            val row = of(car.index)
            row.car = car.copy(currentLapMs = 0, gapMs = 0)
            row.driver = tower.drivers[car.index]
            row.tyre = tower.tyreStatuses[car.index]
        }
    }
}

@Composable
private fun TimingTower(
    state: State<TimingTowerState>,
    rows: TimingRows,
    labels: Map<String, String>,
    onRequestParticipants: () -> Unit,
    active: Boolean,
) {
    // The list holds only the running order: it recomposes when a car moves
    // up or down, not when a car's lap or tyre changes. Each row reads its own
    // car from the state, so an update recomposes the rows it changed and no
    // others; gaps, which change on nearly every update, repaint while drawing.
    val order by remember(state) {
        derivedStateOf(structuralEqualityPolicy()) {
            state.value.cars
                .filter { it.position > 0 && it.resultStatus >= 1 }
                .sortedBy(TimingCarEntry::position)
                .map { it.index }
        }
    }
    val playerIndex by remember(state) { derivedStateOf(structuralEqualityPolicy()) { state.value.playerIndex } }
    val participantRosterMissing by remember(state) { derivedStateOf(structuralEqualityPolicy()) { state.value.needsParticipantsRefresh() } }
    // The quickest of the cars' last laps, marked in each row that set it.
    val fastestLastLap by remember(state) {
        derivedStateOf(structuralEqualityPolicy()) {
            state.value.cars
                .filter { it.position > 0 && it.resultStatus >= 1 && it.lastLapMs > 0 }
                .minOfOrNull { it.lastLapMs } ?: 0
        }
    }

    LaunchedEffect(participantRosterMissing, active) {
        if (!active || !participantRosterMissing) return@LaunchedEffect
        while (true) {
            delay(PARTICIPANTS_RETRY_DELAY_MS)
            onRequestParticipants()
        }
    }

    // A plain width check: BoxWithConstraints would re-run the whole list as a
    // subcomposition on every timing update.
    val wide = LocalConfiguration.current.screenWidthDp >= 600
    Column(Modifier.fillMaxSize()) {
        if (order.isEmpty()) {
            Text(
                "Waiting for timing data…",
                modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 36.dp),
                textAlign = TextAlign.Center,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodyMedium,
            )
            return@Column
        }
        // The column names stay put while the rows scroll under them.
        if (wide) TimingTableHeader() else TimingCompactHeader()
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        LazyColumn(Modifier.fillMaxSize()) {
            // One lazy item per car, keyed by car: a timing update recomposes,
            // relays out and redraws only the rows whose values changed.
            items(order, key = { it }) { carIndex ->
                TimingItem(
                    state = state,
                    row = rows.of(carIndex),
                    carIndex = carIndex,
                    wide = wide,
                    labels = labels,
                    isPlayer = carIndex == playerIndex,
                    fastestLastLap = fastestLastLap,
                )
            }
        }
    }
}

/**
 * One car's row. It reads its own row state, which changes only when this
 * car's shown values do: an update to another car, or to this car's gap alone,
 * does not recompose it.
 */
@Composable
private fun TimingItem(
    state: State<TimingTowerState>,
    row: TimingRowState,
    carIndex: Int,
    wide: Boolean,
    labels: Map<String, String>,
    isPlayer: Boolean,
    fastestLastLap: Int,
) {
    val driver = row.driver
    val tyre = row.tyre
    val gap = remember(state, carIndex) { { gapText(state.value, carIndex) } }
    val shown = row.car ?: return
    val fastest = shown.lastLapMs > 0 && shown.lastLapMs == fastestLastLap
    // The player's row is tinted across the full width.
    val background = if (isPlayer) MaterialTheme.colorScheme.primary.copy(alpha = 0.10f) else Color.Transparent
    // Its own drawing list: another row changing does not re-record this one.
    Column(Modifier.fillMaxWidth().graphicsLayer().background(background)) {
        if (wide) {
            TimingTableRow(car = shown, gap = gap, driver = driver, tyre = tyre, labels = labels, isPlayer = isPlayer, fastest = fastest)
        } else {
            TimingCompactRow(car = shown, gap = gap, driver = driver, tyre = tyre, labels = labels, isPlayer = isPlayer, fastest = fastest)
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f))
    }
}

@Composable
private fun TimingCompactHeader() {
    Row(
        modifier = Modifier.fillMaxWidth().height(28.dp).padding(horizontal = CompactPadding),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(CompactGapBetween),
    ) {
        TimingHeaderText("Pos", Modifier.width(CompactPos))
        Spacer(Modifier.width(TeamBarWidth))
        TimingHeaderText("Driver", Modifier.width(CompactCode))
        TimingHeaderText("Lap", Modifier.width(CompactLap))
        TimingHeaderText("Last", Modifier.width(CompactLast))
        TimingHeaderText("Gap", Modifier.weight(1f), TextAlign.End)
        TimingHeaderText("Tyre", Modifier.width(CompactTyre), TextAlign.End)
    }
}

/** Portrait: position, team, code, lap, last lap, gap and tyre on one line. */
@Composable
private fun TimingCompactRow(
    car: TimingCarEntry,
    gap: () -> String,
    driver: TimingDriver?,
    tyre: TimingTyreStatus?,
    labels: Map<String, String>,
    isPlayer: Boolean,
    fastest: Boolean,
) {
    val gapColor = if (resultLabel(car.resultStatus) != null) {
        MaterialTheme.colorScheme.error
    } else {
        MaterialTheme.colorScheme.onSurface
    }
    // A stop is shown in place of the last lap; the other flags sit before the gap.
    val flags = flagLabels(car, includePit = false)

    Row(
        modifier = Modifier.fillMaxWidth().height(CompactRowHeight).padding(horizontal = CompactPadding),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(CompactGapBetween),
    ) {
        // Everything but the gap changes only with a lap, a stop or a pass:
        // cached as one texture, so a new gap repaints its readout alone.
        Row(
            Modifier.staticLayer(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(CompactGapBetween),
        ) {
            PositionText(car.position, isPlayer, MaterialTheme.typography.labelLarge, Modifier.width(CompactPos))
            TeamBar(driver, 18.dp)
            CodeText(driverCode(driver?.name, car.index), isPlayer, Modifier.width(CompactCode))
            Text(
                car.lapNumber.toString(),
                modifier = Modifier.width(CompactLap),
                style = numberStyle(),
                color = MaterialTheme.colorScheme.outline,
                maxLines = 1,
            )
            LastLapText(car, fastest, showPit = true, Modifier.width(CompactLast))
        }
        if (flags.isNotEmpty()) {
            Text(
                flags,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.error,
                maxLines = 1,
            )
        }
        Readout(
            runs = { listOf(ReadoutRun(gap(), gapColor)) },
            style = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace, fontWeight = FontWeight.SemiBold),
            modifier = Modifier.weight(1f),
            alignEnd = true,
        )
        Row(Modifier.width(CompactTyre), horizontalArrangement = Arrangement.End) {
            if (tyre == null) {
                Text("—", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.outline)
            } else {
                Text(
                    compoundText(tyre, labels),
                    style = MaterialTheme.typography.labelMedium,
                    color = tyreTextColor(tyre.visualCompound),
                    maxLines = 1,
                )
                Text(
                    " ${tyre.ageLaps}",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.outline,
                    maxLines = 1,
                )
            }
        }
    }
}

@Composable
private fun TimingTableHeader() {
    Row(
        modifier = Modifier.fillMaxWidth().height(32.dp).padding(horizontal = TablePadding),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(TableGapBetween),
    ) {
        TimingHeaderText("Pos", Modifier.width(TablePos))
        Spacer(Modifier.width(TeamBarWidth))
        TimingHeaderText("Driver", Modifier.weight(1f))
        TimingHeaderText("Lap", Modifier.width(TableLap))
        TimingHeaderText("Last lap", Modifier.width(TableLast))
        TimingHeaderText("Gap", Modifier.width(TableGap), TextAlign.End)
        TimingHeaderText("Tyre", Modifier.width(TableTyre))
        TimingHeaderText("Status", Modifier.width(TableStatus), TextAlign.End)
    }
}

/** Landscape: every column, with the driver's number and full name. */
@Composable
private fun TimingTableRow(
    car: TimingCarEntry,
    gap: () -> String,
    driver: TimingDriver?,
    tyre: TimingTyreStatus?,
    labels: Map<String, String>,
    isPlayer: Boolean,
    fastest: Boolean,
) {
    val gapColor = if (resultLabel(car.resultStatus) != null) {
        MaterialTheme.colorScheme.error
    } else {
        MaterialTheme.colorScheme.onSurface
    }
    val status = tableStatus(car)
    val name = buildString {
        if (isPlayer) append("You · ")
        if (driver != null && driver.raceNumber > 0) append("#${driver.raceNumber} ")
        if (driver != null) append(driver.name)
    }

    Row(
        modifier = Modifier.fillMaxWidth().height(TableRowHeight).padding(horizontal = TablePadding),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(TableGapBetween),
    ) {
        PositionText(car.position, isPlayer, MaterialTheme.typography.titleMedium, Modifier.width(TablePos))
        TeamBar(driver, 20.dp)
        Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
            CodeText(driverCode(driver?.name, car.index), isPlayer, Modifier)
            Spacer(Modifier.width(10.dp))
            Text(
                name,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.outline,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Text(
            car.lapNumber.toString(),
            modifier = Modifier.width(TableLap),
            style = numberStyle(13),
            color = MaterialTheme.colorScheme.outline,
            maxLines = 1,
        )
        LastLapText(car, fastest, showPit = false, Modifier.width(TableLast), size = 13)
        Readout(
            runs = { listOf(ReadoutRun(gap(), gapColor)) },
            style = MaterialTheme.typography.bodyMedium.copy(
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.SemiBold,
                fontSize = 15.sp,
            ),
            modifier = Modifier.width(TableGap),
            alignEnd = true,
        )
        Row(Modifier.width(TableTyre), verticalAlignment = Alignment.CenterVertically) {
            if (tyre == null) {
                Text("—", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.outline)
            } else {
                Spacer(Modifier.size(7.dp).background(tyreTextColor(tyre.visualCompound), CircleShape))
                Spacer(Modifier.width(8.dp))
                Text(
                    "${compoundText(tyre, labels)} · ${tyre.ageLaps} laps",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        Text(
            status,
            modifier = Modifier.width(TableStatus),
            style = MaterialTheme.typography.labelSmall,
            color = if (car.pitStatus > 0) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.error,
            textAlign = TextAlign.End,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
private fun TimingHeaderText(
    text: String,
    modifier: Modifier,
    alignment: TextAlign = TextAlign.Start,
) {
    Text(
        text,
        modifier = modifier,
        style = MaterialTheme.typography.labelSmall,
        color = MaterialTheme.colorScheme.outline,
        textAlign = alignment,
        maxLines = 1,
    )
}

@Composable
private fun PositionText(position: Int, isPlayer: Boolean, style: TextStyle, modifier: Modifier) {
    Text(
        position.toString(),
        modifier = modifier,
        color = if (isPlayer) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
        style = style,
        maxLines = 1,
    )
}

@Composable
private fun TeamBar(driver: TimingDriver?, height: Dp) {
    Spacer(
        Modifier
            .width(TeamBarWidth)
            .height(height)
            .background(parseColor(driver?.teamColor), RoundedCornerShape(2.dp)),
    )
}

@Composable
private fun CodeText(code: String, isPlayer: Boolean, modifier: Modifier) {
    Text(
        code,
        modifier = modifier,
        color = if (isPlayer) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
        style = MaterialTheme.typography.labelLarge,
        maxLines = 1,
        overflow = TextOverflow.Clip,
    )
}

/** The last lap: purple when it is the quickest, and, where [showPit], the stop in its place. */
@Composable
private fun LastLapText(car: TimingCarEntry, fastest: Boolean, showPit: Boolean, modifier: Modifier, size: Int = 12) {
    val pit = showPit && car.pitStatus > 0
    Text(
        when {
            pit && car.pitStatus == 1 -> "Pitting"
            pit -> "In pit"
            else -> formatLapTime(car.lastLapMs)
        },
        modifier = modifier,
        style = numberStyle(size),
        color = when {
            pit -> MaterialTheme.colorScheme.tertiary
            fastest -> fastestLapColor()
            else -> MaterialTheme.colorScheme.onSurfaceVariant
        },
        maxLines = 1,
    )
}

@Composable
private fun numberStyle(size: Int = 12): TextStyle = MaterialTheme.typography.bodySmall.copy(
    fontFamily = FontFamily.Monospace,
    fontFeatureSettings = "tnum",
    fontSize = size.sp,
)

private fun driverCode(name: String?, index: Int): String {
    if (name.isNullOrBlank()) return "CAR ${index + 1}"
    return name.trim().split(Regex("\\s+")).last().take(3).uppercase(Locale.ROOT)
}

private fun formatLapTime(milliseconds: Int): String {
    if (milliseconds <= 0) return "—"
    val minutes = milliseconds / 60_000
    val seconds = (milliseconds % 60_000) / 1_000
    val millis = milliseconds % 1_000
    return "%d:%02d.%03d".format(Locale.ROOT, minutes, seconds, millis)
}

/** The gap shown for car [index] in [state]: a result such as DNF, else its gap. */
private fun gapText(state: TimingTowerState, index: Int): String {
    val car = state.cars.firstOrNull { it.index == index } ?: return "—"
    return resultLabel(car.resultStatus) ?: formatGap(car.gapMs, car.position)
}

private fun formatGap(milliseconds: Int, position: Int): String {
    if (position == 1) return "Leader"
    if (milliseconds <= 0) return "—"
    val seconds = milliseconds / 1_000.0
    return if (seconds < 60) {
        "+%.3f".format(Locale.ROOT, seconds)
    } else {
        val minutes = (seconds / 60).toInt()
        "+%d:%06.3f".format(Locale.ROOT, minutes, seconds % 60)
    }
}

private fun resultLabel(status: Int): String? = when (status) {
    4 -> "DNF"
    5 -> "DSQ"
    7 -> "RET"
    else -> null
}

/** The short flags shown beside a portrait row's gap. */
private fun flagLabels(car: TimingCarEntry, includePit: Boolean): String = buildList {
    if (includePit && car.pitStatus > 0) add(if (car.pitStatus == 1) "PIT" else "PIT LANE")
    if (car.lapInvalid) add("INV")
    if (car.penaltiesSeconds > 0) add("+${car.penaltiesSeconds}s")
    if (car.driveThroughPenalties > 0) add("DT")
    if (car.stopGoPenalties > 0) add("SG")
}.joinToString(" · ")

/** The landscape table's status column, in words. */
private fun tableStatus(car: TimingCarEntry): String = buildList {
    if (car.pitStatus > 0) add(if (car.pitStatus == 1) "Pitting" else "In pit lane")
    if (car.lapInvalid) add("Lap invalid")
    if (car.penaltiesSeconds > 0) add("+${car.penaltiesSeconds}s penalty")
    if (car.driveThroughPenalties > 0) add("Drive-through")
    if (car.stopGoPenalties > 0) add("Stop-go")
}.joinToString(" · ")

private fun compoundText(tyre: TimingTyreStatus, labels: Map<String, String>): String =
    labels["tyre.actual.${tyre.actualCompound}"] ?: compoundLabel(tyre.actualCompound)

private fun compoundLabel(compound: Int) = when (compound) {
    7 -> "INT"; 8 -> "WET"; 16 -> "C5"; 17 -> "C4"; 18 -> "C3"
    19 -> "C2"; 20 -> "C1"; 21 -> "C0"; 22 -> "C6"; else -> compound.toString()
}

/**
 * A compound's colour for tyre text: the hard's white reads black on a light
 * surface, and the soft's red is lifted on a dark one so small text stays legible.
 */
@Composable
private fun tyreTextColor(visualCompound: Int): Color {
    val light = MaterialTheme.colorScheme.surface.luminance() >= 0.5f
    return when {
        visualCompound == 18 && light -> Color.Black
        visualCompound == 16 && !light -> Color(0xffef5350)
        else -> compoundColor(visualCompound)
    }
}

private fun compoundColor(compound: Int) = when (compound) {
    16 -> Color(0xffe53935); 17 -> Color(0xffffc107); 18 -> Color(0xffeceff1)
    7 -> Color(0xff43a047); 8 -> Color(0xff1e88e5); else -> Color.Unspecified
}

/** The fastest-lap purple, darkened on a light surface. */
@Composable
private fun fastestLapColor(): Color =
    if (MaterialTheme.colorScheme.surface.luminance() >= 0.5f) Color(0xff6a3fb5) else Color(0xffc5b3f0)

private fun parseColor(value: String?): Color = try {
    Color(android.graphics.Color.parseColor(value ?: "#8e8e8e"))
} catch (_: IllegalArgumentException) {
    Color(0xff8e8e8e)
}
