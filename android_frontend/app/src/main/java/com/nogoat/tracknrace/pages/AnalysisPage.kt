package com.nogoat.tracknrace.pages

import android.content.res.Configuration
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
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
import androidx.compose.ui.graphics.luminance
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
import com.nogoat.tracknrace.AnalysisLapKey
import com.nogoat.tracknrace.AnalysisView
import com.nogoat.tracknrace.DEFAULT_COMPARE_LABEL
import com.nogoat.tracknrace.DEFAULT_CURRENT_LABEL
import com.nogoat.tracknrace.DEFAULT_DELTA_NEGATIVE_COLOR
import com.nogoat.tracknrace.DEFAULT_DELTA_POSITIVE_COLOR
import com.nogoat.tracknrace.DEFAULT_LAP_A_LABEL
import com.nogoat.tracknrace.DEFAULT_LAP_B_LABEL
import com.nogoat.tracknrace.MapApex
import com.nogoat.tracknrace.MapLapView
import com.nogoat.tracknrace.MapLayer
import com.nogoat.tracknrace.PairedTelemetryClient
import com.nogoat.tracknrace.R
import com.nogoat.tracknrace.TelemetryStore
import com.nogoat.tracknrace.formatDelta
import com.nogoat.tracknrace.formatLapTimeMs
import com.nogoat.tracknrace.parseColor
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
    // Each lap's driver; the followed lap is always the driver playing.
    val currentDriver = if (fixed) analysis.lapADriver else -1
    val comparisonDriver = if (fixed) analysis.lapBDriver else analysis.compareDriver
    val currentKey = AnalysisLapKey(currentDriver, currentLapNumber)
    val comparisonKey = AnalysisLapKey(comparisonDriver, comparisonLapNumber)
    val channels = remember(config.series) { requiredChannels(config) }
    val sectorDelta = config.sectorBoundaries && config.sectorDelta
    val distanceMode = catalog.deltaAvailable

    // The Map view's braking zones: switched on and off from the map's tool
    // rail, and while on, listed or one zone's head to head in the drawer.
    // Closing the drawer leaves them on the map. They are fetched only while on.
    var brakingOn by rememberSaveable { mutableStateOf(false) }
    var brakingPanel by rememberSaveable { mutableStateOf(BrakingPanel.CLOSED) }
    var brakingZoneIndex by rememberSaveable { mutableIntStateOf(0) }
    val brakingZonesOn = config.view == AnalysisView.MAP && brakingOn
    val brakingShown = brakingZonesOn && brakingPanel != BrakingPanel.CLOSED
    // The map layer sheet, from the rail; the layer is painted for the theme shown.
    var layersOpen by rememberSaveable { mutableStateOf(false) }
    // The map's playback control: a small pill, or opened out into the full bar.
    var playbackExpanded by rememberSaveable { mutableStateOf(false) }
    val darkTheme = MaterialTheme.colorScheme.surface.luminance() < 0.5f
    val mapLayer = if (config.view == AnalysisView.GRAPH) MapLayer.NONE else config.mapLayer

    // Fetch the shown laps, and ask again on a slow tick if a reply went missing.
    LaunchedEffect(
        active, catalog, currentKey, comparisonKey, channels, sectorDelta, brakingZonesOn,
        mapLayer, darkTheme, config.mapCurrentColor, config.mapComparisonColor,
    ) {
        if (!active) return@LaunchedEffect
        while (true) {
            analysis.syncDriverLaps(catalog, listOf(currentDriver, comparisonDriver))
            analysis.sync(catalog, listOf(currentKey, comparisonKey), channels)
            analysis.syncDelta(catalog, currentKey, comparisonKey, sectorDelta)
            analysis.syncBraking(catalog, currentKey, comparisonKey, brakingZonesOn)
            analysis.syncMapLayer(
                catalog, mapLayer, currentKey, comparisonKey,
                darkTheme, config.mapCurrentColor, config.mapComparisonColor,
            )
            delay(LAP_SYNC_INTERVAL_MS)
        }
    }

    // The recording's drivers, for the lap sheets and the lap bar, while each
    // lap can have its own. Read through derived state: the timing tower's
    // cars change several times a second.
    val roster by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.timing.drivers } }
    val rosterPlayer by remember(store) { derivedStateOf { store.timing.playerIndex } }
    val driverChoice = analysis.driverChoiceAvailable(catalog)
    val sheetDrivers = remember(roster, rosterPlayer, driverChoice, catalog.driverIndex) {
        if (!driverChoice) {
            emptyList()
        } else {
            // During playback the recording's player is the one car always readable.
            val player = if (rosterPlayer >= 0) rosterPlayer else catalog.driverIndex
            roster.values
                .filter { it.name.isNotBlank() }
                .sortedWith(compareBy({ it.raceNumber }, { it.index }))
                .map { driver ->
                    LapSheetDriver(
                        index = driver.index,
                        name = driver.name,
                        raceNumber = driver.raceNumber,
                        teamColor = parseColor(driver.teamColor)?.let { Color(it) } ?: Color.Gray,
                        restricted = driver.index != player && driver.yourTelemetry?.let { it != 1 } == true,
                    )
                }
        }
    }

    val current = analysis.laps[currentKey]
    val comparison = analysis.laps[comparisonKey]
    val currentInfo = analysis.lapsOf(catalog, currentDriver)?.firstOrNull { it.lapNumber == currentLapNumber }
    val comparisonInfo = analysis.lapsOf(catalog, comparisonDriver)?.firstOrNull { it.lapNumber == comparisonLapNumber }
    val comparisonSelected = currentLapNumber > 0 && comparisonLapNumber > 0
    val deltaSeries = config.deltaSeries
    val showDelta = deltaSeries?.visible == true && distanceMode && comparisonSelected
    val delta = analysis.deltaCurveFor(currentKey, comparisonKey, sectorDelta)
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
    LaunchedEffect(fixed, currentKey, comparisonKey) {
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
    // Back leaves a zone for the list, then the list for the map.
    BackHandler(enabled = brakingShown && !(lapsExpanded && !landscape)) {
        brakingPanel = if (brakingPanel == BrakingPanel.ZONE) BrakingPanel.LIST else BrakingPanel.CLOSED
    }
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
        val first = currentKey
        val second = comparisonKey
        analysis.selectLapA(second.lap, second.driver)
        analysis.selectLapB(first.lap, first.driver)
        analysis.useFixedLaps(true)
    }
    val lapDelta = if (comparisonSelected && delta != null) summary else null
    // The bar names each lap's driver while there is a choice of them.
    fun slotDriver(driver: Int): String? {
        val index = if (driver < 0) catalog.driverIndex else driver
        return sheetDrivers.firstOrNull { it.index == index }?.let { driverCode(it.name) }
    }
    val slotA = LapSlot(
        lap = currentLapNumber,
        color = currentColor,
        driver = slotDriver(currentDriver),
        time = currentInfo?.let { formatLapTimeMs(it.lapTimeMs) },
        fastest = false,
    )
    val slotB = LapSlot(
        lap = comparisonLapNumber,
        color = comparisonColor,
        driver = slotDriver(comparisonDriver),
        time = comparisonInfo?.let { formatLapTimeMs(it.lapTimeMs) },
        fastest = comparisonLapNumber > 0 && comparisonLapNumber == analysis.fastestLapOf(catalog, comparisonDriver),
    )
    val lapRow: @Composable (Boolean) -> Unit = { stacked ->
        LapSelectorRow(
            fixed = fixed,
            lapA = slotA,
            lapB = slotB,
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
                    fixed = fixed,
                    lapA = slotA,
                    lapB = slotB,
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
                    // The braking zones of the laps on show, marked on the Map view's map,
                    // named as the label settings name them everywhere else.
                    val brakingLaps = BrakingLaps(
                        currentLabel = primaryLabel,
                        currentColor = currentColor,
                        comparisonLabel = comparisonLabel,
                        comparisonColor = comparisonColor,
                    )
                    val brakingZones = if (brakingZonesOn) analysis.brakingZonesFor(currentKey, comparisonKey) else null
                    val zoneRows = brakingZones?.zones.orEmpty()
                    val zoneIndex = brakingZoneIndex.coerceIn(0, max(0, zoneRows.lastIndex))
                    val neutralZone = MaterialTheme.colorScheme.onSurfaceVariant
                    val zoneMarks = zoneRows.map { MapZoneMark(it.number, it.fromM, brakingZoneColor(it, brakingLaps, neutralZone)) }
                    val zoneOpen = brakingPanel == BrakingPanel.ZONE && zoneRows.isNotEmpty()
                    val openZone: (Int) -> Unit = { index ->
                        brakingZoneIndex = index
                        brakingPanel = BrakingPanel.ZONE
                    }
                    // The map layer, painted by libtnrp for the laps, theme and lap colours shown.
                    val layerPaint = if (mapLayer.native != null) {
                        analysis.mapLayerFor(
                            mapLayer, currentKey, comparisonKey,
                            darkTheme, config.mapCurrentColor, config.mapComparisonColor,
                        )
                    } else {
                        null
                    }
                    val currentName = MapLapName(if (currentLapNumber > 0) "L$currentLapNumber" else primaryLabel, currentColor)
                    val comparisonName = MapLapName(if (comparisonLapNumber > 0) "L$comparisonLapNumber" else comparisonLabel, comparisonColor)
                    // A per-lap layer with two laps offers the lap strip; alone, it shows the one lap.
                    val lapChoice = mapLayer.perLap && comparisonSelected
                    val lapView = if (lapChoice) config.mapLapView else MapLapView.CURRENT
                    val lapPaint = when (lapView) {
                        MapLapView.COMPARISON -> layerPaint?.comparison
                        else -> layerPaint?.current
                    }
                    val layerMessage = when {
                        mapLayer.native == null -> null
                        analysis.mapLayerUnavailable -> "Update Track N Race on the desktop to see map layers"
                        lapPaint?.restricted == true -> "No ERS data: this car's telemetry is Restricted"
                        mapLayer == MapLayer.DOMINANCE && !comparisonSelected -> "Choose a comparison lap"
                        else -> null
                    }
                    // [braking]: the Map view, whose map carries the braking zones.
                    val map: @Composable (Modifier, Boolean) -> Unit = { modifier, braking ->
                        // The maps' zoom: shared, so the two maps of the Both view zoom together and the rail can reset it.
                        val mapZoom = remember { MapZoomControl() }
                        // One map: its laps, its paint, the Speed layer's corner speeds, and whether it carries the zones.
                        val layerMap: @Composable (Modifier, List<MapLap>, MapTrackPaint?, List<MapApex>, Boolean) -> Unit =
                            { mapModifier, lapsShown, paint, apexes, zones ->
                            AnalysisMap(
                                trackId,
                                lapsShown,
                                mapElapsed,
                                aeroMode,
                                mapModifier,
                                focusSector = stripSelection?.takeIf { it != WHOLE_LAP },
                                slmTrackStatus = slmTrackStatus,
                                zoneMarks = if (zones) zoneMarks else emptyList(),
                                selectedZone = if (zones && zoneOpen) zoneIndex else -1,
                                onZone = openZone,
                                trackPaint = paint,
                                apexes = apexes,
                                // Dragging a car moves through the lap, as reading the graph does.
                                onScrub = scrub,
                                zoomControl = mapZoom,
                            )
                        }
                        Box(modifier) {
                            val single: MapTrackPaint? = when (mapLayer) {
                                MapLayer.NONE -> null
                                MapLayer.RACING_LINE -> MapTrackPaint.RacingLine
                                else -> MapTrackPaint.Layer(lapPaint)
                            }
                            when (lapView) {
                                MapLapView.BOTH -> BoxWithConstraints(Modifier.fillMaxSize()) {
                                    // Each lap on a map of its own: stacked on a tall pane, side by side on a wide one.
                                    val tall = maxHeight > maxWidth
                                    val first = mapLaps.firstOrNull()
                                    val second = mapLaps.getOrNull(1)
                                    val firstMap: @Composable (Modifier) -> Unit = { half ->
                                        Box(half) {
                                            layerMap(
                                                Modifier.fillMaxSize(), listOfNotNull(first),
                                                MapTrackPaint.Layer(layerPaint?.current), emptyList(), braking,
                                            )
                                            MapLapTag(currentName, Modifier.align(Alignment.TopStart).padding(12.dp))
                                        }
                                    }
                                    val secondMap: @Composable (Modifier) -> Unit = { half ->
                                        Box(half) {
                                            layerMap(
                                                Modifier.fillMaxSize(), listOfNotNull(second),
                                                MapTrackPaint.Layer(layerPaint?.comparison), emptyList(), false,
                                            )
                                            MapLapTag(comparisonName, Modifier.align(Alignment.TopStart).padding(12.dp))
                                        }
                                    }
                                    if (tall) {
                                        Column(Modifier.fillMaxSize()) {
                                            firstMap(Modifier.fillMaxWidth().weight(1f))
                                            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                                            secondMap(Modifier.fillMaxWidth().weight(1f))
                                        }
                                    } else {
                                        Row(Modifier.fillMaxSize()) {
                                            firstMap(Modifier.fillMaxHeight().weight(1f))
                                            VerticalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                                            secondMap(Modifier.fillMaxHeight().weight(1f))
                                        }
                                    }
                                }
                                MapLapView.COMBINED -> layerMap(
                                    Modifier.fillMaxSize(), mapLaps,
                                    MapTrackPaint.Bands(layerPaint?.current, layerPaint?.comparison), emptyList(), braking,
                                )
                                else -> layerMap(Modifier.fillMaxSize(), mapLaps, single, lapPaint?.apexes.orEmpty(), braking)
                            }
                            // Fixed corners over the map: the rail top right, the legend top left
                            // (below the first map's lap name in the Both view) with the layer's
                            // status under it, and the playback control bottom right with the
                            // zoom and back-to-live chips above it.
                            MapToolRail(
                                onLayers = { layersOpen = true },
                                layersOpen = layersOpen,
                                brakingOn = if (braking) brakingOn else null,
                                onBraking = {
                                    if (brakingOn) {
                                        brakingOn = false
                                        brakingPanel = BrakingPanel.CLOSED
                                    } else {
                                        brakingOn = true
                                        brakingPanel = BrakingPanel.LIST
                                    }
                                },
                                zoomed = mapZoom.zoomed,
                                onResetZoom = mapZoom::reset,
                                modifier = Modifier.align(Alignment.TopEnd).padding(8.dp),
                            )
                            Column(
                                Modifier
                                    .align(Alignment.TopStart)
                                    .padding(start = 8.dp, end = 8.dp, top = if (lapView == MapLapView.BOTH) 40.dp else 8.dp),
                                verticalArrangement = Arrangement.spacedBy(8.dp),
                            ) {
                                MapLayerLegend(
                                    layer = mapLayer,
                                    paint = layerPaint,
                                    dark = darkTheme,
                                    compact = false,
                                )
                                MapLayerStatus(
                                    loading = mapLayer.native != null && analysis.mapLayerLoading,
                                    message = layerMessage,
                                )
                            }
                            Column(
                                Modifier.align(Alignment.BottomEnd).padding(8.dp),
                                horizontalAlignment = Alignment.End,
                                verticalArrangement = Arrangement.spacedBy(8.dp),
                            ) {
                                chips(Modifier)
                                MapPlaybackControl(
                                    clock = if (fixed) clock else null,
                                    elapsed = mapElapsed,
                                    total = mapTotal,
                                    sectorMarks = current?.sectorSplits?.map { it.elapsedSeconds }.orEmpty(),
                                    expanded = playbackExpanded,
                                    onExpand = { playbackExpanded = it },
                                    lapChoice = if (lapChoice) {
                                        MapLapChoice(
                                            view = config.mapLapView,
                                            onView = { view -> analysis.updateConfig { it.copy(mapLapView = view) } },
                                            current = currentName,
                                            comparison = comparisonName,
                                        )
                                    } else {
                                        null
                                    },
                                )
                            }
                        }
                    }
                    // The zone list, or one zone's head to head: below the map
                    // in portrait, beside it in landscape.
                    val brakingDrawer: @Composable (Modifier) -> Unit = { modifier ->
                        if (zoneOpen) {
                            BrakingZoneDrawer(
                                zone = zoneRows[zoneIndex],
                                index = zoneIndex,
                                count = zoneRows.size,
                                laps = brakingLaps,
                                docked = landscape,
                                onStep = { brakingZoneIndex = it.coerceIn(0, zoneRows.lastIndex) },
                                onBack = { brakingPanel = BrakingPanel.LIST },
                                modifier = modifier,
                            )
                        } else {
                            BrakingZoneList(
                                zones = brakingZones,
                                loading = analysis.brakingLoading,
                                unavailable = analysis.brakingUnavailable,
                                laps = brakingLaps,
                                docked = landscape,
                                onZone = openZone,
                                onClose = { brakingPanel = BrakingPanel.CLOSED },
                                modifier = modifier,
                            )
                        }
                    }
                    when (config.view) {
                        AnalysisView.GRAPH -> chart(Modifier.fillMaxSize())
                        // The map fills the view, and refits beside or above
                        // whichever braking drawer is open.
                        AnalysisView.MAP -> if (landscape) {
                            Row(Modifier.fillMaxSize()) {
                                map(Modifier.weight(1f).fillMaxHeight(), true)
                                if (brakingShown) {
                                    VerticalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                                    brakingDrawer(Modifier.width(300.dp).fillMaxHeight())
                                }
                            }
                        } else {
                            Column(Modifier.fillMaxSize()) {
                                map(Modifier.fillMaxWidth().weight(if (brakingShown) 0.45f else 1f), true)
                                if (brakingShown) brakingDrawer(Modifier.fillMaxWidth().weight(0.55f))
                            }
                        }
                        AnalysisView.SPLIT -> if (landscape) {
                            Row(Modifier.fillMaxSize()) {
                                chart(Modifier.weight(1f).fillMaxHeight())
                                VerticalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                                map(Modifier.weight(1f).fillMaxHeight(), false)
                            }
                        } else {
                            Column(Modifier.fillMaxSize()) {
                                map(Modifier.fillMaxWidth().weight(0.85f), false)
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
            drivers = sheetDrivers,
            onDismiss = { lapPicker = null },
        )
    }
    if (layersOpen) {
        MapLayerSheet(
            selected = config.mapLayer,
            onSelect = { layer -> analysis.updateConfig { it.copy(mapLayer = layer) } },
            onDismiss = { layersOpen = false },
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
 * What the lap bar shows of one compared lap: its number (0 = none), colour
 * and time, its driver's short name while each lap can have its own, and
 * whether it is that driver's fastest.
 */
private class LapSlot(
    val lap: Int,
    val color: Color,
    val driver: String?,
    val time: String?,
    val fastest: Boolean,
)

/** A driver's name as three letters, as timing screens abbreviate it: "NOR" for "Lando NORRIS". */
private fun driverCode(name: String): String =
    name.trim().substringAfterLast(' ').take(3).uppercase(Locale.ROOT)

/**
 * The compared laps, one button each: its colour, driver, number, role and
 * time. Each opens that lap's sheet; between them, a swap fixes the two laps
 * the other way round. [stacked] puts them one above the other, for the
 * landscape column.
 */
@Composable
private fun LapSelectorRow(
    fixed: Boolean,
    lapA: LapSlot,
    lapB: LapSlot,
    stacked: Boolean,
    onPick: (LapTarget) -> Unit,
    onSwap: () -> Unit,
) {
    val first: @Composable (Modifier) -> Unit = { modifier ->
        LapButton(
            slot = lapA,
            placeholder = if (fixed) "Lap A" else "Waiting",
            role = if (fixed) "fixed" else "following",
            roleColor = null,
            onClick = { onPick(LapTarget.LAP_A) },
            modifier = modifier,
        )
    }
    val second: @Composable (Modifier) -> Unit = { modifier ->
        LapButton(
            slot = lapB,
            placeholder = if (fixed) "Lap B" else "Compare",
            role = when {
                lapB.lap <= 0 -> "none chosen"
                lapB.fastest -> "fastest"
                else -> null
            },
            roleColor = if (lapB.fastest) FastestLapColor else null,
            onClick = { onPick(LapTarget.LAP_B) },
            modifier = modifier,
        )
    }
    val swap: @Composable () -> Unit = {
        IconButton(onClick = onSwap, enabled = lapA.lap > 0 && lapB.lap > 0) {
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
 * colour, driver, number and time. Tapping it opens [LapSelectorRow] over the
 * page.
 */
@Composable
private fun LapSummaryBar(
    fixed: Boolean,
    lapA: LapSlot,
    lapB: LapSlot,
    onExpand: () -> Unit,
) {
    val colors = MaterialTheme.colorScheme
    val timeStyle = MaterialTheme.typography.labelMedium.copy(fontFeatureSettings = "tnum")
    // Each lap fills half the strip, under its button in the open row.
    val lap: @Composable RowScope.(LapSlot, String) -> Unit = { slot, placeholder ->
        Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(width = 3.dp, height = 14.dp).background(if (slot.lap > 0) slot.color else slot.color.copy(alpha = 0.35f), RoundedCornerShape(1.5.dp)))
            if (slot.lap > 0 && slot.driver != null) {
                Text(slot.driver, modifier = Modifier.padding(start = 6.dp), style = MaterialTheme.typography.labelMedium, color = colors.onSurfaceVariant, maxLines = 1)
            }
            Text(
                if (slot.lap > 0) "L${slot.lap}" else placeholder,
                modifier = Modifier.padding(start = 6.dp),
                style = MaterialTheme.typography.labelLarge,
                maxLines = 1,
            )
            if (slot.lap > 0 && slot.time != null) {
                Text(slot.time, modifier = Modifier.padding(start = 6.dp), style = timeStyle, color = colors.onSurfaceVariant, maxLines = 1)
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
        lap(lapA, if (fixed) "Lap A" else "Waiting")
        // As wide as the open row's swap button, so the halves line up.
        Text("vs", modifier = Modifier.width(48.dp), style = timeStyle, color = colors.onSurfaceVariant, textAlign = TextAlign.Center)
        lap(lapB, if (fixed) "Lap B" else "Compare")
        Icon(
            painterResource(R.drawable.ic_expand_more),
            contentDescription = null,
            modifier = Modifier.size(20.dp),
            tint = colors.onSurfaceVariant,
        )
    }
}

@Composable
private fun LapButton(
    slot: LapSlot,
    placeholder: String,
    role: String?,
    roleColor: Color?,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val lap = slot.lap
    val color = slot.color
    val time = slot.time
    val title = buildAnnotatedString {
        if (lap > 0) {
            if (slot.driver != null) {
                withStyle(SpanStyle(color = colors.onSurfaceVariant)) { append(slot.driver) }
                append(" · ")
            }
            append("Lap $lap")
        } else {
            append(placeholder)
        }
    }
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
                title,
                style = MaterialTheme.typography.titleMedium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
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
