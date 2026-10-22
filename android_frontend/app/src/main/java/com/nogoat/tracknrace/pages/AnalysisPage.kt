package com.nogoat.tracknrace.pages

import android.content.res.Configuration
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectVerticalDragGestures
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
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ElevatedAssistChip
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.PrimaryTabRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.material3.VerticalDivider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import com.nogoat.tracknrace.AnalysisCatalog
import com.nogoat.tracknrace.AnalysisConfig
import com.nogoat.tracknrace.AnalysisController
import com.nogoat.tracknrace.AnalysisDeltaCurve
import com.nogoat.tracknrace.AnalysisDeltaSummary
import com.nogoat.tracknrace.AnalysisLap
import com.nogoat.tracknrace.AnalysisView
import com.nogoat.tracknrace.DEFAULT_COMPARE_LABEL
import com.nogoat.tracknrace.DEFAULT_CURRENT_LABEL
import com.nogoat.tracknrace.DEFAULT_DELTA_NEGATIVE_COLOR
import com.nogoat.tracknrace.DEFAULT_DELTA_POSITIVE_COLOR
import com.nogoat.tracknrace.DEFAULT_LAP_A_LABEL
import com.nogoat.tracknrace.DEFAULT_LAP_B_LABEL
import com.nogoat.tracknrace.PairedTelemetryClient
import com.nogoat.tracknrace.PlaybackCatalog
import com.nogoat.tracknrace.R
import com.nogoat.tracknrace.TelemetryStore
import com.nogoat.tracknrace.formatDelta
import com.nogoat.tracknrace.formatLapTimeMs
import com.nogoat.tracknrace.summarizeDelta
import java.util.Locale
import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.max
import kotlin.math.min
import kotlinx.coroutines.delay

private const val LAP_SYNC_INTERVAL_MS = 1_000L
// The followed cursor: how far ahead of the newest sample it may run before
// waiting for the next, how quickly it closes on a sample it has drifted
// from, and how far off a sample must be to be a seek rather than drift.
private const val CURSOR_MAX_LEAD_SECONDS = 0.5
private const val CURSOR_CORRECTION_SECONDS = 0.15
private const val CURSOR_SNAP_SECONDS = 1.0
// The sector strip's last cell: the whole lap, after S1 to S3.
private const val WHOLE_LAP = 3
private val LANDSCAPE_RAIL_WIDTH = 232.dp

/**
 * The desktop's playback cursor as a smooth clock. Its position arrives a few
 * times a second, in bursts; between samples the cursor moves on at the
 * playback speed, and each new sample pulls it back gently rather than making
 * it jump. A seek or a pause is followed at once.
 */
private class SmoothCursor {
    private var sample = Double.NaN
    private var sampleAt = 0L
    private var shown = Double.NaN
    private var shownAt = 0L

    fun at(now: Long, latest: Double, playing: Boolean, speed: Float): Double {
        if (!latest.isFinite()) {
            shown = Double.NaN
            sample = Double.NaN
            return Double.NaN
        }
        if (latest != sample) {
            sample = latest
            sampleAt = now
        }
        val rate = if (playing) speed.toDouble().coerceAtLeast(0.0) else 0.0
        // Where the desktop is now, by the newest sample and the speed.
        val predicted = sample + min((now - sampleAt) / 1e9, CURSOR_MAX_LEAD_SECONDS) * rate
        if (!shown.isFinite() || !playing || abs(predicted - shown) > CURSOR_SNAP_SECONDS) {
            shown = predicted
        } else {
            val dt = ((now - shownAt) / 1e9).coerceIn(0.0, 0.1)
            val advanced = shown + dt * rate
            val pull = 1.0 - exp(-dt / CURSOR_CORRECTION_SECONDS)
            shown = advanced + (predicted - advanced) * pull
        }
        shownAt = now
        return shown
    }
}

/**
 * Lap analysis of the recording playing, on this phone or on the paired
 * desktop: Graph, Split and Map tabs over a lap bar that names the compared
 * laps and their delta. The current lap follows the playback cursor unless
 * two fixed laps are chosen.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun AnalysisScreen(
    store: TelemetryStore,
    analysis: AnalysisController,
    active: Boolean,
) {
    // The recording analysed: one open on the phone (Direct mode), else the
    // one the paired desktop is playing.
    val local = store.localRecording != null
    val paired = store.settings.source == PairedTelemetryClient.SOURCE_PAIRED
    // Read through derived state: the rest of the cold state (lap clock, ERS,
    // fuel) changes several times a second and would recompose this screen.
    val trackId by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.cold.trackId } }
    val aeroMode by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.cold.aeroMode } }
    val slmTrackStatus by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.cold.slmTrackStatus } }
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
        val cursorClock = SmoothCursor()
        while (true) {
            withFrameNanos { now ->
                // Every frame: the desktop's cursor arrives in uneven bursts, so
                // it is carried forward at the playback speed between them.
                val transport = store.latestTransport()
                val cursor = cursorClock.at(now, store.latestPlaybackCursorTime(), transport.playing, transport.speed)
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
    // Portrait shows the laps as one slim strip; tapping it opens the full
    // lap row over the page, as the player opens from its strip.
    var lapsExpanded by rememberSaveable { mutableStateOf(false) }
    var tabsHeight by remember { mutableIntStateOf(0) }
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    BackHandler(enabled = lapsExpanded && !landscape) { lapsExpanded = false }
    val onView: (AnalysisView) -> Unit = { view ->
        // Choosing a view drops a point opened from the graph, as on the desktop.
        mapFocus = null
        analysis.updateConfig { it.copy(view = view) }
    }

    // The sector the graph is zoomed to, from the sector strip: its range
    // follows the laps shown. A pinch zoom of two fixed laps is kept until
    // they are left; following playback has no zoom but a sector's.
    var sectorFocus by remember { mutableStateOf<Int?>(null) }
    val sectors = remember(current, comparison, distanceMode, catalog.trackLengthM) {
        sectorDomains(current, comparison, distanceMode, catalog.trackLengthM)
    }
    LaunchedEffect(fixed, sectorFocus, sectors) {
        val range = sectorFocus?.let { sectors.getOrNull(it) }
        if (range != null) chartState.zoom = range else if (!fixed) chartState.resetZoom()
    }
    // The strip's selection: a sector, the whole lap, or none for a pinch zoom.
    val zoom = chartState.zoom
    val stripSelection = when {
        zoom == null -> WHOLE_LAP
        sectorFocus != null && sectors.getOrNull(sectorFocus!!) == zoom -> sectorFocus
        else -> null
    }
    val onSector: (Int) -> Unit = { index ->
        if (index == WHOLE_LAP || index == stripSelection) {
            sectorFocus = null
            chartState.resetZoom()
        } else {
            sectorFocus = index
        }
    }
    val swapLaps: () -> Unit = {
        val first = currentLapNumber
        val second = comparisonLapNumber
        analysis.selectLapA(second)
        analysis.selectLapB(first)
        analysis.useFixedLaps(true)
    }
    val lapDelta = if (comparisonSelected && delta != null) summary else null
    val lapRow: @Composable (Boolean) -> Unit = { stacked ->
        LapSelectorRow(
            catalog = catalog,
            fixed = fixed,
            lapA = currentLapNumber,
            lapB = comparisonLapNumber,
            colorA = currentColor,
            colorB = comparisonColor,
            stacked = stacked,
            onPick = { lapPicker = it },
            onSwap = swapLaps,
        )
    }
    val sectorStrip: @Composable (Boolean) -> Unit = { stacked ->
        if (sectors.isNotEmpty()) {
            SectorStrip(
                summary = lapDelta,
                selected = stripSelection,
                stacked = stacked,
                positiveColor = positiveColor,
                negativeColor = negativeColor,
                onSelect = onSector,
            )
        }
    }

    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            ViewTabs(config.view, onView, Modifier.fillMaxWidth().onSizeChanged { tabsHeight = it.height })
            if (landscape) {
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            } else {
                // Stays in place while the full row is open over it, so the page
                // below does not move.
                LapSummaryBar(
                    catalog = catalog,
                    fixed = fixed,
                    lapA = currentLapNumber,
                    lapB = comparisonLapNumber,
                    colorA = currentColor,
                    colorB = comparisonColor,
                    onExpand = { lapsExpanded = true },
                )
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                sectorStrip(false)
                if (sectors.isNotEmpty()) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            }

            val empty = when {
                !local && !paired -> EmptyState(
                    "No recording open",
                    "Analysis reads a recording's laps. Open one from the menu, or pair a desktop and play one there.",
                )
                !catalog.active && local -> EmptyState("Opening the recording", "Reading its laps.", loading = true)
                !catalog.active -> EmptyState("No recording playing", "Open a recording on the desktop, or one on this phone from the menu.")
                !local && analysis.lapDataUnavailable -> EmptyState(
                    "Desktop update needed",
                    "This desktop cannot send laps yet. Update Track N Race on the desktop.",
                )
                currentLapNumber <= 0 && fixed -> EmptyState(
                    "Choose two laps",
                    "Pick Lap A and Lap B to compare.",
                    action = "Choose laps",
                )
                currentLapNumber <= 0 -> EmptyState("Waiting for a lap", "Playback has not reached a lap yet.", loading = true)
                current == null -> EmptyState(
                    "Loading lap $currentLapNumber",
                    if (local) "Reading it from the recording." else "Fetching it from the desktop.",
                    loading = true,
                )
                else -> null
            }
            // Landscape: the laps and sectors in a column beside the content.
            Row(Modifier.fillMaxWidth().weight(1f)) {
                if (landscape) {
                    Column(Modifier.width(LANDSCAPE_RAIL_WIDTH).fillMaxHeight()) {
                        lapRow(true)
                        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                        sectorStrip(true)
                    }
                    VerticalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                }
                Box(Modifier.weight(1f).fillMaxHeight()) {
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
                        lapColors = listOf(currentColor, comparisonColor),
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
                            // A sector's zoom is undone from the sector strip.
                            zoomed = chartState.zoomed && stripSelection == null,
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
                                AnalysisMap(trackId, mapLaps, mapElapsed, aeroMode, Modifier.fillMaxSize(), focusSector = stripSelection?.takeIf { it != WHOLE_LAP }, slmTrackStatus = slmTrackStatus)
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
        }
        if (lapsExpanded && !landscape) {
            // Over the page: a tap outside it hides it, and is not passed on.
            Box(Modifier.fillMaxSize().pointerInput(Unit) { detectTapGestures { lapsExpanded = false } })
            Surface(
                Modifier
                    .fillMaxWidth()
                    .padding(top = with(LocalDensity.current) { tabsHeight.toDp() })
                    .pointerInput(Unit) {
                        // A swipe up hides it.
                        var travel = 0f
                        detectVerticalDragGestures(
                            onDragStart = { travel = 0f },
                            onDragEnd = { if (travel < -32.dp.toPx()) lapsExpanded = false },
                        ) { change, amount ->
                            travel += amount
                            change.consume()
                        }
                    },
                shape = RoundedCornerShape(bottomStart = 16.dp, bottomEnd = 16.dp),
                color = MaterialTheme.colorScheme.surfaceContainer,
                shadowElevation = 6.dp,
            ) {
                lapRow(false)
            }
        }
    }

    lapPicker?.let { target ->
        AnalysisLapSheet(
            analysis = analysis,
            catalog = catalog,
            playbackLap = store.playbackSelectedLap,
            target = target,
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
            withFrameNanos { }
            val offset = cursorOffset.value
            val progress = current?.progress
            val next = when {
                !offset.isFinite() || progress == null -> Double.NaN
                offset >= progress.maxTime -> progress.maxDistance
                else -> progress.distanceAt(offset)
            }
            if (next != visible.value && !(next.isNaN() && visible.value?.isNaN() == true)) visible.value = next
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

/**
 * The compared laps, one button each: its colour, number, role and time.
 * Each opens that lap's sheet; between them, a swap fixes the two laps the
 * other way round. [stacked] puts them one above the other, for the
 * landscape column.
 */
@Composable
private fun LapSelectorRow(
    catalog: PlaybackCatalog,
    fixed: Boolean,
    lapA: Int,
    lapB: Int,
    colorA: Color,
    colorB: Color,
    stacked: Boolean,
    onPick: (LapTarget) -> Unit,
    onSwap: () -> Unit,
) {
    val first: @Composable (Modifier) -> Unit = { modifier ->
        LapButton(
            lap = lapA,
            color = colorA,
            placeholder = if (fixed) "Lap A" else "Waiting",
            role = if (fixed) "fixed" else "following",
            roleColor = null,
            time = lapTime(catalog, lapA),
            onClick = { onPick(LapTarget.LAP_A) },
            modifier = modifier,
        )
    }
    val second: @Composable (Modifier) -> Unit = { modifier ->
        val fastest = lapB > 0 && lapB == catalog.fastestLap
        LapButton(
            lap = lapB,
            color = colorB,
            placeholder = if (fixed) "Lap B" else "Compare",
            role = when {
                lapB <= 0 -> "none chosen"
                fastest -> "fastest"
                else -> null
            },
            roleColor = if (fastest) FastestLapColor else null,
            time = lapTime(catalog, lapB),
            onClick = { onPick(LapTarget.LAP_B) },
            modifier = modifier,
        )
    }
    val swap: @Composable () -> Unit = {
        IconButton(onClick = onSwap, enabled = lapA > 0 && lapB > 0) {
            Icon(painterResource(R.drawable.ic_swap_horiz), contentDescription = "Swap laps", modifier = Modifier.size(20.dp))
        }
    }
    if (stacked) {
        Row(Modifier.fillMaxWidth().padding(start = 4.dp, top = 4.dp, bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                first(Modifier.fillMaxWidth())
                second(Modifier.fillMaxWidth())
            }
            swap()
        }
    } else {
        Row(
            Modifier.fillMaxWidth().heightIn(min = 60.dp).padding(horizontal = 4.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            first(Modifier.weight(1f))
            swap()
            second(Modifier.weight(1f))
        }
    }
}

/**
 * The compared laps folded to one slim strip, like the player's: each lap's
 * colour, number and time. Tapping it opens [LapSelectorRow] over the page.
 */
@Composable
private fun LapSummaryBar(
    catalog: PlaybackCatalog,
    fixed: Boolean,
    lapA: Int,
    lapB: Int,
    colorA: Color,
    colorB: Color,
    onExpand: () -> Unit,
) {
    val colors = MaterialTheme.colorScheme
    val timeStyle = MaterialTheme.typography.labelMedium.copy(fontFeatureSettings = "tnum")
    // Each lap fills half the strip, under its button in the open row.
    val lap: @Composable RowScope.(Int, Color, String) -> Unit = { number, color, placeholder ->
        Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(width = 3.dp, height = 14.dp).background(if (number > 0) color else color.copy(alpha = 0.35f), RoundedCornerShape(1.5.dp)))
            Text(
                if (number > 0) "L$number" else placeholder,
                modifier = Modifier.padding(start = 6.dp),
                style = MaterialTheme.typography.labelLarge,
                maxLines = 1,
            )
            val time = if (number > 0) lapTime(catalog, number) else null
            if (time != null) {
                Text(time, modifier = Modifier.padding(start = 6.dp), style = timeStyle, color = colors.onSurfaceVariant, maxLines = 1)
            }
        }
    }
    Row(
        Modifier
            .fillMaxWidth()
            .height(32.dp)
            .background(colors.surfaceContainer)
            .clickable(onClickLabel = "Show laps", onClick = onExpand)
            .padding(start = 14.dp, end = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        lap(lapA, colorA, if (fixed) "Lap A" else "Waiting")
        // As wide as the open row's swap button, so the halves line up.
        Text("vs", modifier = Modifier.width(48.dp), style = timeStyle, color = colors.onSurfaceVariant, textAlign = TextAlign.Center)
        lap(lapB, colorB, if (fixed) "Lap B" else "Compare")
        Icon(
            painterResource(R.drawable.ic_expand_more),
            contentDescription = null,
            modifier = Modifier.size(20.dp),
            tint = colors.onSurfaceVariant,
        )
    }
}

private fun lapTime(catalog: PlaybackCatalog, lap: Int): String? =
    catalog.laps.firstOrNull { it.lapNumber == lap }?.let { formatLapTimeMs(it.lapTimeMs) }

@Composable
private fun LapButton(
    lap: Int,
    color: Color,
    placeholder: String,
    role: String?,
    roleColor: Color?,
    time: String?,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val detail = buildAnnotatedString {
        if (role != null) {
            withStyle(SpanStyle(color = roleColor ?: colors.onSurfaceVariant)) { append(role) }
        }
        if (lap > 0 && time != null) {
            if (role != null) append(" · ")
            append(time)
        }
    }
    Row(
        modifier
            .heightIn(min = 52.dp)
            .clip(RoundedCornerShape(12.dp))
            .clickable(onClickLabel = "Change lap", onClick = onClick)
            .padding(start = 10.dp, end = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Box(Modifier.size(width = 4.dp, height = 32.dp).background(if (lap > 0) color else color.copy(alpha = 0.35f), RoundedCornerShape(2.dp)))
        Column(Modifier.weight(1f)) {
            Text(
                if (lap > 0) "Lap $lap" else placeholder,
                style = MaterialTheme.typography.titleMedium,
                maxLines = 1,
            )
            if (detail.isNotEmpty()) {
                Text(
                    detail,
                    style = MaterialTheme.typography.bodySmall.copy(fontFeatureSettings = "tnum"),
                    color = colors.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        Icon(
            painterResource(R.drawable.ic_expand_more),
            contentDescription = null,
            modifier = Modifier.size(20.dp),
            tint = colors.onSurfaceVariant,
        )
    }
}

/**
 * S1, S2, S3 and the lap, each with its delta while comparing. Tapping a
 * sector zooms the graph to it and fades the rest of the map; the lap, or the
 * selected sector again, shows the whole lap. [stacked] lists them one per
 * row, for the landscape column.
 */
@Composable
private fun SectorStrip(
    summary: State<AnalysisDeltaSummary>?,
    selected: Int?,
    stacked: Boolean,
    positiveColor: Color,
    negativeColor: Color,
    onSelect: (Int) -> Unit,
) {
    val colors = MaterialTheme.colorScheme
    val names = listOf("S1", "S2", "S3", "Lap")
    // The deltas change ten times a second: painted, read while drawing.
    val value: @Composable (Int, TextStyle, Modifier, Boolean) -> Unit = { index, valueStyle, modifier, alignEnd ->
        Readout(
            runs = {
                val summed = summary?.value
                val delta = if (index == WHOLE_LAP) summed?.lap else summed?.sectors?.getOrNull(index)
                listOf(ReadoutRun(if (delta == null) "—" else formatDelta(delta), deltaColor(delta, colors.onSurfaceVariant, positiveColor, negativeColor)))
            },
            style = valueStyle,
            modifier = modifier,
            alignEnd = alignEnd,
            center = !alignEnd,
        )
    }
    if (stacked) {
        Column(Modifier.fillMaxWidth().selectableGroup()) {
            names.forEachIndexed { index, name ->
                val on = selected == index
                Row(
                    Modifier
                        .fillMaxWidth()
                        .height(44.dp)
                        .selectable(selected = on, role = Role.Tab, onClick = { onSelect(index) })
                        .background(if (on) colors.secondaryContainer.copy(alpha = 0.45f) else Color.Transparent)
                        .drawBehind { if (on) drawRect(colors.primary, size = Size(3.dp.toPx(), size.height)) }
                        .padding(horizontal = 16.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        name,
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.labelLarge,
                        color = if (on) colors.primary else colors.onSurfaceVariant,
                    )
                    value(index, MaterialTheme.typography.labelLarge.copy(fontWeight = FontWeight.SemiBold), Modifier.width(80.dp), true)
                }
            }
        }
    } else {
        // One slim line: each cell's name and delta side by side.
        val compactStyle = MaterialTheme.typography.labelMedium.copy(fontWeight = FontWeight.SemiBold)
        // Wide enough for "+0.000" at this size; a wider delta shrinks to fit.
        val deltaWidth = with(LocalDensity.current) { (compactStyle.fontSize.toPx() * 3.6f).toDp() }
        Row(Modifier.fillMaxWidth().height(32.dp).selectableGroup()) {
            names.forEachIndexed { index, name ->
                val on = selected == index
                Row(
                    Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .selectable(selected = on, role = Role.Tab, onClick = { onSelect(index) })
                        .drawBehind {
                            if (on) {
                                val bar = 2.dp.toPx()
                                drawRect(colors.primary, Offset(0f, size.height - bar), Size(size.width, bar))
                            }
                        },
                    horizontalArrangement = Arrangement.Center,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        name,
                        style = MaterialTheme.typography.labelMedium,
                        color = if (on) colors.primary else colors.onSurfaceVariant,
                        maxLines = 1,
                    )
                    value(index, compactStyle, Modifier.padding(start = 4.dp).width(deltaWidth), false)
                }
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
        // The better of the two laps in each sector, once both have all three.
        val best = if (comparisonLabel == null) null else bestOfBoth(currentTimes, comparisonTimes)
        if (best != null) {
            HorizontalDivider(color = colors.outlineVariant)
            Row(Modifier.fillMaxWidth().height(36.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "Best of both, by sector",
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.bodyMedium,
                    color = colors.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(formatSeconds(best, true), style = numberStyle, color = FastestLapColor, fontWeight = FontWeight.SemiBold)
            }
        }
    }
}

/** The sum of the faster of each lap's S1, S2 and S3, or null while any is missing. */
private fun bestOfBoth(first: List<Double?>, second: List<Double?>): Double? {
    var total = 0.0
    for (sector in 0 until 3) {
        val a = first.getOrNull(sector) ?: return null
        val b = second.getOrNull(sector) ?: return null
        total += min(a, b)
    }
    return total
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
