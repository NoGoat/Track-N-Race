package com.tracknrace.android.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.requiredSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import com.tracknrace.android.AnalysisController
import com.tracknrace.android.DEFAULT_COMPARE_LABEL
import com.tracknrace.android.DEFAULT_CURRENT_LABEL
import com.tracknrace.android.DEFAULT_LAP_A_LABEL
import com.tracknrace.android.DEFAULT_LAP_B_LABEL
import com.tracknrace.android.DEFAULT_MAP_COMPARISON_COLOR
import com.tracknrace.android.DEFAULT_MAP_CURRENT_COLOR
import com.tracknrace.android.PlaybackCatalog
import com.tracknrace.android.PlaybackLapInfo
import com.tracknrace.android.formatLapTimeMs

/** Which lap the picker is choosing. */
internal enum class LapTarget { COMPARE, LAP_A, LAP_B }

/**
 * Picks the compared laps: following the desktop's lap against a chosen one,
 * or two fixed laps. Rows read like the desktop's lap selector: compound in
 * its tyre colour, lap number and time, and FL on the fastest lap.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun AnalysisLapSheet(
    analysis: AnalysisController,
    catalog: PlaybackCatalog,
    playbackLap: Int,
    initialTarget: LapTarget,
    lapAColor: Color,
    lapBColor: Color,
    onDismiss: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    var target by remember {
        mutableStateOf(if (analysis.fixedMode) initialTarget.takeIf { it != LapTarget.COMPARE } ?: LapTarget.LAP_A else LapTarget.COMPARE)
    }
    // The lap whose colour is being picked: Lap A for the first, Lap B for the second.
    var colorTarget by remember { mutableStateOf<LapTarget?>(null) }
    val fixed = analysis.fixedMode
    val selected = when (target) {
        LapTarget.COMPARE -> analysis.compareLap
        LapTarget.LAP_A -> analysis.lapA
        LapTarget.LAP_B -> analysis.lapB
    }

    fun select(lap: Int) {
        when (target) {
            LapTarget.COMPARE -> analysis.selectCompareLap(lap)
            LapTarget.LAP_A -> analysis.selectLapA(lap)
            LapTarget.LAP_B -> analysis.selectLapB(lap)
        }
        // Choosing Lap A with no Lap B yet moves straight on to Lap B.
        if (target == LapTarget.LAP_A && analysis.lapB <= 0) target = LapTarget.LAP_B else onDismiss()
    }

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        LazyColumn(contentPadding = PaddingValues(bottom = 24.dp)) {
            item {
                Text(
                    "Compare",
                    modifier = Modifier.padding(start = 24.dp, end = 24.dp, bottom = 12.dp),
                    style = MaterialTheme.typography.titleLarge,
                )
            }
            item {
                SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
                    SegmentedButton(
                        selected = !fixed,
                        onClick = {
                            analysis.useFixedLaps(false)
                            target = LapTarget.COMPARE
                        },
                        shape = SegmentedButtonDefaults.itemShape(0, 2),
                        enabled = catalog.active,
                    ) { Text("Follow playback") }
                    SegmentedButton(
                        selected = fixed,
                        onClick = {
                            if (!fixed) {
                                // Start from what is on screen now.
                                if (analysis.lapA <= 0 && playbackLap > 0) analysis.selectLapA(playbackLap)
                                if (analysis.lapB <= 0 && analysis.compareLap > 0) analysis.selectLapB(analysis.compareLap)
                                analysis.useFixedLaps(true)
                            }
                            target = if (analysis.lapA <= 0) LapTarget.LAP_A else LapTarget.LAP_B
                        },
                        shape = SegmentedButtonDefaults.itemShape(1, 2),
                        enabled = catalog.active,
                    ) { Text("Two laps") }
                }
            }
            if (fixed) {
                item {
                    SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 12.dp)) {
                        listOf(LapTarget.LAP_A to analysis.lapA, LapTarget.LAP_B to analysis.lapB).forEachIndexed { index, (option, lap) ->
                            SegmentedButton(
                                selected = target == option,
                                onClick = { target = option },
                                shape = SegmentedButtonDefaults.itemShape(index, 2),
                                icon = {
                                    Box(Modifier.requiredSize(10.dp).background(if (index == 0) lapAColor else lapBColor, CircleShape))
                                },
                            ) {
                                Text("${if (index == 0) "Lap A" else "Lap B"} · ${if (lap > 0) "L$lap" else "—"}")
                            }
                        }
                    }
                }
            }
            // The two laps' names and colours, as the chart, map and headers show them.
            item {
                val config = analysis.config
                Column(Modifier.padding(top = 8.dp)) {
                    if (fixed) {
                        LapAppearanceRow(lapAColor, "Lap A colour", config.lapALabel, DEFAULT_LAP_A_LABEL, { colorTarget = LapTarget.LAP_A }) { value ->
                            analysis.updateConfig { it.copy(lapALabel = value) }
                        }
                        LapAppearanceRow(lapBColor, "Lap B colour", config.lapBLabel, DEFAULT_LAP_B_LABEL, { colorTarget = LapTarget.LAP_B }) { value ->
                            analysis.updateConfig { it.copy(lapBLabel = value) }
                        }
                    } else {
                        LapAppearanceRow(lapAColor, "Current lap colour", config.currentLabel, DEFAULT_CURRENT_LABEL, { colorTarget = LapTarget.LAP_A }) { value ->
                            analysis.updateConfig { it.copy(currentLabel = value) }
                        }
                        LapAppearanceRow(lapBColor, "Comparison lap colour", config.compareLabel, DEFAULT_COMPARE_LABEL, { colorTarget = LapTarget.LAP_B }) { value ->
                            analysis.updateConfig { it.copy(compareLabel = value) }
                        }
                    }
                }
            }
            item {
                Text(
                    when (target) {
                        LapTarget.COMPARE -> "Comparison lap"
                        LapTarget.LAP_A -> "Lap A"
                        LapTarget.LAP_B -> "Lap B"
                    },
                    modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 16.dp, bottom = 4.dp),
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
            if (target == LapTarget.COMPARE) {
                item {
                    LapRow(AnnotatedString("None"), fastest = false, selected = selected <= 0) { select(0) }
                }
            }
            if (catalog.laps.isEmpty()) {
                item {
                    Text(
                        "No laps yet",
                        modifier = Modifier.padding(24.dp),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            items(catalog.laps, key = { it.lapNumber }) { lap ->
                LapRow(
                    lapLabel(lap, MaterialTheme.colorScheme.onSurface, MaterialTheme.colorScheme.surface),
                    fastest = lap.lapNumber == catalog.fastestLap,
                    selected = lap.lapNumber == selected,
                ) { select(lap.lapNumber) }
            }
        }
    }

    colorTarget?.let { lap ->
        val first = lap == LapTarget.LAP_A
        val config = analysis.config
        ColorPickerDialog(
            title = when {
                fixed && first -> "Lap A colour"
                fixed -> "Lap B colour"
                first -> "Current lap colour"
                else -> "Comparison lap colour"
            },
            initial = if (first) config.mapCurrentColor else config.mapComparisonColor,
            default = if (first) DEFAULT_MAP_CURRENT_COLOR else DEFAULT_MAP_COMPARISON_COLOR,
            onPick = { color ->
                analysis.updateConfig { if (first) it.copy(mapCurrentColor = color) else it.copy(mapComparisonColor = color) }
                colorTarget = null
            },
            onDismiss = { colorTarget = null },
        )
    }
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
internal fun LapRow(label: AnnotatedString, fastest: Boolean, selected: Boolean, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(label, fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal) },
        leadingContent = { RadioButton(selected = selected, onClick = null) },
        trailingContent = if (fastest) {
            { FastestLapChip() }
        } else {
            null
        },
        colors = ListItemDefaults.colors(
            containerColor = if (selected) MaterialTheme.colorScheme.secondaryContainer.copy(alpha = 0.45f) else Color.Transparent,
        ),
        modifier = Modifier.selectable(selected = selected, role = Role.RadioButton, onClick = onClick),
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
