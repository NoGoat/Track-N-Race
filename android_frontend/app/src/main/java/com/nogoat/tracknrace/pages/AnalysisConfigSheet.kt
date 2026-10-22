package com.nogoat.tracknrace.pages

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.PrimaryTabRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.zIndex
import com.nogoat.tracknrace.AnalysisCatalog
import com.nogoat.tracknrace.AnalysisConfig
import com.nogoat.tracknrace.AnalysisController
import com.nogoat.tracknrace.AnalysisSeries
import com.nogoat.tracknrace.AnalysisTyreRow
import com.nogoat.tracknrace.DEFAULT_DELTA_NEGATIVE_COLOR
import com.nogoat.tracknrace.DEFAULT_DELTA_POSITIVE_COLOR
import com.nogoat.tracknrace.PlaybackCatalog
import com.nogoat.tracknrace.R
import kotlin.math.roundToInt

/** A colour being picked, and what it resets to. */
private class ColorTarget(
    val title: String,
    val color: Int,
    val default: Int,
    val apply: (Int) -> Unit,
)

/** Chart settings: the charted metrics, and how the graph is drawn. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun AnalysisConfigSheet(
    analysis: AnalysisController,
    catalog: PlaybackCatalog,
    onDismiss: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val config = analysis.config
    var tab by rememberSaveable { mutableIntStateOf(0) }
    var pickerOpen by remember { mutableStateOf(false) }
    var colorTarget by remember { mutableStateOf<ColorTarget?>(null) }
    val update: ((AnalysisConfig) -> AnalysisConfig) -> Unit = analysis::updateConfig
    fun updateSeries(transform: (List<AnalysisSeries>) -> List<AnalysisSeries>) =
        update { it.copy(series = transform(it.series)) }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        sheetGesturesEnabled = false,
        dragHandle = null,
    ) {
        Text(
            "Chart",
            modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 24.dp, bottom = 8.dp),
            style = MaterialTheme.typography.titleLarge,
        )
        PrimaryTabRow(selectedTabIndex = tab, containerColor = Color.Transparent) {
            Tab(selected = tab == 0, onClick = { tab = 0 }, text = { Text("Metrics") })
            Tab(selected = tab == 1, onClick = { tab = 1 }, text = { Text("Display") })
        }
        Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
            if (tab == 0) {
                FilledTonalButton(
                    onClick = { pickerOpen = true },
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
                ) {
                    Icon(painterResource(R.drawable.ic_add), contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(8.dp))
                    Text("Add metric")
                }
                if (config.series.isEmpty()) {
                    Text(
                        "No metrics selected",
                        modifier = Modifier.fillMaxWidth().padding(24.dp),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        style = MaterialTheme.typography.bodyMedium,
                        textAlign = TextAlign.Center,
                    )
                }
                ReorderableSeries(
                    series = config.series,
                    deltaSupported = catalog.deltaAvailable || !catalog.active,
                    onMove = { id, delta -> updateSeries { it.moved(id, delta) } },
                    onChange = { next -> updateSeries { series -> series.map { if (it.metricId == next.metricId) next else it } } },
                    onRemove = { id -> updateSeries { series -> series.filter { it.metricId != id } } },
                    onColor = { colorTarget = it },
                )
            } else {
                SwitchRow("Individual graphs", "One panel per metric", config.individualGraphs) { value ->
                    update { it.copy(individualGraphs = value) }
                }
                SwitchRow("Sector boundaries", "Sectors on the x axis", config.sectorBoundaries) { value ->
                    update { it.copy(sectorBoundaries = value) }
                }
                SwitchRow("Sector delta", "Resets each sector", config.sectorDelta, enabled = config.sectorBoundaries) { value ->
                    update { it.copy(sectorDelta = value) }
                }
                val allAxes = config.series.isNotEmpty() && config.series.all { it.showYAxis }
                SwitchRow("Y axes", null, allAxes) { value ->
                    updateSeries { series -> series.map { it.copy(showYAxis = value) } }
                }
            }
        }
    }

    if (pickerOpen) {
        MetricSearch(config.series, ::updateSeries, onClose = { pickerOpen = false })
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
private fun SwitchRow(
    title: String,
    detail: String?,
    checked: Boolean,
    enabled: Boolean = true,
    onChange: (Boolean) -> Unit,
) {
    // A Material list item whose whole row toggles its trailing switch.
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = detail?.let { { Text(it) } },
        trailingContent = { Switch(checked = checked, onCheckedChange = null, enabled = enabled) },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier
            .toggleable(value = checked, enabled = enabled, role = Role.Switch, onValueChange = onChange)
            .alpha(if (enabled) 1f else 0.38f),
    )
}

/**
 * The charted metrics, reordered by dragging their handles. The dragged row
 * follows the finger and swaps with a neighbour once it passes the
 * neighbour's middle.
 */
@Composable
private fun ReorderableSeries(
    series: List<AnalysisSeries>,
    deltaSupported: Boolean,
    onMove: (String, Int) -> Unit,
    onChange: (AnalysisSeries) -> Unit,
    onRemove: (String) -> Unit,
    onColor: (ColorTarget) -> Unit,
) {
    val heights = remember { mutableStateMapOf<String, Int>() }
    var dragged by remember { mutableStateOf<String?>(null) }
    var offset by remember { mutableFloatStateOf(0f) }
    val latestSeries by rememberUpdatedState(series)
    val latestMove by rememberUpdatedState(onMove)

    fun dragBy(amount: Float) {
        val id = dragged ?: return
        offset += amount
        val list = latestSeries
        val index = list.indexOfFirst { it.metricId == id }
        if (index < 0) return
        val next = list.getOrNull(index + 1)
        val previous = list.getOrNull(index - 1)
        if (offset > 0 && next != null) {
            val height = heights[next.metricId] ?: return
            if (offset > height / 2f) {
                latestMove(id, 1)
                offset -= height
            }
        } else if (offset < 0 && previous != null) {
            val height = heights[previous.metricId] ?: return
            if (-offset > height / 2f) {
                latestMove(id, -1)
                offset += height
            }
        }
    }

    Column {
        series.forEachIndexed { index, item ->
            key(item.metricId) {
                val isDragged = dragged == item.metricId
                SeriesRow(
                    item = item,
                    index = index,
                    count = series.size,
                    deltaSupported = deltaSupported,
                    dragged = isDragged,
                    handle = Modifier.pointerInput(item.metricId) {
                        detectDragGestures(
                            onDragStart = {
                                dragged = item.metricId
                                offset = 0f
                            },
                            onDragEnd = { dragged = null; offset = 0f },
                            onDragCancel = { dragged = null; offset = 0f },
                            onDrag = { change, amount ->
                                change.consume()
                                dragBy(amount.y)
                            },
                        )
                    },
                    onMove = { delta -> onMove(item.metricId, delta) },
                    onChange = onChange,
                    onRemove = { onRemove(item.metricId) },
                    onColor = onColor,
                    modifier = Modifier
                        .onSizeChanged { heights[item.metricId] = it.height }
                        .zIndex(if (isDragged) 1f else 0f)
                        .graphicsLayer {
                            translationY = if (isDragged) offset else 0f
                            shadowElevation = if (isDragged) 6.dp.toPx() else 0f
                        },
                )
            }
        }
    }
}

@Composable
private fun Swatch(
    color: Int,
    contentDescription: String?,
    size: Int = 24,
    enabled: Boolean = true,
    onClick: (() -> Unit)? = null,
) {
    // A 48 dp touch target around the swatch when it can be tapped.
    Box(
        Modifier
            .size(if (onClick != null) 48.dp else size.dp)
            .then(if (onClick != null) Modifier.clip(RoundedCornerShape(12.dp)).clickable(enabled = enabled, onClickLabel = contentDescription, onClick = onClick) else Modifier),
        contentAlignment = Alignment.Center,
    ) {
        Box(
            Modifier
                .size(size.dp)
                .alpha(if (enabled) 1f else 0.3f)
                .clip(RoundedCornerShape(6.dp))
                .background(Color(color))
                .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(6.dp)),
        )
    }
}

/** A swatch split into the given colours: the delta's two, or a tyre card's corners. */
@Composable
private fun MultiSwatch(colors: List<Int>, enabled: List<Boolean> = colors.map { true }) {
    Box(Modifier.size(48.dp), contentAlignment = Alignment.Center) {
        Box(
            Modifier
                .size(24.dp)
                .clip(RoundedCornerShape(6.dp))
                .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(6.dp)),
        ) {
            if (colors.size == 2) {
                Row(Modifier.fillMaxSize()) {
                    colors.forEach { Box(Modifier.weight(1f).fillMaxSize().background(Color(it))) }
                }
            } else {
                Column(Modifier.fillMaxSize()) {
                    for (row in colors.indices.chunked(2)) {
                        Row(Modifier.weight(1f).fillMaxWidth()) {
                            for (index in row) {
                                Box(Modifier.weight(1f).fillMaxSize().alpha(if (enabled[index]) 1f else 0.2f).background(Color(colors[index])))
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SeriesRow(
    item: AnalysisSeries,
    index: Int,
    count: Int,
    deltaSupported: Boolean,
    dragged: Boolean,
    handle: Modifier,
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
    val defaultColor = combined?.defaultColor ?: metric?.defaultColor ?: item.color
    val negative = item.negativeColor ?: DEFAULT_DELTA_NEGATIVE_COLOR
    var menuOpen by remember { mutableStateOf(false) }
    Surface(
        modifier = modifier,
        color = if (dragged) MaterialTheme.colorScheme.surfaceContainerHighest else Color.Transparent,
    ) {
        Row(
            Modifier.fillMaxWidth().height(64.dp).padding(end = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(handle.size(48.dp), contentAlignment = Alignment.Center) {
                Icon(
                    painterResource(R.drawable.ic_drag_handle),
                    contentDescription = "Reorder $label",
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            when {
                isDelta -> MultiSwatch(listOf(item.color, negative))
                combined != null -> MultiSwatch(
                    AnalysisCatalog.corners.map { AnalysisCatalog.lineColor(item, "${combined.idPrefix}-${it.key}") },
                    AnalysisCatalog.corners.map { item.corners?.contains(it.key) ?: true },
                )
                else -> Swatch(item.color, "$label colour") {
                    onColor(ColorTarget("$label colour", item.color, defaultColor) { onChange(item.copy(color = it)) })
                }
            }
            Column(Modifier.weight(1f).padding(start = 4.dp)) {
                Text(
                    label,
                    style = MaterialTheme.typography.bodyLarge,
                    color = if (item.visible) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    when {
                        isDelta && !deltaSupported -> "Not in this recording"
                        isDelta -> "s · slower / faster"
                        combined != null -> listOf(combined.unit) .plus(
                            AnalysisCatalog.corners.filter { item.corners?.contains(it.key) ?: true }.map { it.label },
                        ).joinToString(" · ")
                        else -> metric?.unit.orEmpty()
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                )
            }
            Switch(
                checked = item.visible,
                onCheckedChange = { onChange(item.copy(visible = it)) },
                enabled = !isDelta || deltaSupported,
            )
            Box {
                IconButton(onClick = { menuOpen = true }) {
                    Icon(painterResource(R.drawable.ic_more_vert), contentDescription = "$label options")
                }
                DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                    fun close(action: () -> Unit): () -> Unit = {
                        menuOpen = false
                        action()
                    }
                    DropdownMenuItem(
                        text = { Text("Y axis") },
                        leadingIcon = { Icon(painterResource(R.drawable.ic_axis), contentDescription = null) },
                        trailingIcon = if (item.showYAxis) {
                            { Icon(painterResource(R.drawable.ic_check), contentDescription = "Shown") }
                        } else {
                            null
                        },
                        onClick = close { onChange(item.copy(showYAxis = !item.showYAxis)) },
                    )
                    if (isDelta) {
                        DropdownMenuItem(
                            text = { Text("Slower colour") },
                            leadingIcon = { MenuSwatch(item.color) },
                            onClick = close {
                                onColor(ColorTarget("Slower colour", item.color, DEFAULT_DELTA_POSITIVE_COLOR) { onChange(item.copy(color = it)) })
                            },
                        )
                        DropdownMenuItem(
                            text = { Text("Faster colour") },
                            leadingIcon = { MenuSwatch(negative) },
                            onClick = close {
                                onColor(ColorTarget("Faster colour", negative, DEFAULT_DELTA_NEGATIVE_COLOR) { onChange(item.copy(negativeColor = it)) })
                            },
                        )
                    } else if (combined != null) {
                        for (corner in AnalysisCatalog.corners) {
                            val memberId = "${combined.idPrefix}-${corner.key}"
                            val color = AnalysisCatalog.lineColor(item, memberId)
                            DropdownMenuItem(
                                text = { Text("${corner.label} colour") },
                                leadingIcon = { MenuSwatch(color) },
                                onClick = close {
                                    onColor(ColorTarget("$label ${corner.label} colour", color, corner.color) { next ->
                                        onChange(item.copy(cornerColors = item.cornerColors + (corner.key to next)))
                                    })
                                },
                            )
                        }
                    }
                    // Dragging is not available to every input method.
                    if (index > 0) {
                        DropdownMenuItem(
                            text = { Text("Move up") },
                            leadingIcon = { Icon(painterResource(R.drawable.ic_arrow_up), contentDescription = null) },
                            onClick = close { onMove(-1) },
                        )
                    }
                    if (index < count - 1) {
                        DropdownMenuItem(
                            text = { Text("Move down") },
                            leadingIcon = { Icon(painterResource(R.drawable.ic_arrow_down), contentDescription = null) },
                            onClick = close { onMove(1) },
                        )
                    }
                    if (!isDelta) {
                        DropdownMenuItem(
                            text = { Text("Remove") },
                            leadingIcon = { Icon(painterResource(R.drawable.ic_delete), contentDescription = null) },
                            onClick = close(onRemove),
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun MenuSwatch(color: Int) {
    Box(
        Modifier
            .size(20.dp)
            .clip(RoundedCornerShape(5.dp))
            .background(Color(color))
            .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(5.dp)),
    )
}

/** Full-screen metric search: a filter field over the catalogue's chips and tyre grid. */
@OptIn(ExperimentalLayoutApi::class, ExperimentalMaterial3Api::class)
@Composable
private fun MetricSearch(
    series: List<AnalysisSeries>,
    updateSeries: ((List<AnalysisSeries>) -> List<AnalysisSeries>) -> Unit,
    onClose: () -> Unit,
) {
    var query by remember { mutableStateOf("") }
    val selected = series.map { it.metricId }.toSet()
    val tokens = query.trim().lowercase().split(Regex("\\s+")).filter { it.isNotEmpty() }
    fun matches(haystack: String) = tokens.all { haystack.lowercase().contains(it) }
    val cornerTerms = AnalysisCatalog.corners.joinToString(" ") { it.label }

    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
            Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().imePadding()) {
                Row(Modifier.fillMaxWidth().padding(start = 4.dp, end = 8.dp, top = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                    IconButton(onClick = onClose) {
                        Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Done")
                    }
                    TextField(
                        value = query,
                        onValueChange = { query = it },
                        modifier = Modifier.weight(1f),
                        singleLine = true,
                        placeholder = { Text("Search metrics") },
                        leadingIcon = { Icon(painterResource(R.drawable.ic_search), contentDescription = null) },
                        trailingIcon = if (query.isNotEmpty()) {
                            { IconButton(onClick = { query = "" }) { Icon(painterResource(R.drawable.ic_close), contentDescription = "Clear") } }
                        } else {
                            null
                        },
                        shape = RoundedCornerShape(28.dp),
                        colors = TextFieldDefaults.colors(
                            focusedIndicatorColor = Color.Transparent,
                            unfocusedIndicatorColor = Color.Transparent,
                        ),
                    )
                }
                Column(
                    Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 12.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
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
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            for (metric in metrics) {
                                val on = metric.id in selected
                                FilterChip(
                                    selected = on,
                                    onClick = { updateSeries { it.withMetrics(listOf(metric.id), !on) } },
                                    label = { Text(metric.label) },
                                    leadingIcon = if (on) {
                                        { Icon(painterResource(R.drawable.ic_check), contentDescription = null, modifier = Modifier.size(18.dp)) }
                                    } else {
                                        null
                                    },
                                )
                            }
                        }
                    }
                    if (!anything) {
                        Text(
                            "No metrics match “${query.trim()}”",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun PickerHeading(text: String) {
    Text(
        text,
        modifier = Modifier.padding(top = 6.dp),
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
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
    Column {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Spacer(Modifier.weight(1f))
            for (column in columns) {
                Text(
                    column,
                    modifier = Modifier.width(42.dp),
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
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (picked.any { it }) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
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
    // A 42 × 48 dp target around a 28 dp box.
    Box(
        Modifier
            .width(42.dp)
            .height(48.dp)
            .clickable(onClickLabel = description, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Box(
            Modifier
                .size(28.dp)
                .clip(RoundedCornerShape(6.dp))
                .background(if (on) colors.primary else Color.Transparent)
                .border(BorderStroke(1.5.dp, if (on) colors.primary else colors.outline), RoundedCornerShape(6.dp)),
            contentAlignment = Alignment.Center,
        ) {
            if (on) Icon(painterResource(R.drawable.ic_check), contentDescription = null, tint = colors.onPrimary, modifier = Modifier.size(18.dp))
        }
    }
}

@Composable
private fun ColorDialog(target: ColorTarget, onPick: (Int) -> Unit, onDismiss: () -> Unit) =
    ColorPickerDialog(target.title, target.color, target.default, onPick, onDismiss)

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
