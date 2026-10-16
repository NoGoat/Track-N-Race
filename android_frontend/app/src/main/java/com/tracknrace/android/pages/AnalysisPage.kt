package com.tracknrace.android.pages

import android.content.res.Configuration
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ElevatedAssistChip
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.PrimaryTabRow
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.material3.VerticalDivider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.tracknrace.android.AnalysisCatalog
import com.tracknrace.android.AnalysisConfig
import com.tracknrace.android.AnalysisController
import com.tracknrace.android.AnalysisDeltaCurve
import com.tracknrace.android.AnalysisDeltaSummary
import com.tracknrace.android.AnalysisLap
import com.tracknrace.android.AnalysisView
import com.tracknrace.android.DEFAULT_COMPARE_LABEL
import com.tracknrace.android.DEFAULT_CURRENT_LABEL
import com.tracknrace.android.DEFAULT_DELTA_NEGATIVE_COLOR
import com.tracknrace.android.DEFAULT_DELTA_POSITIVE_COLOR
import com.tracknrace.android.DEFAULT_LAP_A_LABEL
import com.tracknrace.android.DEFAULT_LAP_B_LABEL
import com.tracknrace.android.PairedTelemetryClient
import com.tracknrace.android.PlaybackCatalog
import com.tracknrace.android.R
import com.tracknrace.android.TelemetryStore
import com.tracknrace.android.formatDelta
import com.tracknrace.android.formatLapTimeMs
import com.tracknrace.android.summarizeDelta
import java.util.Locale
import kotlin.math.max
import kotlinx.coroutines.delay

private const val LAP_SYNC_INTERVAL_MS = 1_000L
private const val DELTA_READOUT_INTERVAL_MS = 100L
private const val CLOCK_FRAME_NANOS = 30_000_000L

/**
 * Lap analysis of the recording the desktop is playing: Graph, Split and Map
 * tabs over a lap bar that names the compared laps and their delta. The
 * current lap follows the desktop's cursor unless two fixed laps are chosen.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun AnalysisScreen(
    store: TelemetryStore,
    analysis: AnalysisController,
    active: Boolean,
) {
    val paired = store.settings.source == PairedTelemetryClient.SOURCE_PAIRED
    // Read through derived state: the rest of the cold state (lap clock, ERS,
    // fuel) changes several times a second and would recompose this screen.
    val trackId by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.cold.trackId } }
    val aeroMode by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.cold.aeroMode } }
    val catalog = store.playbackCatalog
    val config = analysis.config
    val fixed = analysis.fixedMode
    val currentLapNumber = if (fixed) analysis.lapA else store.playbackSelectedLap
    val comparisonLapNumber = if (fixed) analysis.lapB else analysis.compareLap
    val channels = remember(config.series) { requiredChannels(config) }
    val sectorDelta = config.sectorBoundaries && config.sectorDelta
    val distanceMode = catalog.deltaAvailable

    // Fetch the shown laps, and ask again on a slow tick if a reply went missing.
    LaunchedEffect(active, catalog, currentLapNumber, comparisonLapNumber, channels, sectorDelta) {
        if (!active) return@LaunchedEffect
        while (true) {
            analysis.sync(catalog, listOf(currentLapNumber, comparisonLapNumber), channels)
            analysis.syncDelta(catalog, currentLapNumber, comparisonLapNumber, sectorDelta)
            delay(LAP_SYNC_INTERVAL_MS)
        }
    }

    val current = analysis.laps[currentLapNumber]
    val comparison = analysis.laps[comparisonLapNumber]
    val comparisonSelected = currentLapNumber > 0 && comparisonLapNumber > 0
    val deltaSeries = config.deltaSeries
    val showDelta = deltaSeries?.visible == true && distanceMode && comparisonSelected
    val delta = analysis.deltaCurve?.takeIf {
        it.currentLap == currentLapNumber && it.comparisonLap == comparisonLapNumber && it.sectorDelta == sectorDelta
    }
    val primaryLabel = if (fixed) config.lapALabel.ifBlank { DEFAULT_LAP_A_LABEL } else config.currentLabel.ifBlank { DEFAULT_CURRENT_LABEL }
    val comparisonLabel = if (fixed) config.lapBLabel.ifBlank { DEFAULT_LAP_B_LABEL } else config.compareLabel.ifBlank { DEFAULT_COMPARE_LABEL }
    val currentColor = Color(config.mapCurrentColor)
    val comparisonColor = Color(config.mapComparisonColor)
    val positiveColor = Color(deltaSeries?.color ?: DEFAULT_DELTA_POSITIVE_COLOR)
    val negativeColor = Color(deltaSeries?.negativeColor ?: DEFAULT_DELTA_NEGATIVE_COLOR)

    // Per-frame values: the desktop cursor within the current lap, and the map clock.
    val clock = remember { AnalysisMapClock() }
    val cursorOffset = remember { mutableDoubleStateOf(Double.NaN) }
    val mapElapsed = remember { mutableDoubleStateOf(0.0) }
    var mapFocus by remember { mutableStateOf<Double?>(null) }
    val mapTotal = max(current?.duration ?: 0.0, comparison?.duration ?: 0.0)
    LaunchedEffect(mapTotal) { clock.setTotal(mapTotal) }
    LaunchedEffect(fixed, currentLapNumber, comparisonLapNumber) {
        clock.reset()
        mapFocus = null
    }
    LaunchedEffect(active, fixed, current) {
        if (!active) return@LaunchedEffect
        var lastUpdate = 0L
        while (true) {
            withFrameNanos { now ->
                // 30 Hz is smooth for a marker moving a pixel a frame, and on a
                // low-end phone replaying the whole window costs ~9 ms of
                // render-thread time per frame, so skip every other vsync.
                if (now - lastUpdate < CLOCK_FRAME_NANOS) return@withFrameNanos
                lastUpdate = now
                val cursor = store.latestPlaybackCursorTime()
                val offset = if (!fixed && current != null && cursor.isFinite()) cursor - current.startSessionTime else Double.NaN
                if (offset != cursorOffset.doubleValue && !(offset.isNaN() && cursorOffset.doubleValue.isNaN())) {
                    cursorOffset.doubleValue = offset
                }
                val focus = mapFocus
                val elapsed = when {
                    fixed -> clock.read(now)
                    focus != null -> focus
                    offset.isFinite() -> offset
                    else -> 0.0
                }
                if (elapsed != mapElapsed.doubleValue) mapElapsed.doubleValue = elapsed
            }
        }
    }

    val visibleDistance = rememberVisibleDistance(!fixed, current, cursorOffset)
    val summary = remember(delta, current, comparison) {
        derivedStateOf(structuralEqualityPolicy()) { summarizeDelta(delta, current, comparison, visibleDistance.value) }
    }

    val chartState = remember { AnalysisChartState() }
    var lapPicker by remember { mutableStateOf<LapTarget?>(null) }
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val onView: (AnalysisView) -> Unit = { view ->
        // Choosing a view drops a point opened from the graph, as on the desktop.
        mapFocus = null
        analysis.updateConfig { it.copy(view = view) }
    }
    val lapBar: @Composable (Modifier) -> Unit = { modifier ->
        LapBar(
            catalog = catalog,
            fixed = fixed,
            primaryLap = currentLapNumber,
            primaryColor = currentColor,
            comparisonLap = comparisonLapNumber,
            comparisonColor = comparisonColor,
            lapDelta = if (comparisonSelected && delta != null) summary else null,
            positiveColor = positiveColor,
            negativeColor = negativeColor,
            onPick = { lapPicker = it },
            modifier = modifier,
        )
    }

    Column(Modifier.fillMaxSize()) {
        if (landscape) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                lapBar(Modifier.weight(1f))
                ViewTabs(config.view, onView, Modifier.width(320.dp))
            }
        } else {
            ViewTabs(config.view, onView, Modifier.fillMaxWidth())
            lapBar(Modifier.fillMaxWidth())
        }
        // Without sector boundaries the x axis has no room for sector deltas.
        if (!config.sectorBoundaries && comparisonSelected && delta != null && config.view != AnalysisView.MAP) {
            SectorDeltaLine(summary, positiveColor, negativeColor)
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)

        val empty = when {
            !paired -> EmptyState(
                "No paired desktop",
                "Analysis reads the recording playing on your desktop. Pair it and choose Paired mode in Settings.",
            )
            !catalog.active -> EmptyState("No recording playing", "Open a recording on the desktop to analyse its laps.")
            analysis.lapDataUnavailable -> EmptyState(
                "Desktop update needed",
                "This desktop cannot send laps yet. Update Track N Race on the desktop.",
            )
            currentLapNumber <= 0 && fixed -> EmptyState(
                "Choose two laps",
                "Pick Lap A and Lap B to compare.",
                action = "Choose laps",
            )
            currentLapNumber <= 0 -> EmptyState("Waiting for a lap", "The desktop's playback has not reached a lap yet.", loading = true)
            current == null -> EmptyState("Loading lap $currentLapNumber", "Fetching it from the desktop.", loading = true)
            else -> null
        }
        Box(Modifier.fillMaxWidth().weight(1f)) {
            if (empty != null) {
                AnalysisEmptyState(empty, onAction = { lapPicker = LapTarget.LAP_A }, modifier = Modifier.align(Alignment.Center))
                return@Box
            }
            val chartInput = AnalysisChartInput(
                current = current,
                comparison = comparison,
                series = config.series,
                distanceMode = distanceMode,
                trackLengthM = catalog.trackLengthM,
                delta = delta,
                showDelta = showDelta,
                stacked = config.individualGraphs,
                sectorBoundaries = config.sectorBoundaries,
                realtime = !fixed,
                zoomEnabled = fixed,
                deltaPositive = positiveColor,
                deltaNegative = negativeColor,
                mapCursorColors = if (config.view == AnalysisView.SPLIT && fixed) listOf(currentColor, comparisonColor) else null,
            )
            val mapLaps = listOfNotNull(
                current?.let { MapLap(it, currentColor, primaryLabel) },
                comparison?.let { MapLap(it, comparisonColor, comparisonLabel) },
            )
            // Reading the graph moves the cars on the map: the fixed laps'
            // own clock, or a held point instead of the desktop's cursor.
            val scrub: (Double) -> Unit = { elapsed ->
                if (fixed) clock.seek(elapsed) else mapFocus = elapsed
            }
            val backToLive = !fixed && (mapFocus != null || chartState.crosshair != null)
            val chips: @Composable (Modifier) -> Unit = { modifier ->
                ChartChips(
                    zoomed = chartState.zoomed,
                    onWholeLap = chartState::resetZoom,
                    backToLive = backToLive,
                    onBackToLive = {
                        mapFocus = null
                        chartState.crosshair = null
                    },
                    modifier = modifier,
                )
            }
            val chart: @Composable (Modifier) -> Unit = { modifier ->
                Box(modifier) {
                    AnalysisChart(
                        input = chartInput,
                        state = chartState,
                        cursorOffset = cursorOffset,
                        mapElapsed = if (config.view == AnalysisView.SPLIT || fixed) mapElapsed else null,
                        sectorDeltas = if (comparisonSelected && delta != null) summary else null,
                        onScrub = scrub,
                        onDoubleTap = {
                            if (config.view == AnalysisView.GRAPH) analysis.updateConfig { it.copy(view = AnalysisView.SPLIT) }
                        },
                        modifier = Modifier.fillMaxSize(),
                    )
                    // In Split the chips sit on the map instead.
                    if (config.view == AnalysisView.GRAPH) chips(Modifier.align(Alignment.BottomEnd).padding(end = 8.dp, bottom = 26.dp))
                }
            }
            val map: @Composable (Modifier) -> Unit = { modifier ->
                Column(modifier) {
                    Box(Modifier.fillMaxWidth().weight(1f)) {
                        AnalysisMap(trackId, mapLaps, mapElapsed, aeroMode, Modifier.fillMaxSize())
                        chips(Modifier.align(Alignment.BottomEnd).padding(8.dp))
                    }
                    if (fixed) AnalysisPlaybackBar(clock, mapElapsed, mapTotal, current?.sectorSplits?.map { it.elapsedSeconds }.orEmpty())
                }
            }
            val sectorTable: @Composable (Modifier) -> Unit = { modifier ->
                SectorTable(
                    catalog = catalog,
                    current = current,
                    comparison = comparison,
                    primaryLabel = "L$currentLapNumber",
                    comparisonLabel = if (comparisonLapNumber > 0) "L$comparisonLapNumber" else null,
                    primaryColor = currentColor,
                    comparisonColor = comparisonColor,
                    visibleDistance = visibleDistance,
                    summary = if (comparisonSelected && delta != null) summary else null,
                    positiveColor = positiveColor,
                    negativeColor = negativeColor,
                    modifier = modifier,
                )
            }
            when (config.view) {
                AnalysisView.GRAPH -> chart(Modifier.fillMaxSize())
                AnalysisView.MAP -> if (landscape) {
                    Row(Modifier.fillMaxSize()) {
                        map(Modifier.weight(1f).fillMaxHeight())
                        VerticalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                        sectorTable(Modifier.width(300.dp).fillMaxHeight())
                    }
                } else {
                    Column(Modifier.fillMaxSize()) {
                        map(Modifier.fillMaxWidth().weight(1f))
                        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                        sectorTable(Modifier.fillMaxWidth())
                    }
                }
                AnalysisView.SPLIT -> if (landscape) {
                    Row(Modifier.fillMaxSize()) {
                        chart(Modifier.weight(1f).fillMaxHeight())
                        VerticalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                        map(Modifier.weight(1f).fillMaxHeight())
                    }
                } else {
                    Column(Modifier.fillMaxSize()) {
                        map(Modifier.fillMaxWidth().weight(0.85f))
                        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                        chart(Modifier.fillMaxWidth().weight(1.15f))
                    }
                }
            }
        }
    }

    lapPicker?.let { target ->
        AnalysisLapSheet(
            analysis = analysis,
            catalog = catalog,
            playbackLap = store.playbackSelectedLap,
            initialTarget = target,
            lapAColor = currentColor,
            lapBColor = comparisonColor,
            onDismiss = { lapPicker = null },
        )
    }
    if (analysis.settingsOpen) {
        AnalysisConfigSheet(
            analysis = analysis,
            catalog = catalog,
            onDismiss = { analysis.settingsOpen = false },
        )
    }
}

/** Every "family.field" the configured series draw. */
private fun requiredChannels(config: AnalysisConfig): Set<String> = buildSet {
    for (series in config.series) {
        for (memberId in AnalysisCatalog.memberIds(series)) {
            AnalysisCatalog.metricById[memberId]?.let { addAll(it.channels) }
        }
    }
}

/**
 * How far into the current lap the desktop's cursor is, in metres, checked
 * ten times a second: null for the whole lap (a fixed comparison), NaN while
 * unknown.
 */
@Composable
private fun rememberVisibleDistance(followCursor: Boolean, current: AnalysisLap?, cursorOffset: State<Double>): State<Double?> {
    val visible = remember { mutableStateOf<Double?>(null) }
    LaunchedEffect(followCursor, current) {
        if (!followCursor) {
            visible.value = null
            return@LaunchedEffect
        }
        while (true) {
            val offset = cursorOffset.value
            val progress = current?.progress
            visible.value = when {
                !offset.isFinite() || progress == null -> Double.NaN
                offset >= progress.maxTime -> progress.maxDistance
                else -> progress.distanceAt(offset)
            }
            delay(DELTA_READOUT_INTERVAL_MS)
        }
    }
    return visible
}

@Composable
private fun ViewTabs(view: AnalysisView, onView: (AnalysisView) -> Unit, modifier: Modifier) {
    val views = listOf(AnalysisView.GRAPH to "Graph", AnalysisView.SPLIT to "Split", AnalysisView.MAP to "Map")
    PrimaryTabRow(
        selectedTabIndex = views.indexOfFirst { it.first == view }.coerceAtLeast(0),
        modifier = modifier.staticLayer(),
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        divider = {},
    ) {
        for ((option, label) in views) {
            Tab(
                selected = view == option,
                onClick = { onView(option) },
                text = { Text(label, maxLines = 1) },
                selectedContentColor = MaterialTheme.colorScheme.primary,
                unselectedContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/** The compared laps as chips that open the lap picker, and the lap delta. */
@Composable
private fun LapBar(
    catalog: PlaybackCatalog,
    fixed: Boolean,
    primaryLap: Int,
    primaryColor: Color,
    comparisonLap: Int,
    comparisonColor: Color,
    lapDelta: State<AnalysisDeltaSummary>?,
    positiveColor: Color,
    negativeColor: Color,
    onPick: (LapTarget) -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    Row(
        modifier.heightIn(min = 52.dp).padding(start = 12.dp, end = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        // The chips are cached as one texture; the live delta beside them is
        // not, or every change of it would re-render the chips too.
        Row(
            Modifier.staticLayer(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            LapChip(
                lap = primaryLap,
                color = primaryColor,
                time = null,
                live = !fixed && primaryLap > 0,
                placeholder = if (fixed) "Lap A" else "—",
                onClick = { onPick(if (fixed) LapTarget.LAP_A else LapTarget.COMPARE) },
            )
            Text("vs", style = MaterialTheme.typography.labelMedium, color = colors.onSurfaceVariant)
            LapChip(
                lap = comparisonLap,
                color = comparisonColor,
                // Two laps leave the delta no room for both times; the picker and
                // the Map table show them.
                time = if (fixed) null else lapTime(catalog, comparisonLap),
                live = false,
                placeholder = if (fixed) "Lap B" else "Compare",
                onClick = { onPick(if (fixed) LapTarget.LAP_B else LapTarget.COMPARE) },
            )
        }
        // The delta takes what the chips leave; the chips keep their full labels.
        Column(Modifier.weight(1f), horizontalAlignment = Alignment.End) {
            if (lapDelta != null) {
                // Read while drawing: the delta changes ten times a second.
                Readout(
                    runs = {
                        val value = lapDelta.value.lap
                        listOf(ReadoutRun(formatDelta(value), deltaColor(value, colors.onSurfaceVariant, positiveColor, negativeColor)))
                    },
                    style = MaterialTheme.typography.titleLarge.copy(fontWeight = FontWeight.SemiBold),
                    modifier = Modifier.fillMaxWidth(),
                    alignEnd = true,
                )
                Text(
                    "LAP Δ",
                    style = MaterialTheme.typography.labelSmall,
                    color = colors.onSurfaceVariant,
                )
            }
        }
    }
}

private fun lapTime(catalog: PlaybackCatalog, lap: Int): String? =
    catalog.laps.firstOrNull { it.lapNumber == lap }?.let { formatLapTimeMs(it.lapTimeMs) }

@Composable
private fun LapChip(
    lap: Int,
    color: Color,
    time: String?,
    live: Boolean,
    placeholder: String,
    onClick: () -> Unit,
) {
    val colors = MaterialTheme.colorScheme
    AssistChip(
        onClick = onClick,
        leadingIcon = {
            Box(Modifier.size(10.dp).background(if (lap > 0) color else color.copy(alpha = 0.35f), CircleShape))
        },
        trailingIcon = {
            Icon(painterResource(R.drawable.ic_expand_more), contentDescription = null, modifier = Modifier.size(AssistChipDefaults.IconSize))
        },
        label = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    if (lap > 0) "L$lap" else placeholder,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                )
                if (live) {
                    Text(
                        "LIVE",
                        modifier = Modifier
                            .padding(start = 6.dp)
                            .background(colors.primary, MaterialTheme.shapes.extraSmall)
                            .padding(horizontal = 4.dp),
                        style = MaterialTheme.typography.labelSmall,
                        fontWeight = FontWeight.SemiBold,
                        color = colors.onPrimary,
                        maxLines = 1,
                    )
                }
                if (lap > 0 && time != null) {
                    Text(
                        " $time",
                        style = MaterialTheme.typography.labelLarge.copy(fontFeatureSettings = "tnum"),
                        color = colors.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        },
    )
}

/** S1 S2 S3 deltas on one line, for when the x axis is not split into sectors. */
@Composable
private fun SectorDeltaLine(summary: State<AnalysisDeltaSummary>, positiveColor: Color, negativeColor: Color) {
    val colors = MaterialTheme.colorScheme
    val sectors = summary.value.sectors
    Row(
        Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(20.dp),
    ) {
        sectors.forEachIndexed { index, value ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("S${index + 1}", style = MaterialTheme.typography.labelMedium, color = colors.onSurfaceVariant)
                Text(
                    " ${if (value == null) "—" else formatDelta(value)}",
                    style = MaterialTheme.typography.labelMedium.copy(fontFeatureSettings = "tnum"),
                    fontWeight = FontWeight.SemiBold,
                    color = deltaColor(value, colors.onSurfaceVariant, positiveColor, negativeColor),
                )
            }
        }
    }
}

/** Zoom reset and the way back to the desktop's cursor, over the graph or map. */
@Composable
private fun ChartChips(
    zoomed: Boolean,
    onWholeLap: () -> Unit,
    backToLive: Boolean,
    onBackToLive: () -> Unit,
    modifier: Modifier = Modifier,
) {
    if (!zoomed && !backToLive) return
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        if (zoomed) {
            ElevatedAssistChip(
                onClick = onWholeLap,
                label = { Text("Whole lap") },
                leadingIcon = {
                    Icon(painterResource(R.drawable.ic_zoom_reset), contentDescription = null, modifier = Modifier.size(AssistChipDefaults.IconSize))
                },
            )
        }
        if (backToLive) {
            ElevatedAssistChip(
                onClick = onBackToLive,
                label = { Text("Back to live") },
                leadingIcon = {
                    Icon(painterResource(R.drawable.ic_my_location), contentDescription = null, modifier = Modifier.size(AssistChipDefaults.IconSize))
                },
            )
        }
    }
}

/**
 * Sector and lap times of both laps, and the delta between them. Following
 * the desktop, the current lap shows only the sectors its cursor has passed.
 */
@Composable
private fun SectorTable(
    catalog: PlaybackCatalog,
    current: AnalysisLap?,
    comparison: AnalysisLap?,
    primaryLabel: String,
    comparisonLabel: String?,
    primaryColor: Color,
    comparisonColor: Color,
    visibleDistance: State<Double?>,
    summary: State<AnalysisDeltaSummary>?,
    positiveColor: Color,
    negativeColor: Color,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    // The cursor moves ten times a second, but a sector time appears only as
    // a sector is completed: recompose then, not on every move.
    val currentTimes by remember(current, catalog) {
        derivedStateOf(structuralEqualityPolicy()) { sectorTimes(current, catalog, visibleDistance.value) }
    }
    val comparisonTimes = remember(comparison, catalog) { sectorTimes(comparison, catalog, null) }
    val numberStyle = MaterialTheme.typography.bodyMedium.copy(fontFeatureSettings = "tnum")
    @Composable
    fun RowScope.Cell(text: String, color: Color = colors.onSurface, weight: FontWeight = FontWeight.Normal, align: TextAlign = TextAlign.End) {
        Text(text, modifier = Modifier.weight(1f), style = numberStyle, color = color, fontWeight = weight, textAlign = align, maxLines = 1)
    }
    Column(modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
        Row(Modifier.fillMaxWidth().height(28.dp), verticalAlignment = Alignment.CenterVertically) {
            Spacer(Modifier.weight(0.6f))
            Row(Modifier.weight(1f), horizontalArrangement = Arrangement.End, verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(8.dp).background(primaryColor, CircleShape))
                Text(" $primaryLabel", style = MaterialTheme.typography.labelLarge, color = colors.onSurfaceVariant)
            }
            Row(Modifier.weight(1f), horizontalArrangement = Arrangement.End, verticalAlignment = Alignment.CenterVertically) {
                if (comparisonLabel != null) {
                    Box(Modifier.size(8.dp).background(comparisonColor, CircleShape))
                    Text(" $comparisonLabel", style = MaterialTheme.typography.labelLarge, color = colors.onSurfaceVariant)
                }
            }
            Cell("Δ", colors.onSurfaceVariant)
        }
        listOf("S1", "S2", "S3", "Lap").forEachIndexed { index, name ->
            HorizontalDivider(color = colors.outlineVariant.copy(alpha = 0.5f))
            Row(Modifier.fillMaxWidth().height(32.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(
                    name,
                    modifier = Modifier.weight(0.6f),
                    style = MaterialTheme.typography.labelLarge,
                    color = colors.onSurfaceVariant,
                    fontWeight = if (index == 3) FontWeight.SemiBold else FontWeight.Normal,
                )
                val bold = if (index == 3) FontWeight.SemiBold else FontWeight.Normal
                Cell(formatSeconds(currentTimes[index], index == 3), weight = bold)
                Cell(if (comparisonLabel == null) "" else formatSeconds(comparisonTimes[index], index == 3), colors.onSurfaceVariant, bold)
                // The live deltas are painted, read while drawing.
                Readout(
                    runs = {
                        val summed = summary?.value ?: return@Readout emptyList()
                        val value = if (index == 3) summed.lap else summed.sectors.getOrNull(index)
                        listOf(ReadoutRun(if (value == null) "—" else formatDelta(value), deltaColor(value, colors.onSurfaceVariant, positiveColor, negativeColor)))
                    },
                    style = numberStyle.copy(fontWeight = FontWeight.SemiBold),
                    modifier = Modifier.weight(1f),
                    alignEnd = true,
                )
            }
        }
    }
}

/** S1, S2, S3 and lap seconds of [lap], each null until [visible] metres have passed its end. */
private fun sectorTimes(lap: AnalysisLap?, catalog: PlaybackCatalog, visible: Double?): List<Double?> {
    lap ?: return listOf(null, null, null, null)
    if (visible?.isNaN() == true) return listOf(null, null, null, null)
    val splits = lap.sectorSplits
    val s1 = splits.firstOrNull { it.afterSector == 1 }
    val s2 = splits.firstOrNull { it.afterSector == 2 }
    val lapSeconds = catalog.laps.firstOrNull { it.lapNumber == lap.lapNumber }?.lapTimeMs?.takeIf { it > 0 }?.div(1000.0)
    val finished = visible == null || lap.progress?.let { visible >= it.maxDistance - 1.0 } == true
    fun passed(distance: Double) = visible == null || visible >= distance
    val first = s1?.takeIf { passed(it.distance) }?.elapsedSeconds
    val second = if (s1 != null && s2 != null && passed(s2.distance)) s2.elapsedSeconds - s1.elapsedSeconds else null
    val third = if (s2 != null && lapSeconds != null && finished) lapSeconds - s2.elapsedSeconds else null
    return listOf(first, second, third, lapSeconds?.takeIf { finished })
}

private fun formatSeconds(seconds: Double?, lap: Boolean): String {
    if (seconds == null || !seconds.isFinite() || seconds <= 0) return "—"
    if (lap || seconds >= 60) return formatLapTimeMs((seconds * 1000).toInt()) ?: "—"
    return String.format(Locale.US, "%.3f", seconds)
}

/** A lap's catalogue label: "Lap 12 · 1:31.234 · FL". */
internal fun lapOptionLabel(lapNumber: Int, lapTimeMs: Int, fastest: Boolean): String = buildString {
    append("Lap ").append(lapNumber)
    formatLapTimeMs(lapTimeMs)?.let { append(" · ").append(it) }
    if (fastest) append(" · FL")
}

private class EmptyState(
    val title: String,
    val body: String,
    val loading: Boolean = false,
    val action: String? = null,
)

/** The Material empty state: an icon or progress, a headline, supporting text and an action. */
@Composable
private fun AnalysisEmptyState(state: EmptyState, onAction: () -> Unit, modifier: Modifier = Modifier) {
    Column(
        modifier.padding(horizontal = 32.dp).widthIn(max = 360.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (state.loading) {
            CircularProgressIndicator(Modifier.size(40.dp))
        } else {
            Icon(
                painterResource(R.drawable.ic_analysis),
                contentDescription = null,
                modifier = Modifier.size(48.dp),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Text(
            state.title,
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onSurface,
            textAlign = TextAlign.Center,
        )
        Text(
            state.body,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
        state.action?.let { label ->
            FilledTonalButton(onClick = onAction) { Text(label) }
        }
    }
}
