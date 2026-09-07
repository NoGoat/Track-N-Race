package com.tracknrace.android.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.tracknrace.android.TimingCarEntry
import com.tracknrace.android.TimingDriver
import com.tracknrace.android.TimingTowerState
import com.tracknrace.android.TimingTyreStatus
import com.tracknrace.android.needsParticipantsRefresh
import java.util.Locale
import kotlinx.coroutines.delay

internal const val PARTICIPANTS_RETRY_DELAY_MS = 3_000L

@Composable
internal fun TimingScreen(
    state: TimingTowerState,
    labels: Map<String, String>,
    oneLine: Boolean,
    onRequestParticipants: () -> Unit,
    active: Boolean = true,
) {
    val cars = state.cars
        .filter { it.position > 0 && it.resultStatus >= 1 }
        .sortedBy(TimingCarEntry::position)
    val participantRosterMissing = state.needsParticipantsRefresh()

    LaunchedEffect(participantRosterMissing, active) {
        if (!active || !participantRosterMissing) return@LaunchedEffect
        while (true) {
            delay(PARTICIPANTS_RETRY_DELAY_MS)
            onRequestParticipants()
        }
    }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val wide = maxWidth >= 600.dp
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(
                horizontal = if (wide) 16.dp else 12.dp,
                vertical = 12.dp,
            ),
        ) {
            item {
                Card(
                    modifier = Modifier.fillMaxWidth(),
                    shape = MaterialTheme.shapes.large,
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
                    ),
                ) {
                    TimingHeader(cars.size)
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    if (cars.isEmpty()) {
                        Text(
                            "Waiting for timing data…",
                            modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 36.dp),
                            textAlign = TextAlign.Center,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    } else {
                        if (wide && !oneLine) TimingTableHeader()
                        cars.forEachIndexed { index, car ->
                            if (index > 0 || (wide && !oneLine)) {
                                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                            }
                            if (oneLine) {
                                TimingOneLineRow(
                                    car = car,
                                    driver = state.drivers[car.index],
                                    tyre = state.tyreStatuses[car.index],
                                    labels = labels,
                                )
                            } else {
                                TimingRow(
                                    car = car,
                                    driver = state.drivers[car.index],
                                    tyre = state.tyreStatuses[car.index],
                                    labels = labels,
                                    isPlayer = car.index == state.playerIndex,
                                    wide = wide,
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun TimingOneLineRow(
    car: TimingCarEntry,
    driver: TimingDriver?,
    tyre: TimingTyreStatus?,
    labels: Map<String, String>,
) {
    val gap = resultLabel(car.resultStatus) ?: formatGap(car.gapMs, car.position)
    val tyreText = tyre?.let {
        val compound = labels["tyre.actual.${it.actualCompound}"] ?: compoundLabel(it.actualCompound)
        "$compound ${it.ageLaps}L"
    } ?: "—"
    val tyreColor = tyre?.let { compoundColor(it.visualCompound) }
        ?: MaterialTheme.colorScheme.onSurfaceVariant
    val numberStyle = MaterialTheme.typography.bodySmall.copy(
        fontFamily = FontFamily.Monospace,
        fontFeatureSettings = "tnum",
    )

    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 9.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        PositionText(car.position, Modifier.width(36.dp))
        Text(
            driverCode(driver?.name, car.index),
            modifier = Modifier.width(42.dp),
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
            overflow = TextOverflow.Clip,
        )
        Text(
            car.lapNumber.toString(),
            modifier = Modifier.width(24.dp),
            style = numberStyle,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            maxLines = 1,
        )
        Text(
            formatLapTime(car.lastLapMs),
            modifier = Modifier.weight(1f),
            style = numberStyle,
            fontWeight = FontWeight.SemiBold,
            textAlign = TextAlign.End,
            maxLines = 1,
        )
        Text(
            tyreText,
            modifier = Modifier.width(58.dp),
            color = tyreColor,
            style = MaterialTheme.typography.bodySmall,
            fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center,
            maxLines = 1,
        )
        Text(
            gap,
            modifier = Modifier.width(74.dp),
            style = numberStyle,
            fontWeight = FontWeight.SemiBold,
            textAlign = TextAlign.End,
            maxLines = 1,
        )
    }
}

@Composable
private fun TimingHeader(count: Int) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                "Live order",
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                "Timing tower",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
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
private fun TimingTableHeader() {
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        TimingHeaderText("Pos", Modifier.width(52.dp))
        TimingHeaderText("Driver", Modifier.weight(1f))
        TimingHeaderText("Lap", Modifier.width(54.dp))
        TimingHeaderText("Last lap", Modifier.width(112.dp))
        TimingHeaderText("Gap", Modifier.width(96.dp))
        TimingHeaderText("Tyre", Modifier.width(76.dp))
        TimingHeaderText("Status", Modifier.width(116.dp), TextAlign.End)
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
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        fontWeight = FontWeight.Bold,
        textAlign = alignment,
        maxLines = 1,
    )
}

@Composable
private fun TimingRow(
    car: TimingCarEntry,
    driver: TimingDriver?,
    tyre: TimingTyreStatus?,
    labels: Map<String, String>,
    isPlayer: Boolean,
    wide: Boolean,
) {
    val driverCode = driverCode(driver?.name, car.index)
    val gap = resultLabel(car.resultStatus) ?: formatGap(car.gapMs, car.position)
    val tyreText = tyre?.let {
        val compound = labels["tyre.actual.${it.actualCompound}"] ?: compoundLabel(it.actualCompound)
        "$compound ${it.ageLaps}L"
    } ?: "—"
    val tyreColor = tyre?.let { compoundColor(it.visualCompound) }
        ?: MaterialTheme.colorScheme.onSurfaceVariant
    val status = statusLabel(car)

    if (wide) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 11.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            PositionText(car.position, Modifier.width(52.dp))
            DriverIdentity(driverCode, driver, isPlayer, Modifier.weight(1f), showFullName = true)
            TimingValue(car.lapNumber.toString(), Modifier.width(54.dp))
            TimingValue(formatLapTime(car.lastLapMs), Modifier.width(112.dp), strong = true)
            TimingValue(gap, Modifier.width(96.dp), strong = car.position == 1)
            Text(
                tyreText,
                modifier = Modifier.width(76.dp),
                color = tyreColor,
                style = MaterialTheme.typography.bodySmall,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
            StatusText(status, Modifier.width(116.dp), TextAlign.End)
        }
    } else {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 11.dp),
            verticalAlignment = Alignment.Top,
        ) {
            PositionText(car.position, Modifier.width(44.dp))
            Column(modifier = Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    DriverIdentity(driverCode, driver, isPlayer, Modifier.weight(1f), showFullName = false)
                    Spacer(Modifier.width(8.dp))
                    Text(
                        gap,
                        style = MaterialTheme.typography.titleSmall.copy(
                            fontFamily = FontFamily.Monospace,
                            fontFeatureSettings = "tnum",
                        ),
                        color = if (resultLabel(car.resultStatus) != null) {
                            MaterialTheme.colorScheme.error
                        } else {
                            MaterialTheme.colorScheme.onSurface
                        },
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                    )
                }
                Spacer(Modifier.height(5.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "Lap ${car.lapNumber}  ·  ${formatLapTime(car.lastLapMs)}  ·  ",
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        style = MaterialTheme.typography.bodySmall.copy(
                            fontFamily = FontFamily.Monospace,
                            fontFeatureSettings = "tnum",
                        ),
                        maxLines = 1,
                    )
                    Text(
                        tyreText,
                        modifier = Modifier.weight(1f),
                        color = tyreColor,
                        style = MaterialTheme.typography.bodySmall,
                        fontWeight = FontWeight.Bold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    if (status.isNotEmpty()) {
                        Spacer(Modifier.width(8.dp))
                        StatusText(status, Modifier.widthIn(max = 104.dp), TextAlign.End)
                    }
                }
            }
        }
    }
}

@Composable
private fun PositionText(position: Int, modifier: Modifier) {
    Text(
        "P$position",
        modifier = modifier,
        color = MaterialTheme.colorScheme.primary,
        style = MaterialTheme.typography.titleSmall.copy(
            fontFamily = FontFamily.Monospace,
            fontFeatureSettings = "tnum",
        ),
        fontWeight = FontWeight.Bold,
    )
}

@Composable
private fun DriverIdentity(
    code: String,
    driver: TimingDriver?,
    isPlayer: Boolean,
    modifier: Modifier,
    showFullName: Boolean,
) {
    Row(modifier = modifier, verticalAlignment = Alignment.CenterVertically) {
        Spacer(
            modifier = Modifier
                .size(8.dp)
                .background(parseColor(driver?.teamColor), RoundedCornerShape(2.dp)),
        )
        Spacer(Modifier.width(8.dp))
        Text(
            code,
            style = MaterialTheme.typography.titleSmall,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
        )
        if (driver != null && driver.raceNumber > 0) {
            Spacer(Modifier.width(6.dp))
            Text(
                "#${driver.raceNumber}",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (showFullName && driver != null) {
            Spacer(Modifier.width(10.dp))
            Text(
                driver.name,
                modifier = Modifier.weight(1f),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        if (isPlayer) {
            Spacer(Modifier.width(8.dp))
            Surface(
                shape = MaterialTheme.shapes.extraSmall,
                color = MaterialTheme.colorScheme.secondaryContainer,
                contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
            ) {
                Text(
                    "YOU",
                    modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                    style = MaterialTheme.typography.labelSmall,
                    fontWeight = FontWeight.Bold,
                )
            }
        }
    }
}

@Composable
private fun TimingValue(text: String, modifier: Modifier, strong: Boolean = false) {
    Text(
        text,
        modifier = modifier,
        color = if (strong) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
        style = MaterialTheme.typography.bodySmall.copy(
            fontFamily = FontFamily.Monospace,
            fontFeatureSettings = "tnum",
        ),
        fontWeight = if (strong) FontWeight.SemiBold else FontWeight.Normal,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
    )
}

@Composable
private fun StatusText(text: String, modifier: Modifier, alignment: TextAlign) {
    Text(
        text,
        modifier = modifier,
        color = if (text.contains("INV") || text.contains("DNF") || text.contains("DSQ")) {
            MaterialTheme.colorScheme.error
        } else {
            MaterialTheme.colorScheme.tertiary
        },
        style = MaterialTheme.typography.labelSmall,
        fontWeight = FontWeight.Bold,
        textAlign = alignment,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
    )
}

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

private fun statusLabel(car: TimingCarEntry): String = buildList {
    if (car.pitStatus > 0) add(if (car.pitStatus == 1) "PIT" else "PIT LANE")
    if (car.lapInvalid) add("INV")
    if (car.penaltiesSeconds > 0) add("+${car.penaltiesSeconds}s")
    if (car.driveThroughPenalties > 0) add("DT")
    if (car.stopGoPenalties > 0) add("SG")
}.joinToString(" · ")

private fun compoundLabel(compound: Int) = when (compound) {
    7 -> "INT"; 8 -> "WET"; 16 -> "C5"; 17 -> "C4"; 18 -> "C3"
    19 -> "C2"; 20 -> "C1"; 21 -> "C0"; 22 -> "C6"; else -> compound.toString()
}

private fun compoundColor(compound: Int) = when (compound) {
    16 -> Color(0xffe53935); 17 -> Color(0xffffc107); 18 -> Color(0xffeceff1)
    7 -> Color(0xff43a047); 8 -> Color(0xff1e88e5); else -> Color.Unspecified
}

private fun parseColor(value: String?): Color = try {
    Color(android.graphics.Color.parseColor(value ?: "#8e8e8e"))
} catch (_: IllegalArgumentException) {
    Color(0xff8e8e8e)
}
