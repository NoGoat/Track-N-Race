package com.tracknrace.android.pages

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuAnchorType
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.IconToggleButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.tracknrace.android.AnalysisCatalog
import com.tracknrace.android.AnalysisConfig
import com.tracknrace.android.AnalysisController
import com.tracknrace.android.AnalysisSeries
import com.tracknrace.android.AnalysisTyreRow
import com.tracknrace.android.DEFAULT_COMPARE_LABEL
import com.tracknrace.android.DEFAULT_CURRENT_LABEL
import com.tracknrace.android.DEFAULT_DELTA_NEGATIVE_COLOR
import com.tracknrace.android.DEFAULT_DELTA_POSITIVE_COLOR
import com.tracknrace.android.DEFAULT_LAP_A_LABEL
import com.tracknrace.android.DEFAULT_LAP_B_LABEL
import com.tracknrace.android.DEFAULT_MAP_COMPARISON_COLOR
import com.tracknrace.android.DEFAULT_MAP_CURRENT_COLOR
import com.tracknrace.android.PlaybackCatalog
import com.tracknrace.android.R
import com.tracknrace.android.TelemetryStore

private val FastestLapColor = Color(0xffb36bff)

// Swatches offered by the colour picker: the desktop's default series colours
// plus a few neutrals.
private val Palette = listOf(
    0xF2495C, 0xC4162A, 0xE10600, 0xFF780A, 0xFF9830, 0xF0A500, 0xFADE2A, 0xFFD700,
    0x73BF69, 0x37872D, 0x8AB8FF, 0x5794F2, 0x4488FF, 0xB877DB, 0xBF5FFF, 0xFFFFFF,
).map { (0xff shl 24) or it }

/** A colour being picked, and what it resets to. */
private class ColorTarget(
    val title: String,
    val color: Int,
    val default: Int,
    val apply: (Int) -> Unit,
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun AnalysisConfigSheet(
    store: TelemetryStore,
    analysis: AnalysisController,
    onDismiss: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val config = analysis.config
    val catalog = store.playbackCatalog
    var pickerOpen by remember { mutableStateOf(false) }
    var colorTarget by remember { mutableStateOf<ColorTarget?>(null) }
    val update: ((AnalysisConfig) -> AnalysisConfig) -> Unit = analysis::updateConfig
    fun updateSeries(transform: (List<AnalysisSeries>) -> List<AnalysisSeries>) =
        update { it.copy(series = transform(it.series)) }

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        LazyColumn(contentPadding = PaddingValues(bottom = 32.dp)) {
            item { SectionTitle("Laps") }
            item {
                LapsSection(store, analysis, catalog, config, onColor = { colorTarget = it })
            }
            item { SectionTitle("Chart") }
            item {
                SwitchRow("Individual graphs", "One panel per metric instead of one shared graph", config.individualGraphs) { value ->
                    update { it.copy(individualGraphs = value) }
                }
                SwitchRow("Synced tooltip", "Inspecting shows every panel's values", config.syncedTooltip, enabled = config.individualGraphs) { value ->
                    update { it.copy(syncedTooltip = value) }
                }
                SwitchRow("Sector boundaries", "Sectors instead of distance on the x axis", config.sectorBoundaries) { value ->
                    update { it.copy(sectorBoundaries = value) }
                }
                SwitchRow("Sector delta", "Delta restarts from zero in each sector", config.sectorDelta, enabled = config.sectorBoundaries) { value ->
                    update { it.copy(sectorDelta = value) }
                }
                val allAxes = config.series.all { it.showYAxis }
                OutlinedButton(
                    onClick = { updateSeries { series -> series.map { it.copy(showYAxis = !allAxes) } } },
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                ) { Text(if (allAxes) "Hide all y axes" else "Show all y axes") }
            }
            item {
                Row(
                    Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        "Metrics",
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.titleSmall,
                        color = MaterialTheme.colorScheme.primary,
                        fontWeight = FontWeight.SemiBold,
                    )
                    TextButton(onClick = { pickerOpen = !pickerOpen }) {
                        Icon(
                            painterResource(if (pickerOpen) R.drawable.ic_close else R.drawable.ic_add),
                            contentDescription = null,
                            modifier = Modifier.size(18.dp),
                        )
                        Spacer(Modifier.width(6.dp))
                        Text(if (pickerOpen) "Done" else "Add metrics")
                    }
                }
            }
            if (pickerOpen) {
                item { MetricPicker(config.series, ::updateSeries) }
            }
            if (config.series.isEmpty()) {
                item {
                    Text(
                        "No metrics selected",
                        modifier = Modifier.fillMaxWidth().padding(24.dp),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
            itemsIndexed(config.series, key = { _, item -> item.metricId }) { index, item ->
                SeriesRow(
                    item = item,
                    index = index,
                    count = config.series.size,
                    deltaSupported = catalog.deltaAvailable || !catalog.active,
                    onMove = { delta -> updateSeries { it.moved(item.metricId, delta) } },
                    onChange = { next -> updateSeries { series -> series.map { if (it.metricId == item.metricId) next else it } } },
                    onRemove = { updateSeries { series -> series.filter { it.metricId != item.metricId } } },
                    onColor = { colorTarget = it },
                    modifier = Modifier.animateItem(),
                )
            }
        }
    }

    colorTarget?.let { target ->
        ColorDialog(
            target = target,
            onPick = { color ->
                target.apply(color)
                colorTarget = null
            },
            onDismiss = { colorTarget = null },
        )
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text,
        modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 4.dp),
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        fontWeight = FontWeight.SemiBold,
    )
}

@Composable
private fun SwitchRow(
    title: String,
    detail: String,
    checked: Boolean,
    enabled: Boolean = true,
    onChange: (Boolean) -> Unit,
) {
    // A Material list item whose whole row toggles its trailing switch.
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(detail) },
        trailingContent = { Switch(checked = checked, onCheckedChange = null, enabled = enabled) },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier
            .toggleable(value = checked, enabled = enabled, role = Role.Switch, onValueChange = onChange)
            .alpha(if (enabled) 1f else 0.38f),
    )
}

@Composable
private fun LapsSection(
    store: TelemetryStore,
    analysis: AnalysisController,
    catalog: PlaybackCatalog,
    config: AnalysisConfig,
    onColor: (ColorTarget) -> Unit,
) {
    val update: ((AnalysisConfig) -> AnalysisConfig) -> Unit = analysis::updateConfig
    SwitchRow(
        "Compare two fixed laps",
        "Choose Lap A and Lap B instead of following the desktop",
        analysis.fixedMode,
        enabled = catalog.active,
        onChange = analysis::useFixedLaps,
    )
    val currentColor = ColorTarget("Current lap colour", config.mapCurrentColor, DEFAULT_MAP_CURRENT_COLOR) { color ->
        update { it.copy(mapCurrentColor = color) }
    }
    val comparisonColor = ColorTarget("Comparison lap colour", config.mapComparisonColor, DEFAULT_MAP_COMPARISON_COLOR) { color ->
        update { it.copy(mapComparisonColor = color) }
    }
    if (analysis.fixedMode) {
        LapGroup("Lap A", currentColor, onColor) {
            LapPicker(analysis.lapA, catalog, allowNone = false, onSelect = analysis::selectLapA)
            LabelField(config.lapALabel, DEFAULT_LAP_A_LABEL) { value -> update { it.copy(lapALabel = value) } }
        }
        LapGroup("Lap B", comparisonColor, onColor) {
            LapPicker(analysis.lapB, catalog, allowNone = false, onSelect = analysis::selectLapB)
            LabelField(config.lapBLabel, DEFAULT_LAP_B_LABEL) { value -> update { it.copy(lapBLabel = value) } }
        }
    } else {
        LapGroup("Current", currentColor, onColor) {
            val lap = store.playbackSelectedLap
            val info = catalog.laps.firstOrNull { it.lapNumber == lap }
            OutlinedTextField(
                value = info?.let { lapOptionLabel(it.lapNumber, it.lapTimeMs, it.lapNumber == catalog.fastestLap) } ?: "—",
                onValueChange = {},
                readOnly = true,
                singleLine = true,
                label = { Text("Lap") },
                supportingText = { Text("Follows the desktop's playback") },
                modifier = Modifier.fillMaxWidth(),
            )
            LabelField(config.currentLabel, DEFAULT_CURRENT_LABEL) { value -> update { it.copy(currentLabel = value) } }
        }
        LapGroup("Compare", comparisonColor, onColor) {
            LapPicker(analysis.compareLap, catalog, allowNone = true, onSelect = analysis::selectCompareLap)
            LabelField(config.compareLabel, DEFAULT_COMPARE_LABEL) { value -> update { it.copy(compareLabel = value) } }
        }
    }
}

@Composable
private fun LapGroup(
    title: String,
    color: ColorTarget,
    onColor: (ColorTarget) -> Unit,
    content: @Composable () -> Unit,
) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp), verticalAlignment = Alignment.Top) {
        Column(Modifier.width(76.dp).padding(top = 10.dp)) {
            Text(title, style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.height(6.dp))
            Swatch(color.color, contentDescription = color.title) { onColor(color) }
        }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) { content() }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LapPicker(
    value: Int,
    catalog: PlaybackCatalog,
    allowNone: Boolean,
    onSelect: (Int) -> Unit,
) {
    // The Material exposed dropdown menu: a read-only field that opens the laps.
    var open by remember { mutableStateOf(false) }
    val selected = catalog.laps.firstOrNull { it.lapNumber == value }
    val enabled = catalog.laps.isNotEmpty()
    ExposedDropdownMenuBox(expanded = open, onExpandedChange = { if (enabled) open = it }) {
        OutlinedTextField(
            value = selected?.let { lapOptionLabel(it.lapNumber, it.lapTimeMs, it.lapNumber == catalog.fastestLap) }
                ?: if (allowNone) "None" else "",
            onValueChange = {},
            readOnly = true,
            enabled = enabled,
            singleLine = true,
            label = { Text("Lap") },
            placeholder = { Text("Select lap") },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = open) },
            modifier = Modifier
                .fillMaxWidth()
                .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable, enabled),
        )
        ExposedDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            if (allowNone) {
                DropdownMenuItem(
                    text = { Text("None") },
                    onClick = {
                        onSelect(0)
                        open = false
                    },
                    contentPadding = ExposedDropdownMenuDefaults.ItemContentPadding,
                )
            }
            for (lap in catalog.laps) {
                val fastest = lap.lapNumber == catalog.fastestLap
                DropdownMenuItem(
                    text = {
                        Text(
                            lapOptionLabel(lap.lapNumber, lap.lapTimeMs, fastest),
                            fontWeight = if (lap.lapNumber == value) FontWeight.SemiBold else FontWeight.Normal,
                        )
                    },
                    // The fastest lap wears the timing screens' purple.
                    leadingIcon = if (fastest) {
                        { Box(Modifier.size(10.dp).background(FastestLapColor, CircleShape)) }
                    } else {
                        null
                    },
                    onClick = {
                        onSelect(lap.lapNumber)
                        open = false
                    },
                    contentPadding = ExposedDropdownMenuDefaults.ItemContentPadding,
                )
            }
        }
    }
}

@Composable
private fun LabelField(value: String, placeholder: String, onChange: (String) -> Unit) {
    OutlinedTextField(
        value = value,
        onValueChange = { onChange(it.take(40)) },
        modifier = Modifier.fillMaxWidth(),
        singleLine = true,
        label = { Text("Name") },
        placeholder = { Text(placeholder) },
    )
}

@Composable
private fun Swatch(
    color: Int,
    contentDescription: String,
    size: Int = 28,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    Box(
        Modifier
            .size(size.dp)
            .alpha(if (enabled) 1f else 0.3f)
            .clip(RoundedCornerShape(6.dp))
            .background(Color(color))
            .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(6.dp))
            .clickable(enabled = enabled, onClickLabel = contentDescription, onClick = onClick),
    )
}

@Composable
private fun SeriesRow(
    item: AnalysisSeries,
    index: Int,
    count: Int,
    deltaSupported: Boolean,
    onMove: (Int) -> Unit,
    onChange: (AnalysisSeries) -> Unit,
    onRemove: () -> Unit,
    onColor: (ColorTarget) -> Unit,
    modifier: Modifier = Modifier,
) {
    val isDelta = item.metricId == AnalysisCatalog.DELTA_ID
    val combined = AnalysisCatalog.combinedById[item.metricId]
    val metric = AnalysisCatalog.metricById[item.metricId]
    val label = AnalysisCatalog.labelOf(item)
    val defaultColor = when {
        isDelta -> DEFAULT_DELTA_POSITIVE_COLOR
        combined != null -> combined.defaultColor
        else -> metric?.defaultColor ?: item.color
    }
    ListItem(
        modifier = modifier,
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        leadingContent = { when {
            isDelta -> Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                Swatch(item.color, "Positive delta colour", size = 22, enabled = deltaSupported) {
                    onColor(ColorTarget("Positive delta colour", item.color, DEFAULT_DELTA_POSITIVE_COLOR) { onChange(item.copy(color = it)) })
                }
                val negative = item.negativeColor ?: DEFAULT_DELTA_NEGATIVE_COLOR
                Swatch(negative, "Negative delta colour", size = 22, enabled = deltaSupported) {
                    onColor(ColorTarget("Negative delta colour", negative, DEFAULT_DELTA_NEGATIVE_COLOR) { onChange(item.copy(negativeColor = it)) })
                }
            }
            else -> Swatch(item.color, "$label colour") {
                onColor(ColorTarget("$label colour", item.color, defaultColor) { onChange(item.copy(color = it)) })
            }
        } },
        headlineContent = {
            Text(
                label,
                color = if (item.visible) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        supportingContent = {
            if (combined != null) {
                // One swatch per corner; corners not on the card stay greyed out.
                Row(Modifier.padding(top = 3.dp), horizontalArrangement = Arrangement.spacedBy(3.dp)) {
                    for (corner in AnalysisCatalog.corners) {
                        val memberId = "${combined.idPrefix}-${corner.key}"
                        val picked = item.corners?.contains(corner.key) ?: true
                        val color = AnalysisCatalog.lineColor(item, memberId)
                        Swatch(color, "${corner.label} colour", size = 16, enabled = picked) {
                            onColor(ColorTarget("$label ${corner.label} colour", color, corner.color) { next ->
                                onChange(item.copy(cornerColors = item.cornerColors + (corner.key to next)))
                            })
                        }
                    }
                }
            } else {
                Text(
                    when {
                        isDelta && !deltaSupported -> "Not supported in this recording"
                        isDelta -> "Time · + / −"
                        metric != null -> listOf(metric.group, metric.unit).filter { it.isNotEmpty() }.joinToString(" · ")
                        else -> ""
                    },
                    maxLines = 1,
                )
            }
        },
        trailingContent = {
            Row {
                SmallIconButton(R.drawable.ic_arrow_up, "Move $label up", enabled = index > 0) { onMove(-1) }
                SmallIconButton(R.drawable.ic_arrow_down, "Move $label down", enabled = index < count - 1) { onMove(1) }
                SmallToggleButton(R.drawable.ic_axis, "$label y axis", item.showYAxis) {
                    onChange(item.copy(showYAxis = it))
                }
                SmallToggleButton(if (item.visible) R.drawable.ic_eye else R.drawable.ic_eye_off, "Show $label", item.visible) {
                    onChange(item.copy(visible = it))
                }
                SmallIconButton(R.drawable.ic_delete, "Remove $label", enabled = !isDelta, onClick = onRemove)
            }
        },
    )
}

/** An on/off action, as a Material icon toggle button. */
@Composable
private fun SmallToggleButton(icon: Int, description: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    IconToggleButton(checked = checked, onCheckedChange = onChange, modifier = Modifier.size(36.dp)) {
        Icon(painterResource(icon), contentDescription = description, modifier = Modifier.size(18.dp))
    }
}

@Composable
private fun SmallIconButton(
    icon: Int,
    description: String,
    enabled: Boolean = true,
    dimmed: Boolean = false,
    onClick: () -> Unit,
) {
    IconButton(onClick = onClick, enabled = enabled, modifier = Modifier.size(36.dp)) {
        Icon(
            painterResource(icon),
            contentDescription = description,
            modifier = Modifier.size(18.dp).alpha(if (dimmed) 0.4f else 1f),
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun MetricPicker(
    series: List<AnalysisSeries>,
    updateSeries: ((List<AnalysisSeries>) -> List<AnalysisSeries>) -> Unit,
) {
    var query by remember { mutableStateOf("") }
    val selected = series.map { it.metricId }.toSet()
    val tokens = query.trim().lowercase().split(Regex("\\s+")).filter { it.isNotEmpty() }
    fun matches(haystack: String) = tokens.all { haystack.lowercase().contains(it) }
    val cornerTerms = AnalysisCatalog.corners.joinToString(" ") { it.label }

    Surface(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            OutlinedTextField(
                value = query,
                onValueChange = { query = it },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                placeholder = { Text("Filter metrics") },
            )
            var anything = false
            for (category in AnalysisCatalog.categories) {
                val metrics = AnalysisCatalog.metrics.filter {
                    it.group == category && it.id !in AnalysisCatalog.cornerMetricIds &&
                        matches("$category ${it.label} ${it.unit}")
                }
                val tyreRows = if (category == "Tyres") {
                    AnalysisCatalog.tyreRows.filter { matches("tyres tires ${it.label} all com combined $cornerTerms") }
                } else {
                    emptyList()
                }
                if (tyreRows.isNotEmpty()) {
                    anything = true
                    PickerHeading("Tyres by corner")
                    TyreMatrix(tyreRows, series, updateSeries)
                }
                if (metrics.isEmpty()) continue
                anything = true
                PickerHeading(if (category == "Tyres") "Tyre averages" else category)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (metric in metrics) {
                        val on = metric.id in selected
                        FilterChip(
                            selected = on,
                            onClick = { updateSeries { it.withMetrics(listOf(metric.id), !on) } },
                            label = { Text(metric.label) },
                        )
                    }
                }
            }
            if (!anything) {
                Text(
                    "No metrics match “${query.trim()}”",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun PickerHeading(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.labelMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

/**
 * Per-corner tyre metrics: FL FR RL RR pick corners, ALL picks or clears all
 * four, and COM switches the row between a card per corner and one combined card.
 */
@Composable
private fun TyreMatrix(
    rows: List<AnalysisTyreRow>,
    series: List<AnalysisSeries>,
    updateSeries: ((List<AnalysisSeries>) -> List<AnalysisSeries>) -> Unit,
) {
    val columns = listOf("COM") + AnalysisCatalog.corners.map { it.label } + "ALL"
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Spacer(Modifier.width(96.dp))
            for (column in columns) {
                Text(
                    column,
                    modifier = Modifier.width(36.dp),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                )
            }
        }
        for (row in rows) {
            val combined = series.firstOrNull { it.metricId == "${row.idPrefix}-all" }
            val picked = AnalysisCatalog.corners.map { corner ->
                combined?.corners?.contains(corner.key) ?: series.any { it.metricId == "${row.idPrefix}-${corner.key}" }
            }
            val allPicked = picked.all { it }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    row.label,
                    modifier = Modifier.width(96.dp),
                    style = MaterialTheme.typography.bodySmall,
                    color = if (picked.any { it }) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                )
                MatrixCell(combined != null, "${row.label} combined") {
                    updateSeries { it.withTyreCombinedToggled(row.idPrefix) }
                }
                AnalysisCatalog.corners.forEachIndexed { index, corner ->
                    MatrixCell(picked[index], "${row.label} ${corner.label}") {
                        updateSeries { it.withTyreCornerToggled(row.idPrefix, corner.key) }
                    }
                }
                MatrixCell(allPicked, "All ${row.label} corners") {
                    updateSeries { it.withTyreAllCornersToggled(row.idPrefix) }
                }
            }
        }
    }
}

@Composable
private fun MatrixCell(on: Boolean, description: String, onClick: () -> Unit) {
    val colors = MaterialTheme.colorScheme
    Box(Modifier.width(36.dp).height(32.dp), contentAlignment = Alignment.Center) {
        Box(
            Modifier
                .size(width = 26.dp, height = 22.dp)
                .clip(RoundedCornerShape(5.dp))
                .background(if (on) colors.primary else Color.Transparent)
                .border(BorderStroke(1.dp, if (on) colors.primary else colors.outline), RoundedCornerShape(5.dp))
                .clickable(onClickLabel = description, onClick = onClick),
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ColorDialog(target: ColorTarget, onPick: (Int) -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(target.title) },
        text = {
            FlowRow(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                for (color in Palette) {
                    Box(
                        Modifier
                            .size(36.dp)
                            .clip(RoundedCornerShape(8.dp))
                            .background(Color(color))
                            .border(
                                if (color == target.color) 3.dp else 1.dp,
                                if (color == target.color) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.outline,
                                RoundedCornerShape(8.dp),
                            )
                            .clickable { onPick(color) },
                    )
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } },
        dismissButton = { TextButton(onClick = { onPick(target.default) }) { Text("Default") } },
    )
}

// ── Series edits, ported from the desktop's Analysis sidebar ───────────────

private fun List<AnalysisSeries>.moved(metricId: String, delta: Int): List<AnalysisSeries> {
    val from = indexOfFirst { it.metricId == metricId }
    val to = from + delta
    if (from < 0 || to < 0 || to >= size) return this
    return toMutableList().apply { add(to, removeAt(from)) }
}

/** Adds or removes metrics; a combined card and its own corners displace each other. */
private fun List<AnalysisSeries>.withMetrics(metricIds: List<String>, selected: Boolean): List<AnalysisSeries> {
    if (!selected) {
        val removed = metricIds.filter { it != AnalysisCatalog.DELTA_ID }.toSet()
        return filter { it.metricId !in removed }
    }
    val charted = map { it.metricId }.toMutableSet()
    val displaced = mutableSetOf<String>()
    val added = metricIds.mapNotNull { id ->
        val combined = AnalysisCatalog.combinedById[id]
        val metric = AnalysisCatalog.metricById[id]
        if ((combined == null && metric == null) || id in charted) return@mapNotNull null
        charted += id
        displaced += AnalysisCatalog.conflicts(id)
        if (combined != null) {
            AnalysisSeries(id, combined.defaultColor, corners = AnalysisCatalog.sanitizeCorners(null))
        } else {
            AnalysisSeries(id, metric!!.defaultColor)
        }
    }
    if (added.isEmpty()) return this
    return filter { it.metricId !in displaced } + added
}

private fun List<AnalysisSeries>.withTyreCornerToggled(idPrefix: String, cornerKey: String): List<AnalysisSeries> {
    val combined = firstOrNull { it.metricId == "$idPrefix-all" }
    if (combined == null) {
        val id = "$idPrefix-$cornerKey"
        return withMetrics(listOf(id), none { it.metricId == id })
    }
    val corners = combined.corners ?: emptyList()
    val next = if (cornerKey in corners) corners - cornerKey else AnalysisCatalog.sanitizeCorners(corners + cornerKey)
    return map { if (it === combined) it.copy(corners = next) else it }
}

private fun List<AnalysisSeries>.withTyreAllCornersToggled(idPrefix: String): List<AnalysisSeries> {
    val combined = firstOrNull { it.metricId == "$idPrefix-all" }
    if (combined != null) {
        val all = AnalysisCatalog.sanitizeCorners(combined.corners ?: emptyList()).size == AnalysisCatalog.corners.size
        return map { if (it === combined) it.copy(corners = if (all) emptyList() else AnalysisCatalog.sanitizeCorners(null)) else it }
    }
    val ids = AnalysisCatalog.corners.map { "$idPrefix-${it.key}" }
    val charted = map { it.metricId }.toSet()
    return withMetrics(ids, !ids.all { it in charted })
}

/** Combining merges a row's charted corners into one card; separating splits it back. */
private fun List<AnalysisSeries>.withTyreCombinedToggled(idPrefix: String): List<AnalysisSeries> {
    val combinedId = "$idPrefix-all"
    val combined = firstOrNull { it.metricId == combinedId }
    if (combined != null) {
        val split = (combined.corners ?: emptyList()).mapNotNull { key ->
            val metric = AnalysisCatalog.metricById["$idPrefix-$key"] ?: return@mapNotNull null
            AnalysisSeries(
                metric.id,
                AnalysisCatalog.lineColor(combined, metric.id),
                visible = combined.visible,
                showYAxis = combined.showYAxis,
            )
        }
        return flatMap { if (it === combined) split else listOf(it) }
    }
    val definition = AnalysisCatalog.combinedById.getValue(combinedId)
    val memberIds = definition.memberIds.toSet()
    val members = filter { it.metricId in memberIds }
    val merged = AnalysisSeries(
        metricId = combinedId,
        color = definition.defaultColor,
        visible = members.isEmpty() || members.any { it.visible },
        showYAxis = members.isEmpty() || members.any { it.showYAxis },
        corners = AnalysisCatalog.sanitizeCorners(members.map { it.metricId.substring(idPrefix.length + 1) }),
        cornerColors = members.mapNotNull { member ->
            val metric = AnalysisCatalog.metricById[member.metricId] ?: return@mapNotNull null
            if (member.color != metric.defaultColor) member.metricId.substring(idPrefix.length + 1) to member.color else null
        }.toMap(),
    )
    val insertAt = indexOfFirst { it.metricId in memberIds }
    val rest = filter { it.metricId !in memberIds }.toMutableList()
    rest.add(if (insertAt < 0) rest.size else insertAt.coerceAtMost(rest.size), merged)
    return rest
}
