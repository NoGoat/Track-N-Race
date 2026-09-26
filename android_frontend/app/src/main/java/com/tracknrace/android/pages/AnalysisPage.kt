package com.tracknrace.android.pages

import android.content.res.Configuration
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.material3.VerticalDivider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.tracknrace.android.AnalysisCatalog
import com.tracknrace.android.AnalysisConfig
import com.tracknrace.android.AnalysisController
import com.tracknrace.android.AnalysisDeltaCurve
import com.tracknrace.android.AnalysisLap
import com.tracknrace.android.AnalysisView
import com.tracknrace.android.DEFAULT_COMPARE_LABEL
import com.tracknrace.android.DEFAULT_CURRENT_LABEL
import com.tracknrace.android.DEFAULT_DELTA_NEGATIVE_COLOR
import com.tracknrace.android.DEFAULT_DELTA_POSITIVE_COLOR
import com.tracknrace.android.DEFAULT_LAP_A_LABEL
import com.tracknrace.android.DEFAULT_LAP_B_LABEL
import com.tracknrace.android.PairedTelemetryClient
import com.tracknrace.android.R
import com.tracknrace.android.TelemetryStore
import com.tracknrace.android.formatDelta
import com.tracknrace.android.formatLapTimeMs
import com.tracknrace.android.summarizeDelta
import kotlin.math.abs
import kotlin.math.max
import kotlinx.coroutines.delay

private const val LAP_SYNC_INTERVAL_MS = 1_000L
private const val DELTA_READOUT_INTERVAL_MS = 100L
private const val CLOCK_FRAME_NANOS = 30_000_000L

/**
 * Lap analysis of the recording the desktop is playing: Graph, Split and Map,
 * as on the desktop's Analysis page. The current lap follows the desktop's
 * cursor unless a fixed Lap A / Lap B comparison is chosen; everything else is
 * set in the configuration sheet.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun AnalysisScreen(
    store: TelemetryStore,
    analysis: AnalysisController,
    active: Boolean,
) {
    val paired = store.settings.source == PairedTelemetryClient.SOURCE_PAIRED
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

    val chartState = remember { AnalysisChartState() }
    var configOpen by remember { mutableStateOf(false) }
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE

    Column(Modifier.fillMaxSize()) {
        AnalysisToolbar(
            view = config.view,
            // Choosing a view drops a point opened from the graph, as on the desktop.
            onView = { view ->
                mapFocus = null
                analysis.updateConfig { it.copy(view = view) }
            },
            zoomed = chartState.zoomed && config.view != AnalysisView.MAP,
            onResetZoom = chartState::resetZoom,
            onConfigure = { configOpen = true },
        )
        LapLegend(
            primaryLap = currentLapNumber,
            primaryColor = currentColor,
            comparisonLap = comparisonLapNumber,
            comparisonColor = comparisonColor,
            delta = delta,
            current = current,
            comparison = comparison,
            followCursor = !fixed,
            cursorOffset = cursorOffset,
            showSectors = true,
            positiveColor = Color(deltaSeries?.color ?: DEFAULT_DELTA_POSITIVE_COLOR),
            negativeColor = Color(deltaSeries?.negativeColor ?: DEFAULT_DELTA_NEGATIVE_COLOR),
        )
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
                "Pick Lap A and Lap B in the configuration.",
                action = "Configure",
            )
            currentLapNumber <= 0 -> EmptyState("Waiting for a lap", "The desktop's playback has not reached a lap yet.", loading = true)
            current == null -> EmptyState("Loading lap $currentLapNumber", "Fetching it from the desktop.", loading = true)
            else -> null
        }
        Box(Modifier.fillMaxWidth().weight(1f)) {
            if (empty != null) {
                AnalysisEmptyState(empty, onAction = { configOpen = true }, modifier = Modifier.align(Alignment.Center))
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
                syncedTooltip = config.syncedTooltip,
                sectorBoundaries = config.sectorBoundaries,
                realtime = !fixed,
                zoomEnabled = fixed,
                currentLabel = primaryLabel,
                comparisonLabel = comparisonLabel,
                deltaPositive = Color(deltaSeries?.color ?: DEFAULT_DELTA_POSITIVE_COLOR),
                deltaNegative = Color(deltaSeries?.negativeColor ?: DEFAULT_DELTA_NEGATIVE_COLOR),
                mapCursorColors = if (config.view == AnalysisView.SPLIT && fixed) listOf(currentColor, comparisonColor) else null,
            )
            val mapLaps = listOfNotNull(
                current?.let { MapLap(it, currentColor, primaryLabel) },
                comparison?.let { MapLap(it, comparisonColor, comparisonLabel) },
            )
            val inspect: (Double) -> Unit = { elapsed ->
                if (fixed) clock.seek(elapsed) else mapFocus = elapsed
                if (config.view == AnalysisView.GRAPH) {
                    analysis.updateConfig { it.copy(view = AnalysisView.SPLIT) }
                }
            }
            val chart: @Composable (Modifier) -> Unit = { modifier ->
                AnalysisChart(
                    input = chartInput,
                    state = chartState,
                    cursorOffset = cursorOffset,
                    mapElapsed = if (config.view == AnalysisView.SPLIT) mapElapsed else null,
                    onInspect = inspect,
                    modifier = modifier,
                )
            }
            val map: @Composable (Modifier) -> Unit = { modifier ->
                Column(modifier) {
                    AnalysisMap(store.cold.trackId, mapLaps, mapElapsed, store.cold.aeroMode, Modifier.fillMaxWidth().weight(1f))
                    if (fixed) AnalysisPlaybackBar(clock, mapElapsed, mapTotal)
                }
            }
            when (config.view) {
                AnalysisView.GRAPH -> chart(Modifier.fillMaxSize())
                AnalysisView.MAP -> map(Modifier.fillMaxSize())
                AnalysisView.SPLIT -> if (landscape) {
                    Row(Modifier.fillMaxSize()) {
                        chart(Modifier.weight(1f).fillMaxHeight())
                        VerticalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                        map(Modifier.weight(1f).fillMaxHeight())
                    }
                } else {
                    Column(Modifier.fillMaxSize()) {
                        map(Modifier.fillMaxWidth().weight(0.9f))
                        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                        chart(Modifier.fillMaxWidth().weight(1.1f))
                    }
                }
            }
        }
    }

    if (configOpen) {
        AnalysisConfigSheet(
            store = store,
            analysis = analysis,
            onDismiss = { configOpen = false },
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

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AnalysisToolbar(
    view: AnalysisView,
    onView: (AnalysisView) -> Unit,
    zoomed: Boolean,
    onResetZoom: () -> Unit,
    onConfigure: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().padding(start = 12.dp, end = 4.dp, top = 6.dp, bottom = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        val views = listOf(
            Triple(AnalysisView.GRAPH, "Graph", R.drawable.ic_view_graph),
            Triple(AnalysisView.SPLIT, "Split", R.drawable.ic_view_split),
            Triple(AnalysisView.MAP, "Map", R.drawable.ic_view_map),
        )
        SingleChoiceSegmentedButtonRow(Modifier.weight(1f)) {
            views.forEachIndexed { index, (option, label, icon) ->
                SegmentedButton(
                    selected = view == option,
                    onClick = { onView(option) },
                    shape = SegmentedButtonDefaults.itemShape(index, views.size),
                    icon = {
                        Icon(painterResource(icon), contentDescription = null, modifier = Modifier.size(18.dp))
                    },
                ) {
                    Text(label, maxLines = 1)
                }
            }
        }
        if (zoomed) {
            IconButton(onClick = onResetZoom) {
                Icon(painterResource(R.drawable.ic_zoom_reset), contentDescription = "Show the whole lap")
            }
        }
        IconButton(onClick = onConfigure) {
            Icon(painterResource(R.drawable.ic_tune), contentDescription = "Configure analysis")
        }
    }
}

/** Which laps are compared, and the running delta between them. */
@Composable
private fun LapLegend(
    primaryLap: Int,
    primaryColor: Color,
    comparisonLap: Int,
    comparisonColor: Color,
    delta: AnalysisDeltaCurve?,
    current: AnalysisLap?,
    comparison: AnalysisLap?,
    followCursor: Boolean,
    cursorOffset: State<Double>,
    showSectors: Boolean,
    positiveColor: Color,
    negativeColor: Color,
) {
    Row(
        Modifier.fillMaxWidth().height(34.dp).padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        LegendEntry(primaryLap, primaryColor)
        Text("vs", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        LegendEntry(comparisonLap, comparisonColor)
        Spacer(Modifier.weight(1f))
        DeltaReadout(delta, current, comparison, followCursor, cursorOffset, showSectors, positiveColor, negativeColor)
    }
}

@Composable
private fun LegendEntry(lap: Int, color: Color) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(8.dp).clip(CircleShape).background(if (lap > 0) color else color.copy(alpha = 0.3f)))
        Spacer(Modifier.width(5.dp))
        Text(
            if (lap > 0) "L$lap" else "—",
            style = MaterialTheme.typography.labelMedium,
            fontWeight = FontWeight.SemiBold,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
        )
    }
}

/**
 * S1 S2 S3 and lap delta. Following the desktop they run up to its cursor;
 * a fixed comparison shows the whole lap.
 */
@Composable
private fun DeltaReadout(
    delta: AnalysisDeltaCurve?,
    current: AnalysisLap?,
    comparison: AnalysisLap?,
    followCursor: Boolean,
    cursorOffset: State<Double>,
    showSectors: Boolean,
    positiveColor: Color,
    negativeColor: Color,
) {
    var visibleDistance by remember { mutableStateOf<Double?>(null) }
    LaunchedEffect(followCursor, current) {
        if (!followCursor) {
            visibleDistance = null
            return@LaunchedEffect
        }
        while (true) {
            val offset = cursorOffset.value
            val progress = current?.progress
            visibleDistance = when {
                !offset.isFinite() || progress == null -> Double.NaN
                offset >= progress.maxTime -> progress.maxDistance
                else -> progress.distanceAt(offset)
            }
            delay(DELTA_READOUT_INTERVAL_MS)
        }
    }
    val summary by remember(delta, current, comparison) {
        derivedStateOf { summarizeDelta(delta, current, comparison, visibleDistance) }
    }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
        val values = if (showSectors) {
            listOf("S1" to summary.sectors[0], "S2" to summary.sectors[1], "S3" to summary.sectors[2], "Lap" to summary.lap)
        } else {
            listOf("Lap" to summary.lap)
        }
        for ((label, value) in values) {
            Column(horizontalAlignment = Alignment.End) {
                Text(
                    label,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    formatDelta(value),
                    style = MaterialTheme.typography.labelMedium.copy(fontFeatureSettings = "tnum"),
                    fontWeight = FontWeight.SemiBold,
                    color = when {
                        value == null || abs(value) < 0.0005 -> MaterialTheme.colorScheme.onSurfaceVariant
                        value > 0 -> positiveColor
                        else -> negativeColor
                    },
                )
            }
        }
    }
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
