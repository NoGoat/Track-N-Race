package com.nogoat.tracknrace.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.nogoat.tracknrace.AnalysisController
import com.nogoat.tracknrace.DEFAULT_COMPARE_LABEL
import com.nogoat.tracknrace.DEFAULT_CURRENT_LABEL
import com.nogoat.tracknrace.DEFAULT_LAP_A_LABEL
import com.nogoat.tracknrace.DEFAULT_LAP_B_LABEL
import com.nogoat.tracknrace.DEFAULT_MAP_COMPARISON_COLOR
import com.nogoat.tracknrace.DEFAULT_MAP_CURRENT_COLOR
import com.nogoat.tracknrace.PlaybackCatalog
import com.nogoat.tracknrace.PlaybackLapInfo
import com.nogoat.tracknrace.R
import com.nogoat.tracknrace.formatLapTimeMs
import kotlinx.coroutines.delay

private const val DRIVER_LAPS_SYNC_INTERVAL_MS = 1_000L

/** Which lap a sheet chooses: the first (current, or Lap A) or the second (comparison, or Lap B). */
internal enum class LapTarget { LAP_A, LAP_B }

/** A driver whose laps a lap sheet can list, as the desktop's driver menus show one. */
internal data class LapSheetDriver(
    val index: Int,
    val name: String,
    val raceNumber: Int,
    val teamColor: Color,
    /** Only this driver's public telemetry was recorded. */
    val restricted: Boolean,
)

/**
 * One lap's sheet, opened from its button in the lap row. Its colour and name
 * come first, as the chart, map and headers show them. The first lap follows
 * playback or is fixed to a chosen lap; the second has quick picks for the
 * fastest and previous laps, then every lap. Rows read like the desktop's lap
 * selector: compound in its tyre colour, lap number and time, and FL on the
 * fastest lap.
 *
 * With [drivers], a driver row above the laps opens the roster in place; the
 * laps listed are the chosen driver's, and choosing one of them gives this
 * lap that driver.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun AnalysisLapSheet(
    analysis: AnalysisController,
    catalog: PlaybackCatalog,
    playbackLap: Int,
    target: LapTarget,
    lapAColor: Color,
    lapBColor: Color,
    /** The recording's drivers while each lap can have its own; empty for no choice. */
    drivers: List<LapSheetDriver>,
    onDismiss: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    var colorOpen by remember { mutableStateOf(false) }
    val fixed = analysis.fixedMode
    val first = target == LapTarget.LAP_A
    val config = analysis.config
    val lapA = if (fixed) analysis.lapA else playbackLap
    val lapB = if (fixed) analysis.lapB else analysis.compareLap
    // Each lap's driver, -1 for the one playing; the followed lap is always theirs.
    val lapADriver = if (fixed) analysis.lapADriver else -1
    val lapBDriver = if (fixed) analysis.lapBDriver else analysis.compareDriver
    val ownDriver = if (first) lapADriver else lapBDriver

    // The driver whose laps are listed. It becomes this lap's driver only
    // when one of those laps is chosen.
    var driver by remember(catalog.generation) { mutableIntStateOf(ownDriver) }
    var driversOpen by remember { mutableStateOf(false) }
    val driverChoice = drivers.size > 1
    val shownDriver = drivers.firstOrNull { it.index == if (driver < 0) catalog.driverIndex else driver }
    val laps = analysis.lapsOf(catalog, driver)
    val fastestLap = analysis.fastestLapOf(catalog, driver)
    // Another driver's laps are fetched when first listed; asked again on a
    // slow tick if the reply went missing.
    LaunchedEffect(catalog, driver) {
        while (true) {
            analysis.syncDriverLaps(catalog, listOf(driver))
            delay(DRIVER_LAPS_SYNC_INTERVAL_MS)
        }
    }

    // Fixing the first lap carries the comparison over as the second.
    fun fixLapA(lap: Int) {
        if (!fixed) {
            analysis.selectLapB(analysis.compareLap, analysis.compareDriver)
            analysis.useFixedLaps(true)
        }
        analysis.selectLapA(lap, driver)
        onDismiss()
    }

    fun follow(enabled: Boolean) {
        if (enabled == !fixed) return
        if (enabled) {
            if (analysis.lapB > 0) analysis.selectCompareLap(analysis.lapB, analysis.lapBDriver)
            analysis.useFixedLaps(false)
            // The followed lap is the driver playing.
            driver = -1
        } else {
            // Start from what is on screen now.
            if (playbackLap > 0) analysis.selectLapA(playbackLap, -1)
            analysis.selectLapB(analysis.compareLap, analysis.compareDriver)
            analysis.useFixedLaps(true)
        }
    }

    fun selectLapB(lap: Int) {
        val lapDriver = if (lap > 0) driver else -1
        if (fixed) analysis.selectLapB(lap, lapDriver) else analysis.selectCompareLap(lap, lapDriver)
        onDismiss()
    }

    fun selectDriver(index: Int) {
        driver = if (index == catalog.driverIndex) -1 else index
        driversOpen = false
    }

    val colorTitle = when {
        fixed && first -> "Lap A colour"
        fixed -> "Lap B colour"
        first -> "Current lap colour"
        else -> "Comparison lap colour"
    }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        sheetGesturesEnabled = false,
        dragHandle = null,
    ) {
        LazyColumn(contentPadding = PaddingValues(bottom = 24.dp)) {
            item {
                Column(Modifier.padding(start = 24.dp, end = 24.dp, top = 24.dp, bottom = 12.dp)) {
                    Text(if (first) "Current lap" else "Comparison lap", style = MaterialTheme.typography.titleLarge)
                    Text(
                        if (first) {
                            if (fixed) "Lap A" else "Lap A · follows playback"
                        } else {
                            "Lap B" + if (lapA > 0) " · compared with Lap $lapA" else ""
                        },
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            // This lap's name and colour.
            item {
                if (first) {
                    if (fixed) {
                        LapAppearanceRow(lapAColor, colorTitle, config.lapALabel, DEFAULT_LAP_A_LABEL, { colorOpen = true }) { value ->
                            analysis.updateConfig { it.copy(lapALabel = value) }
                        }
                    } else {
                        LapAppearanceRow(lapAColor, colorTitle, config.currentLabel, DEFAULT_CURRENT_LABEL, { colorOpen = true }) { value ->
                            analysis.updateConfig { it.copy(currentLabel = value) }
                        }
                    }
                } else if (fixed) {
                    LapAppearanceRow(lapBColor, colorTitle, config.lapBLabel, DEFAULT_LAP_B_LABEL, { colorOpen = true }) { value ->
                        analysis.updateConfig { it.copy(lapBLabel = value) }
                    }
                } else {
                    LapAppearanceRow(lapBColor, colorTitle, config.compareLabel, DEFAULT_COMPARE_LABEL, { colorOpen = true }) { value ->
                        analysis.updateConfig { it.copy(compareLabel = value) }
                    }
                }
            }
            item { HorizontalDivider(Modifier.padding(horizontal = 24.dp, vertical = 4.dp), color = MaterialTheme.colorScheme.outlineVariant) }
            // Whose laps are listed below; the roster opens in place.
            if (driverChoice) {
                item { SheetHeading("Driver") }
                item {
                    DriverRow(
                        driver = shownDriver,
                        selected = false,
                        minHeight = 56.dp,
                        modifier = Modifier.clickable(
                            onClickLabel = if (driversOpen) "Hide drivers" else "Change driver",
                            role = Role.DropdownList,
                        ) { driversOpen = !driversOpen },
                    ) {
                        Icon(
                            painterResource(R.drawable.ic_expand_more),
                            contentDescription = null,
                            modifier = Modifier.size(20.dp).rotate(if (driversOpen) 180f else 0f),
                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                if (driversOpen) {
                    item { HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant) }
                    items(drivers, key = { "driver-${it.index}" }) { option ->
                        val on = option.index == shownDriver?.index
                        DriverRow(
                            driver = option,
                            selected = on,
                            minHeight = 48.dp,
                            modifier = Modifier
                                .background(MaterialTheme.colorScheme.surfaceContainerLowest)
                                .selectable(selected = on, role = Role.RadioButton) { selectDriver(option.index) },
                        )
                    }
                    item { HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant) }
                }
            }
            if (first) {
                item {
                    ListItem(
                        headlineContent = { Text("Follow playback") },
                        supportingContent = { Text("Lap A is always the lap being played") },
                        trailingContent = { Switch(checked = !fixed, onCheckedChange = null, enabled = catalog.active) },
                        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                        modifier = Modifier.toggleable(value = !fixed, enabled = catalog.active, role = Role.Switch, onValueChange = ::follow),
                    )
                }
                item { SheetHeading("Or fix lap A to") }
            } else {
                val fastest = laps?.firstOrNull { it.lapNumber == fastestLap }
                val previous = laps?.firstOrNull { it.lapNumber == lapA - 1 }
                if (fastest != null || previous != null) {
                    item { SheetHeading("Quick pick") }
                    fastest?.let { lap ->
                        item(key = "fastest") {
                            QuickPickRow("Fastest lap", lap, selected = driver == lapBDriver && lapB == lap.lapNumber) { selectLapB(lap.lapNumber) }
                        }
                    }
                    previous?.let { lap ->
                        item(key = "previous") {
                            QuickPickRow("Previous lap", lap, selected = driver == lapBDriver && lapB == lap.lapNumber) { selectLapB(lap.lapNumber) }
                        }
                    }
                }
                item { SheetHeading("Any lap") }
                if (!fixed) {
                    item {
                        LapRow(AnnotatedString("None"), fastest = false, selected = lapB <= 0) { selectLapB(0) }
                    }
                }
            }
            if (laps.isNullOrEmpty()) {
                item {
                    Text(
                        if (laps == null) "Loading laps" else "No laps yet",
                        modifier = Modifier.padding(24.dp),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            items(laps.orEmpty(), key = { it.lapNumber }) { lap ->
                val number = lap.lapNumber
                // The other lap is marked, and the lap playing while following it.
                val isOther = driver == (if (first) lapBDriver else lapADriver) && number == if (first) lapB else lapA
                val own = driver == ownDriver
                LapRow(
                    lapLabel(lap, MaterialTheme.colorScheme.onSurface, MaterialTheme.colorScheme.surface),
                    fastest = number == fastestLap,
                    selected = own && (if (first) fixed && number == lapA else number == lapB),
                    note = when {
                        isOther -> if (first) "lap B" else "lap A"
                        first && !fixed && driver < 0 && number == playbackLap -> "playing"
                        else -> null
                    },
                ) { if (first) fixLapA(number) else selectLapB(number) }
            }
        }
    }

    if (colorOpen) {
        ColorPickerDialog(
            title = colorTitle,
            initial = if (first) config.mapCurrentColor else config.mapComparisonColor,
            default = if (first) DEFAULT_MAP_CURRENT_COLOR else DEFAULT_MAP_COMPARISON_COLOR,
            onPick = { color ->
                analysis.updateConfig { if (first) it.copy(mapCurrentColor = color) else it.copy(mapComparisonColor = color) }
                colorOpen = false
            },
            onDismiss = { colorOpen = false },
        )
    }
}

@Composable
private fun SheetHeading(text: String) {
    Text(
        text,
        modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 16.dp, bottom = 4.dp),
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
    )
}

/**
 * A driver as the desktop's driver menus show one: team colour, race number
 * and name, with Public where only their public telemetry was recorded.
 */
@Composable
private fun DriverRow(
    driver: LapSheetDriver?,
    selected: Boolean,
    minHeight: Dp,
    modifier: Modifier = Modifier,
    trailing: @Composable (() -> Unit)? = null,
) {
    val colors = MaterialTheme.colorScheme
    Row(
        modifier
            .fillMaxWidth()
            .background(if (selected) colors.secondaryContainer.copy(alpha = 0.45f) else Color.Transparent)
            .heightIn(min = minHeight)
            .padding(horizontal = 24.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(width = 3.dp, height = 18.dp).background(driver?.teamColor ?: colors.outlineVariant, RoundedCornerShape(1.5.dp)))
        Text(
            driver?.raceNumber?.takeIf { it > 0 }?.toString().orEmpty(),
            modifier = Modifier.padding(start = 8.dp).width(28.dp),
            style = MaterialTheme.typography.labelLarge.copy(fontFeatureSettings = "tnum"),
            fontWeight = FontWeight.SemiBold,
            color = colors.onSurfaceVariant,
            textAlign = TextAlign.End,
            maxLines = 1,
        )
        Text(
            driver?.name ?: "Driver",
            modifier = Modifier.weight(1f).padding(start = 8.dp),
            style = MaterialTheme.typography.bodyLarge,
            fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        if (driver?.restricted == true) {
            Text(
                "Public",
                modifier = Modifier
                    .padding(start = 8.dp)
                    .border(1.dp, colors.outlineVariant, MaterialTheme.shapes.extraSmall)
                    .padding(horizontal = 6.dp, vertical = 1.dp),
                style = MaterialTheme.typography.labelSmall,
                color = colors.onSurfaceVariant,
            )
        }
        if (trailing != null) {
            Box(Modifier.padding(start = 8.dp)) { trailing() }
        }
    }
}

/** A named shortcut to a lap: "Fastest lap", with the lap and its time beneath. */
@Composable
private fun QuickPickRow(title: String, lap: PlaybackLapInfo, selected: Boolean, enabled: Boolean = true, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(title, fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal) },
        supportingContent = { Text(lapLabel(lap, MaterialTheme.colorScheme.onSurfaceVariant, MaterialTheme.colorScheme.surface)) },
        leadingContent = { RadioButton(selected = selected, onClick = null, enabled = enabled) },
        colors = ListItemDefaults.colors(
            containerColor = if (selected) MaterialTheme.colorScheme.secondaryContainer.copy(alpha = 0.45f) else Color.Transparent,
        ),
        modifier = Modifier
            .selectable(selected = selected, enabled = enabled, role = Role.RadioButton, onClick = onClick)
            .alpha(if (enabled) 1f else 0.38f),
    )
}

/** One radio row of a picker: a lap, or any choice, with FL on the fastest lap. */
/** One lap's colour, tapped to change, beside its name. */
@Composable
private fun LapAppearanceRow(
    color: Color,
    colorLabel: String,
    name: String,
    defaultName: String,
    onColor: () -> Unit,
    onName: (String) -> Unit,
) {
    Row(Modifier.fillMaxWidth().padding(start = 4.dp, end = 16.dp, bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier
                .size(48.dp)
                .clip(RoundedCornerShape(12.dp))
                .clickable(onClickLabel = colorLabel, onClick = onColor),
            contentAlignment = Alignment.Center,
        ) {
            Box(Modifier.size(24.dp).background(color, RoundedCornerShape(6.dp)))
        }
        OutlinedTextField(
            value = name,
            onValueChange = { onName(it.take(40)) },
            modifier = Modifier.weight(1f).padding(start = 4.dp),
            singleLine = true,
            label = { Text(defaultName) },
            placeholder = { Text(defaultName) },
        )
    }
}

@Composable
internal fun LapRow(
    label: AnnotatedString,
    fastest: Boolean,
    selected: Boolean,
    enabled: Boolean = true,
    note: String? = null,
    onClick: () -> Unit,
) {
    ListItem(
        headlineContent = { Text(label, fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal) },
        supportingContent = note?.let { { Text(it) } },
        leadingContent = { RadioButton(selected = selected, onClick = null, enabled = enabled) },
        trailingContent = if (fastest) {
            { FastestLapChip() }
        } else {
            null
        },
        colors = ListItemDefaults.colors(
            containerColor = if (selected) MaterialTheme.colorScheme.secondaryContainer.copy(alpha = 0.45f) else Color.Transparent,
        ),
        modifier = Modifier
            .selectable(selected = selected, enabled = enabled, role = Role.RadioButton, onClick = onClick)
            .alpha(if (enabled) 1f else 0.38f),
    )
}

internal val FastestLapColor = Color(0xffb36bff)

/** The desktop's FL chip: the timing screens' purple on a faint tint. */
@Composable
internal fun FastestLapChip() {
    Text(
        "FL",
        modifier = Modifier
            .background(FastestLapColor.copy(alpha = 0.16f), MaterialTheme.shapes.extraSmall)
            .padding(horizontal = 8.dp, vertical = 2.dp),
        style = MaterialTheme.typography.labelSmall,
        fontWeight = FontWeight.SemiBold,
        color = FastestLapColor,
    )
}

/** "C3 · Lap 8 · 1:13.192", the compound in its tyre colour, as the desktop shows it. */
internal fun lapLabel(lap: PlaybackLapInfo, text: Color, surface: Color): AnnotatedString = buildAnnotatedString {
    if (lap.tyreCompound > 0) {
        var color = compoundColor(lap.visualCompound).takeIf { it != Color.Unspecified } ?: text
        // The hard tyre's white disappears on a light surface.
        if (color.luminance() > 0.8f && surface.luminance() > 0.5f) color = text
        withStyle(SpanStyle(color = color, fontWeight = FontWeight.SemiBold)) { append(compoundLabel(lap.tyreCompound)) }
        append(" · ")
    }
    append("Lap ${lap.lapNumber}")
    formatLapTimeMs(lap.lapTimeMs)?.let { append(" · $it") }
}

private fun compoundLabel(compound: Int) = when (compound) {
    7 -> "INT"; 8 -> "WET"; 16 -> "C5"; 17 -> "C4"; 18 -> "C3"
    19 -> "C2"; 20 -> "C1"; 21 -> "C0"; 22 -> "C6"; else -> compound.toString()
}

private fun compoundColor(compound: Int) = when (compound) {
    16 -> Color(0xffe53935); 17 -> Color(0xffffc107); 18 -> Color(0xffeceff1)
    7 -> Color(0xff43a047); 8 -> Color(0xff1e88e5); else -> Color.Unspecified
}
