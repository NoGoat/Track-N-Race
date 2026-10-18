package com.tracknrace.android.pages

import android.content.res.Configuration
import android.graphics.Paint
import android.graphics.RectF
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
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
import androidx.compose.foundation.layout.wrapContentHeight
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.VerticalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.PrimaryScrollableTabRow
import androidx.compose.material3.PrimaryTabRow
import androidx.compose.material3.Tab
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.LayoutCoordinates
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import com.tracknrace.android.GraphWindow
import com.tracknrace.android.GraphsController
import com.tracknrace.android.PlaybackCatalog
import com.tracknrace.android.LapTrace
import com.tracknrace.android.SampleSeries
import com.tracknrace.android.TrendData
import com.tracknrace.android.TrendLap
import com.tracknrace.android.R
import com.tracknrace.android.formatLapTimeMs
import com.tracknrace.android.PairedTelemetryPage
import com.tracknrace.android.GraphFamily
import com.tracknrace.android.GraphField
import com.tracknrace.android.SampleHistory
import com.tracknrace.android.TelemetryStore
import java.util.Locale
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.log10
import kotlin.math.pow
import kotlin.math.max
import kotlin.math.min
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.conflate
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.job
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.launch

// Live status arrives about twice a second; the window keeps moving between
// rows, but never further than this past the newest one.
private const val LIVE_LEAD_SECONDS = 1.0
// A pause between samples longer than this is a gap, not a line.
private const val GAP_SECONDS = 2.5
private const val BACKFILL_TICK_MS = 250L
// A tab's padding either side of its name, as Material's tabs.
private const val TAB_PADDING_DP = 16
private const val Y_TICKS = 5
private const val Y_GUTTER_DP = 46f
// One right-hand axis's labels, as the desktop's 50 px offset.
private const val RIGHT_AXIS_DP = 38f
private const val X_AXIS_DP = 18f
private const val PLOT_PAD_DP = 6f
private const val MIN_X_LABEL_SPACING_DP = 64f
private val X_STEPS = doubleArrayOf(1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0)
// The window's right edge moves in steps of this share of the window: under a
// pixel on any phone, so a long window redraws only when its lines would move.
private const val RIGHT_EDGE_STEPS = 1200.0
// The current lap is drawn up to the cursor in steps of this many seconds.
private const val LAP_CUTOFF_STEP_SECONDS = 0.05
// A span's bitmap covers this much more than the span when drawn, and is
// drawn again once the span outgrows it.
private const val SPAN_HEADROOM = 1.25
private const val SPAN_BASE_SECONDS = 60.0
// New samples are added to a chart's bitmap at most this often.
private const val RASTER_INTERVAL_MS = 50L
// Filtered, so a raster stretched between redraws stays smooth.
private val RASTER_PAINT = Paint(Paint.FILTER_BITMAP_FLAG)
// A round tick nearer a range end than this share of a step is dropped for the end's own.
private const val END_TICK_ROOM = 0.4
// Lap labels on a per-lap axis are at least this far apart.
private const val MIN_LAP_LABEL_SPACING_DP = 36f
// A per-lap line's dot, as the desktop's 8 px.
private const val LAP_DOT_RADIUS_DP = 3f
// Lap numbers sit just right of their lap's start, as on the desktop.
private const val LAP_LABEL_OFFSET_DP = 4f
private const val LAP_LABEL_GAP_DP = 6f
private val DISTANCE_STEPS = doubleArrayOf(100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0)

/** The charts' ids: the desktop's graph sections, which key their windows. */
private val POWER_CHART_IDS = listOf("powerSplit", "powerHarvest", "powerStore", "powerFuel")
private val INPUT_CHART_IDS = listOf("inputGear", "inputThrottleBrake", "inputSteering")
private val SPEED_CHART_IDS = listOf("overviewTelemetry")
private val TYRE_CHART_IDS = listOf("tyreSurface", "tyreInner", "tyreBrake", "tyreWear")
private val MISC_CHART_IDS = listOf("miscGForce", "miscRideHeight")
private val TREND_CHART_IDS = listOf("trendsLapTime", "trendsErsUsage", "trendsRecharge", "stintTyreRange", "trendsCombinedRange")

// Trends charts show the current stint or every lap, as on the desktop.
private val TREND_WINDOWS = listOf(GraphWindow.STINT, GraphWindow.ALL)

/** Which tab, the row family its charts draw, and what it asks the desktop for: live, a lap mode also needs the lap rows. */
private enum class GraphsTab(
    val title: String,
    val chartIds: List<String>,
    val page: PairedTelemetryPage,
    val lapPage: PairedTelemetryPage,
) {
    POWER("Power", POWER_CHART_IDS, PairedTelemetryPage.GRAPHS_POWER, PairedTelemetryPage.GRAPHS_POWER_LAPS),
    INPUTS("Inputs", INPUT_CHART_IDS, PairedTelemetryPage.GRAPHS_INPUTS, PairedTelemetryPage.GRAPHS_INPUTS_LAPS),
    SPEED("Speed", SPEED_CHART_IDS, PairedTelemetryPage.GRAPHS_SPEED, PairedTelemetryPage.GRAPHS_SPEED_LAPS),
    TYRES("Tyres", TYRE_CHART_IDS, PairedTelemetryPage.GRAPHS_TYRES, PairedTelemetryPage.GRAPHS_TYRES_LAPS),
    MISC("Misc", MISC_CHART_IDS, PairedTelemetryPage.GRAPHS_MISC, PairedTelemetryPage.GRAPHS_MISC_LAPS),
    TRENDS("Trends", TREND_CHART_IDS, PairedTelemetryPage.GRAPHS_TRENDS, PairedTelemetryPage.GRAPHS_TRENDS),
}

@Composable
internal fun GraphsScreen(
    store: TelemetryStore,
    graphs: GraphsController,
    active: Boolean = true,
    /** Called with the stream the visible tab needs. */
    onPage: (PairedTelemetryPage) -> Unit = {},
) {
    var saved by rememberSaveable { mutableIntStateOf(0) }
    val pager = rememberPagerState(initialPage = saved) { GraphsTab.entries.size }
    val scope = rememberCoroutineScope()
    val current = pager.currentPage
    val lapMode = GraphsTab.entries[current].chartIds.any { graphs.windowOf(it).isLap }
    LaunchedEffect(current, active, lapMode) {
        saved = current
        val tab = GraphsTab.entries[current]
        if (active) onPage(if (lapMode) tab.lapPage else tab.page)
    }
    Column(Modifier.fillMaxSize()) {
        // Primary tabs continuing the app bar, as on Dashboard: spread across
        // the width when every name fits its share, scrolling when not.
        val tabs: @Composable () -> Unit = {
            GraphsTab.entries.forEachIndexed { index, tab ->
                Tab(
                    selected = current == index,
                    onClick = { scope.launch { pager.animateScrollToPage(index) } },
                    text = { Text(tab.title, maxLines = 1) },
                    selectedContentColor = MaterialTheme.colorScheme.primary,
                    unselectedContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        val measurer = rememberTextMeasurer()
        val tabStyle = MaterialTheme.typography.titleSmall
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val widest = remember(tabStyle, measurer) {
                GraphsTab.entries.maxOf { measurer.measure(it.title, tabStyle).size.width }
            }
            val share = constraints.maxWidth / GraphsTab.entries.size
            val fits = widest + with(LocalDensity.current) { (TAB_PADDING_DP * 2).dp.roundToPx() } <= share
            if (fits) {
                PrimaryTabRow(
                    selectedTabIndex = current,
                    modifier = Modifier.staticLayer(),
                    containerColor = MaterialTheme.colorScheme.surfaceContainer,
                    divider = {},
                    tabs = tabs,
                )
            } else {
                PrimaryScrollableTabRow(
                    selectedTabIndex = current,
                    modifier = Modifier.staticLayer(),
                    containerColor = MaterialTheme.colorScheme.surfaceContainer,
                    edgePadding = 0.dp,
                    divider = {},
                    tabs = tabs,
                )
            }
        }
        HorizontalPager(pager, Modifier.fillMaxWidth().weight(1f), beyondViewportPageCount = 0) { page ->
            when (GraphsTab.entries[page]) {
                GraphsTab.POWER -> PowerTab(store, graphs, active && current == page)
                GraphsTab.INPUTS -> InputsTab(store, graphs, active && current == page)
                GraphsTab.SPEED -> SpeedTab(store, graphs, active && current == page)
                GraphsTab.TYRES -> TyresTab(store, graphs, active && current == page)
                GraphsTab.MISC -> MiscTab(store, graphs, active && current == page)
                GraphsTab.TRENDS -> TrendsTab(store, graphs, active && current == page)
            }
        }
    }
}

// ── Power ───────────────────────────────────────────────────────────────────

/** The window's right edge and the inspected time, shared by every chart on a tab. */
@Stable
private class GraphClock {
    /** Session time at the right edge; NaN before any data. */
    var right by mutableDoubleStateOf(Double.NaN)

    /** Session time under the finger on a time chart, or null. */
    var inspect by mutableStateOf<Double?>(null)

    /** Lap distance under the finger on a lap chart, or null. */
    var inspectDistance by mutableStateOf<Double?>(null)

    fun follow(store: TelemetryStore, history: SampleHistory, nowNanos: Long) {
        // Held still while a finger reads the values.
        if (inspect != null || inspectDistance != null) return
        val cursor = store.latestPlaybackCursorTime()
        val last = history.lastTime
        val target = when {
            cursor.isFinite() -> cursor
            last.isFinite() -> last + ((nowNanos - history.lastAppendNanos) / 1e9).coerceIn(0.0, LIVE_LEAD_SECONDS)
            else -> Double.NaN
        }
        if (target != right && !(target.isNaN() && right.isNaN())) right = target
    }
}

/**
 * Every chart's header values on a tab, painted by one layer over the whole
 * tab: a new sample repaints that layer once, not one layer per chart. Each
 * chart's header keeps an empty slot where its values go. Main thread only.
 */
@Stable
private class ReadoutBoard {
    // [owner] is the header holding the slot: when a layout change moves a
    // chart, its new header registers before the old one leaves, and only the
    // owner may take the slot away.
    private class Slot(var owner: Any, var runs: () -> List<ReadoutRun>, var place: LayoutCoordinates? = null)

    private val slots = LinkedHashMap<String, Slot>()
    private var root: LayoutCoordinates? = null

    /** Bumped when a slot moves or comes and goes, so the board repaints where it now is. */
    var layout by mutableIntStateOf(0)
        private set

    fun register(id: String, owner: Any, runs: () -> List<ReadoutRun>) {
        val slot = slots[id]
        if (slot == null) {
            slots[id] = Slot(owner, runs)
        } else {
            if (slot.owner !== owner) slot.place = null
            slot.owner = owner
            slot.runs = runs
        }
    }

    fun unregister(id: String, owner: Any) {
        if (slots[id]?.owner !== owner) return
        slots.remove(id)
        layout++
    }

    fun place(id: String, owner: Any, coordinates: LayoutCoordinates) {
        val slot = slots[id]?.takeIf { it.owner === owner } ?: return
        slot.place = coordinates
        layout++
    }

    fun placeRoot(coordinates: LayoutCoordinates) {
        root = coordinates
        layout++
    }

    fun DrawScope.paint(paints: Pair<Paint, Paint>, sizePx: Float) {
        layout
        val board = root?.takeIf { it.isAttached } ?: return
        for (slot in slots.values) {
            val place = slot.place?.takeIf { it.isAttached } ?: continue
            paintReadout(slot.runs(), paints, sizePx, board.localBoundingBoxOf(place, clipBounds = false), alignEnd = true)
        }
    }
}

/**
 * The beat every chart on a tab draws its bitmap to, so their new bitmaps
 * go on screen in the same frame rather than four frames apart. A round's
 * results are shown together once its last render is done; a render that
 * runs past the next beat is shown on its own when it finishes. Main thread only.
 */
@Stable
private class RasterBeat {
    private val beats = MutableStateFlow(0L)
    private var round = 0L
    private var outstanding = 0
    private val pending = ArrayList<() -> Unit>()

    /** Waits for the beat after [after], and joins its round. */
    suspend fun next(after: Long): Long {
        val beat = beats.first { it > after }
        outstanding++
        return beat
    }

    /** A render of round [beat] finished: [show] puts its result on screen. */
    fun done(beat: Long, show: () -> Unit) {
        pending += show
        if (beat == round) {
            outstanding--
            if (outstanding > 0) return
        }
        flush()
    }

    suspend fun run() {
        while (true) {
            delay(RASTER_INTERVAL_MS)
            flush()
            outstanding = 0
            round++
            beats.value = round
        }
    }

    private fun flush() {
        if (pending.isEmpty()) return
        // One batch of state writes: the next frame draws them all.
        pending.forEach { it() }
        pending.clear()
    }
}

private class SeriesSpec(
    val label: String,
    val field: GraphField,
    val color: Color,
    val scale: Float = 1f,
    val style: LineStyle = LineStyle(),
)

/** A guide line across the plot at [value], as the desktop's reference lines. */
private class RefLine(val value: Double, val dashed: Boolean)

/** Another set of Y labels on the right of the plot, for a series on its own scale. */
private class RightAxis(val color: Color, val format: (Double) -> String)

private class ChartSpec(
    /** Names the chart on its tab. */
    val id: String,
    val family: GraphFamily,
    val title: String,
    val series: List<SeriesSpec>,
    /** The y range for the samples in view; [visibleMax] is NaN without any. */
    val range: (visibleMax: Double, fuelPeak: Float) -> Pair<Double, Double>,
    val tick: (Double) -> String,
    /** The y labels and grid lines for the range shown; null for five even steps of it. */
    val ticks: ((min: Double, max: Double) -> List<Double>)? = null,
    val refLines: List<RefLine> = emptyList(),
    /** Colour of the left Y labels, when they belong to one series. */
    val axisColor: Color? = null,
    /** Labels of the same ticks on the right, one column per axis. */
    val rightAxes: List<RightAxis> = emptyList(),
    /** Room the header keeps for the values before wrapping onto a second line. */
    val readoutWidthDp: Float = 120f,
    /** The header values for each series' value at the read point, NaN where unknown. */
    val readout: (values: FloatArray, shown: BooleanArray, dim: Color) -> List<ReadoutRun>,
    /** The desktop's graph section whose window and reference lap this chart follows; charts split from one share it. */
    val windowId: String = id,
    /** Width of each right-hand label column. */
    val rightAxisDp: Float = RIGHT_AXIS_DP,
    /** The only windows on offer, the first by default, apart from the global window; null for all. */
    val windowChoices: List<GraphWindow>? = null,
    /** Per-lap lines over the samples, each on its own axis: the Trends combined chart. */
    val lapOverlay: LapOverlay? = null,
)

/** One per-lap line: a value of each lap, on overlay axis [axis]. */
private class OverlaySeries(val label: String, val color: Color, val axis: Int, val value: (TrendLap) -> Double?)

/** One overlay axis: its range for the laps shown, and how it labels a value. */
private class OverlayAxis(val color: Color?, val format: (Double) -> String, val range: (List<TrendLap>) -> Pair<Double, Double>)

/**
 * Per-lap lines drawn over a span chart's samples at each lap's end, as the
 * desktop's Trends combined chart: every line is drawn on the chart's 0-1
 * scale, and each axis labels the same quarter lines in its own units. Axis 0
 * is the left axis; the chart's own samples use the last.
 */
private class LapOverlay(
    val data: State<TrendData>,
    val series: List<OverlaySeries>,
    val axes: List<OverlayAxis>,
    val readout: (TrendLap, Color) -> List<ReadoutRun>,
)

// The desktop's Power colours, with its light-theme variants.
private fun seriesColor(dark: Long, light: Long, isDark: Boolean) = Color(if (isDark) dark else light)

private fun powerCharts(isDark: Boolean, hasMguh: Boolean, harvestLimit: Double): List<ChartSpec> {
    val ice = seriesColor(0xff5794f2, 0xff0b57d0, isDark)
    val mguk = seriesColor(0xfffade2a, 0xff765900, isDark)
    val harvestK = seriesColor(0xff37872d, 0xff0d6b2f, isDark)
    val harvestH = Color(0xffc4162a)
    val fuel = seriesColor(0xfff0a500, 0xffa04300, isDark)
    fun whole(value: Float) = if (value.isFinite()) "%.0f".format(Locale.US, value) else "—"
    return listOf(
        ChartSpec(
            id = POWER_CHART_IDS[0],
            family = GraphFamily.STATUS,
            title = "Power",
            series = listOf(
                SeriesSpec("ICE", GraphField.ICE_KW, ice),
                SeriesSpec("MGU-K", GraphField.MGUK_KW, mguk),
            ),
            range = { visibleMax, _ -> expand(0.0, 500.0, visibleMax) },
            tick = { "${it.roundToLong()}kW" },
            readout = { values, shown, dim ->
                buildList {
                    if (shown[0]) add(ReadoutRun(whole(values[0]), ice))
                    if (shown[0] && shown[1]) add(ReadoutRun(" + ", dim, bold = false))
                    if (shown[1]) add(ReadoutRun(whole(values[1]), mguk))
                    val total = values[0].orZero() + values[1].orZero()
                    if (shown[0] && shown[1]) add(ReadoutRun(" = ${whole(total)}", dim, bold = false))
                    add(ReadoutRun(" kW", dim, bold = false))
                }
            },
        ),
        ChartSpec(
            id = POWER_CHART_IDS[1],
            family = GraphFamily.STATUS,
            title = "ERS Harvest",
            series = listOfNotNull(
                SeriesSpec("MGU-K", GraphField.HARVEST_MGUK_J, harvestK, scale = 0.001f),
                SeriesSpec("MGU-H", GraphField.HARVEST_MGUH_J, harvestH, scale = 0.001f).takeIf { hasMguh },
            ),
            range = { visibleMax, _ -> expand(0.0, harvestLimit, visibleMax) },
            tick = { "${it.roundToLong()}kJ" },
            readout = { values, shown, dim ->
                buildList {
                    if (shown[0]) add(ReadoutRun(whole(values[0]), harvestK))
                    if (values.size > 1 && shown[1]) {
                        if (shown[0]) add(ReadoutRun("  ", dim, bold = false))
                        add(ReadoutRun(whole(values[1]), harvestH))
                    }
                    add(ReadoutRun(" kJ", dim, bold = false))
                }
            },
        ),
        ChartSpec(
            id = POWER_CHART_IDS[2],
            family = GraphFamily.STATUS,
            title = "ERS Store",
            series = listOf(SeriesSpec("ERS", GraphField.ERS_PCT, ice)),
            range = { _, _ -> 0.0 to 100.0 },
            tick = { "${it.roundToLong()}%" },
            readout = { values, _, dim ->
                val pct = values[0]
                listOf(
                    ReadoutRun(if (pct.isFinite()) "%.1f%%".format(Locale.US, pct) else "—", ice),
                    ReadoutRun(if (pct.isFinite()) " · %.2f / 4.00 MJ".format(Locale.US, pct / 100f * 4f) else "", dim, bold = false),
                )
            },
        ),
        ChartSpec(
            id = POWER_CHART_IDS[3],
            family = GraphFamily.STATUS,
            title = "Fuel History",
            series = listOf(SeriesSpec("Fuel", GraphField.FUEL_KG, fuel)),
            // The starting load plus a kilogram, as on the desktop.
            range = { visibleMax, fuelPeak ->
                val peak = fuelPeak.toDouble()
                0.0 to max(1.0, (if (peak.isFinite()) peak else visibleMax.takeIf { it.isFinite() } ?: 0.0) + 1.0)
            },
            tick = { "%.1fkg".format(Locale.US, it) },
            readout = { values, _, dim ->
                val kg = values[0]
                listOf(
                    ReadoutRun(if (kg.isFinite()) "%.2f".format(Locale.US, kg) else "—", fuel),
                    ReadoutRun(" kg", dim, bold = false),
                )
            },
        ),
    )
}

// ── Inputs ──────────────────────────────────────────────────────────────────

private val PEDAL_TICKS = listOf(-1.0, -0.5, 0.0, 0.5, 1.0)

/** The desktop's Inputs charts, in its default layout: Gear, Accelerator / Brake together, Steering. */
private fun inputCharts(isDark: Boolean): List<ChartSpec> {
    val gear = seriesColor(0xff5794f2, 0xff0b57d0, isDark)
    val accelerator = seriesColor(0xff37872d, 0xff0d6b2f, isDark)
    val brake = Color(0xffc4162a)
    val steering = seriesColor(0xffbf5fff, 0xff7c3ba6, isDark)
    fun percent(value: Float) = if (value.isFinite()) "${Math.round(abs(value) * 100)}%" else "—"
    return listOf(
        ChartSpec(
            id = INPUT_CHART_IDS[0],
            family = GraphFamily.TELEMETRY,
            title = "Gear",
            // Held until the next sample, filled from below first gear.
            series = listOf(SeriesSpec("Gear", GraphField.GEAR, gear, style = LineStyle(step = 1f, widthDp = 2f, fillBaseline = 0.5))),
            range = { _, _ -> 0.5 to 8.5 },
            tick = { it.roundToLong().toString() },
            ticks = { _, _ -> (1..8).map { it.toDouble() } },
            readoutWidthDp = 24f,
            refLines = listOf(2.0, 4.0, 6.0).map { RefLine(it, dashed = true) },
            readout = { values, _, _ ->
                listOf(ReadoutRun(if (values[0].isFinite()) Math.round(values[0]).toString() else "—", gear))
            },
        ),
        ChartSpec(
            id = INPUT_CHART_IDS[1],
            family = GraphFamily.TELEMETRY,
            title = "Accelerator / Brake",
            // The accelerator up from the centre line and the brake down from it.
            series = listOf(
                SeriesSpec("Accelerator", GraphField.THROTTLE, accelerator, style = LineStyle(step = 0f, fillBaseline = 0.0)),
                SeriesSpec("Brake", GraphField.BRAKE, brake, scale = -1f, style = LineStyle(step = 0f, fillBaseline = 0.0)),
            ),
            range = { _, _ -> -1.0 to 1.0 },
            tick = { "${Math.round(abs(it) * 100)}%" },
            ticks = { _, _ -> PEDAL_TICKS },
            refLines = listOf(RefLine(0.0, dashed = false)),
            readoutWidthDp = 80f,
            readout = { values, shown, dim ->
                buildList {
                    if (shown[0]) add(ReadoutRun(percent(values[0]), accelerator))
                    if (shown[0] && shown[1]) add(ReadoutRun("  ", dim, bold = false))
                    if (shown[1]) add(ReadoutRun(percent(values[1]), brake))
                }
            },
        ),
        ChartSpec(
            id = INPUT_CHART_IDS[2],
            family = GraphFamily.TELEMETRY,
            title = "Steering",
            series = listOf(SeriesSpec("Steering", GraphField.STEERING, steering)),
            range = { _, _ -> -1.0 to 1.0 },
            tick = ::formatSteeringAxis,
            ticks = { _, _ -> PEDAL_TICKS },
            refLines = listOf(RefLine(0.0, dashed = false)),
            readoutWidthDp = 60f,
            readout = { values, _, _ -> listOf(ReadoutRun(formatSteering(values[0]), steering)) },
        ),
    )
}

// ── Speed ───────────────────────────────────────────────────────────────────

// The desktop draws all three on one 0-1 scale, each labelled on its own axis.
private const val SPEED_MAX = 380f
private const val RPM_MAX = 16000f
private val QUARTER_TICKS = listOf(0.0, 0.25, 0.5, 0.75, 1.0)

/** The desktop's Speed + RPM + ERS chart: one plot, speed labelled left, RPM and ERS right. */
private fun speedCharts(isDark: Boolean): List<ChartSpec> {
    val speed = seriesColor(0xff37872d, 0xff0d6b2f, isDark)
    val rpm = Color(0xffc4162a)
    val ers = seriesColor(0xfffade2a, 0xff765900, isDark)
    fun whole(value: Float) = if (value.isFinite()) "%,d".format(Locale.US, Math.round(value)) else "—"
    return listOf(
        ChartSpec(
            id = SPEED_CHART_IDS[0],
            family = GraphFamily.DRIVE,
            title = "Speed + RPM + ERS",
            series = listOf(
                SeriesSpec("Speed", GraphField.SPEED, speed, scale = 1f / SPEED_MAX),
                SeriesSpec("RPM", GraphField.RPM, rpm, scale = 1f / RPM_MAX),
                SeriesSpec("ERS", GraphField.DRIVE_ERS_PCT, ers, scale = 0.01f),
            ),
            range = { _, _ -> 0.0 to 1.0 },
            tick = { Math.round(it * SPEED_MAX).toString() },
            ticks = { _, _ -> QUARTER_TICKS },
            axisColor = speed,
            rightAxes = listOf(
                RightAxis(rpm) { if (it == 0.0) "0" else "${Math.round(it * RPM_MAX / 1000)}k" },
                RightAxis(ers) { "${Math.round(it * 100)}%" },
            ),
            readoutWidthDp = 170f,
            readout = { values, shown, dim ->
                buildList {
                    if (shown[0]) {
                        add(ReadoutRun(whole(values[0] * SPEED_MAX), speed))
                        add(ReadoutRun(" kph  ", dim, bold = false))
                    }
                    if (shown[1]) {
                        add(ReadoutRun(whole(values[1] * RPM_MAX), rpm))
                        add(ReadoutRun(" rpm  ", dim, bold = false))
                    }
                    if (shown[2]) add(ReadoutRun("${whole(values[2] * 100)}%", ers))
                }
            },
        ),
    )
}

/**
 * Speed, RPM and ERS as three charts, each on its own axis, for a portrait
 * screen too narrow for three sets of labels. They share the combined
 * chart's window, so turning the phone keeps it.
 */
private fun separateSpeedCharts(isDark: Boolean): List<ChartSpec> {
    val speed = seriesColor(0xff37872d, 0xff0d6b2f, isDark)
    val rpm = Color(0xffc4162a)
    val ers = seriesColor(0xfffade2a, 0xff765900, isDark)
    fun whole(value: Float) = if (value.isFinite()) "%,d".format(Locale.US, Math.round(value)) else "—"
    fun quarters(max: Double): (Double, Double) -> List<Double> = { _, _ -> QUARTER_TICKS.map { it * max } }
    return listOf(
        ChartSpec(
            id = "overviewSpeed",
            windowId = SPEED_CHART_IDS[0],
            family = GraphFamily.DRIVE,
            title = "Speed",
            series = listOf(SeriesSpec("Speed", GraphField.SPEED, speed)),
            range = { _, _ -> 0.0 to SPEED_MAX.toDouble() },
            tick = { Math.round(it).toString() },
            ticks = quarters(SPEED_MAX.toDouble()),
            readoutWidthDp = 70f,
            readout = { values, _, dim -> listOf(ReadoutRun(whole(values[0]), speed), ReadoutRun(" kph", dim, bold = false)) },
        ),
        ChartSpec(
            id = "overviewRpm",
            windowId = SPEED_CHART_IDS[0],
            family = GraphFamily.DRIVE,
            title = "RPM",
            series = listOf(SeriesSpec("RPM", GraphField.RPM, rpm)),
            range = { _, _ -> 0.0 to RPM_MAX.toDouble() },
            tick = { if (it == 0.0) "0" else "${Math.round(it / 1000)}k" },
            ticks = quarters(RPM_MAX.toDouble()),
            readoutWidthDp = 80f,
            readout = { values, _, dim -> listOf(ReadoutRun(whole(values[0]), rpm), ReadoutRun(" rpm", dim, bold = false)) },
        ),
        ChartSpec(
            id = "overviewErs",
            windowId = SPEED_CHART_IDS[0],
            family = GraphFamily.DRIVE,
            title = "ERS",
            series = listOf(SeriesSpec("ERS", GraphField.DRIVE_ERS_PCT, ers)),
            range = { _, _ -> 0.0 to 100.0 },
            tick = { "${Math.round(it)}%" },
            ticks = quarters(100.0),
            readoutWidthDp = 50f,
            readout = { values, _, _ -> listOf(ReadoutRun("${whole(values[0])}%", ers)) },
        ),
    )
}

@Composable
private fun SpeedTab(store: TelemetryStore, graphs: GraphsController, active: Boolean) {
    val isDark = MaterialTheme.colorScheme.surface.luminance() < 0.5f
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    // Landscape has the width for the desktop's combined chart; portrait gives each its own.
    val charts = remember(isDark, landscape) { if (landscape) speedCharts(isDark) else separateSpeedCharts(isDark) }
    ChartsTab(store, graphs, active, charts, landscapeRows = listOf(listOf(0)))
}

// ── Tyres ───────────────────────────────────────────────────────────────────

/** The desktop's corner colours; light mode darkens three for contrast. */
private class CornerColors(isDark: Boolean) {
    val fl = Color(0xffe10600)
    val fr = seriesColor(0xff4488ff, 0xff0b57d0, isDark)
    val rl = seriesColor(0xff37872d, 0xff137333, isDark)
    val rr = seriesColor(0xffffd700, 0xff765900, isDark)
}

/** The desktop's nice ticks for a tyre axis, always including both ends. */
private fun tyreTicks(min: Double, max: Double): List<Double> {
    val ticks = niceTicks(min, max, 6).toMutableList()
    // A round tick closer to an end than this share of a step makes way for it.
    val step = if (ticks.size >= 2) ticks[1] - ticks[0] else max - min
    if (ticks.size > 1 && ticks.first() != min && ticks.first() - min < step * END_TICK_ROOM) ticks.removeAt(0)
    if (ticks.isEmpty() || ticks.first() != min) ticks.add(0, min)
    if (ticks.size > 1 && ticks.last() != max && max - ticks.last() < step * END_TICK_ROOM) ticks.removeAt(ticks.lastIndex)
    if (ticks.last() != max) ticks.add(max)
    return ticks
}

/** As [tyreTicks], with a tick halfway through each of the first two steps, as the desktop's temperatures. */
private fun temperatureTicks(min: Double, max: Double): List<Double> {
    val ticks = tyreTicks(min, max)
    if (ticks.size < 3) return ticks
    return listOf(ticks[0], (ticks[0] + ticks[1]) / 2, ticks[1], (ticks[1] + ticks[2]) / 2) + ticks.drop(2)
}

/** Round tick values within [min]..[max], as the desktop's nice-number ticks. */
private fun niceTicks(min: Double, max: Double, count: Int): List<Double> {
    if (!min.isFinite() || !max.isFinite()) return emptyList()
    if (min == max) return listOf(min)
    fun niceNumber(range: Double, round: Boolean): Double {
        val exponent = floor(log10(range))
        val fraction = range / 10.0.pow(exponent)
        val nice = if (round) {
            when { fraction < 1.5 -> 1.0; fraction < 3 -> 2.0; fraction < 7 -> 5.0; else -> 10.0 }
        } else {
            when { fraction <= 1 -> 1.0; fraction <= 2 -> 2.0; fraction <= 5 -> 5.0; else -> 10.0 }
        }
        return nice * 10.0.pow(exponent)
    }
    val step = niceNumber(niceNumber(max - min, false) / maxOf(1, count - 1), true)
    val ticks = ArrayList<Double>()
    var value = ceil(min / step) * step
    val last = floor(max / step) * step
    while (value <= last + step * 0.5 && ticks.size < 100) {
        ticks += Math.round(value * 1e6) / 1e6
        value += step
    }
    return ticks
}

/** A tick value as the desktop's tyre axes print it: whole numbers bare, else to a tenth. */
private fun tyreTickText(value: Double, unit: String): String =
    if (value == floor(value)) "${value.toLong()}$unit" else "%.1f%s".format(Locale.US, value, unit)

/** The desktop's Tyres graphs: surface, inner and brake temperatures and wear, one line per corner. */
private fun tyreCharts(isDark: Boolean): List<ChartSpec> {
    val corners = CornerColors(isDark)
    fun cornerSeries(fl: GraphField, fr: GraphField, rl: GraphField, rr: GraphField) = listOf(
        SeriesSpec("FL", fl, corners.fl, style = LineStyle(widthDp = 2f)),
        SeriesSpec("FR", fr, corners.fr, style = LineStyle(widthDp = 2f)),
        SeriesSpec("RL", rl, corners.rl, style = LineStyle(widthDp = 2f)),
        SeriesSpec("RR", rr, corners.rr, style = LineStyle(widthDp = 2f)),
    )
    val colors = listOf(corners.fl, corners.fr, corners.rl, corners.rr)
    fun cornerReadout(unit: String): (FloatArray, BooleanArray, Color) -> List<ReadoutRun> = { values, shown, dim ->
        buildList {
            values.indices.filter { shown[it] }.forEachIndexed { position, corner ->
                if (position > 0) add(ReadoutRun(" ", dim, bold = false))
                add(ReadoutRun(if (values[corner].isFinite()) Math.round(values[corner]).toString() else "—", colors[corner]))
            }
            add(ReadoutRun(unit, dim, bold = false))
        }
    }
    return listOf(
        ChartSpec(
            id = TYRE_CHART_IDS[0],
            family = GraphFamily.TYRE_TEMPS,
            title = "Surface Temp",
            series = cornerSeries(GraphField.SURFACE_FL, GraphField.SURFACE_FR, GraphField.SURFACE_RL, GraphField.SURFACE_RR),
            range = { visibleMax, _ -> expand(0.0, 125.0, visibleMax) },
            tick = { tyreTickText(it, "°C") },
            ticks = ::temperatureTicks,
            readout = cornerReadout("°C"),
        ),
        ChartSpec(
            id = TYRE_CHART_IDS[1],
            family = GraphFamily.TYRE_TEMPS,
            title = "Inner Temp",
            series = cornerSeries(GraphField.INNER_FL, GraphField.INNER_FR, GraphField.INNER_RL, GraphField.INNER_RR),
            range = { visibleMax, _ -> expand(0.0, 125.0, visibleMax) },
            tick = { tyreTickText(it, "°C") },
            ticks = ::temperatureTicks,
            readout = cornerReadout("°C"),
        ),
        ChartSpec(
            id = TYRE_CHART_IDS[2],
            family = GraphFamily.TYRE_TEMPS,
            title = "Brake Temp",
            series = cornerSeries(GraphField.BRAKE_TEMP_FL, GraphField.BRAKE_TEMP_FR, GraphField.BRAKE_TEMP_RL, GraphField.BRAKE_TEMP_RR),
            range = { visibleMax, _ -> expand(0.0, 1250.0, visibleMax) },
            tick = { tyreTickText(it, "°C") },
            ticks = ::temperatureTicks,
            readoutWidthDp = 150f,
            readout = cornerReadout("°C"),
        ),
        ChartSpec(
            id = TYRE_CHART_IDS[3],
            family = GraphFamily.TYRE_WEAR,
            title = "Tyre Wear",
            series = cornerSeries(GraphField.WEAR_FL, GraphField.WEAR_FR, GraphField.WEAR_RL, GraphField.WEAR_RR),
            range = { _, _ -> 0.0 to 100.0 },
            tick = { tyreTickText(it, "%") },
            ticks = ::tyreTicks,
            readout = cornerReadout("%"),
        ),
    )
}

@Composable
private fun TyresTab(store: TelemetryStore, graphs: GraphsController, active: Boolean) {
    val isDark = MaterialTheme.colorScheme.surface.luminance() < 0.5f
    val charts = remember(isDark) { tyreCharts(isDark) }
    ChartsTab(store, graphs, active, charts, landscapeRows = listOf(listOf(0, 1), listOf(2, 3)))
}

// ── Misc ────────────────────────────────────────────────────────────────────

// Ride height starts at 0-50 mm and grows with this much room above the highest.
private const val RIDE_HEIGHT_INITIAL_MM = 50.0
private const val RIDE_HEIGHT_PAD_MM = 5.0

/** The desktop's Misc charts in their combined layout: G-Force and Ride Height. */
private fun miscCharts(isDark: Boolean): List<ChartSpec> {
    val lateral = seriesColor(0xfff0a500, 0xffa04300, isDark)
    val longitudinal = seriesColor(0xff5794f2, 0xff0b57d0, isDark)
    val front = seriesColor(0xff73bf69, 0xff0d6b2f, isDark)
    val rear = seriesColor(0xffb877db, 0xff7c3ba6, isDark)
    fun fixed(value: Float, digits: Int) = if (value.isFinite()) "%.${digits}f".format(Locale.US, value) else "—"
    return listOf(
        ChartSpec(
            id = MISC_CHART_IDS[0],
            family = GraphFamily.G_FORCE,
            title = "G-Force",
            series = listOf(
                SeriesSpec("Lateral", GraphField.G_LAT, lateral),
                SeriesSpec("Longitudinal", GraphField.G_LONG, longitudinal),
            ),
            range = { _, _ -> -6.0 to 6.0 },
            tick = { "${Math.round(it)}g" },
            ticks = { _, _ -> listOf(-6.0, -4.0, -2.0, 0.0, 2.0, 4.0, 6.0) },
            refLines = listOf(RefLine(0.0, dashed = false), RefLine(4.0, dashed = true), RefLine(-4.0, dashed = true)),
            readoutWidthDp = 100f,
            readout = { values, shown, dim ->
                buildList {
                    if (shown[0]) add(ReadoutRun(fixed(values[0], 2), lateral))
                    if (shown[0] && shown[1]) add(ReadoutRun("  ", dim, bold = false))
                    if (shown[1]) add(ReadoutRun(fixed(values[1], 2), longitudinal))
                    add(ReadoutRun(" g", dim, bold = false))
                }
            },
        ),
        ChartSpec(
            id = MISC_CHART_IDS[1],
            family = GraphFamily.RIDE_HEIGHT,
            title = "Ride Height",
            series = listOf(
                SeriesSpec("Front", GraphField.FRONT_HEIGHT, front),
                SeriesSpec("Rear", GraphField.REAR_HEIGHT, rear),
            ),
            range = { visibleMax, _ ->
                0.0 to if (visibleMax.isFinite()) max(RIDE_HEIGHT_INITIAL_MM, ceil(visibleMax + RIDE_HEIGHT_PAD_MM)) else RIDE_HEIGHT_INITIAL_MM
            },
            tick = { tyreTickText(it, "mm") },
            ticks = ::tyreTicks,
            readoutWidthDp = 110f,
            readout = { values, shown, dim ->
                buildList {
                    if (shown[0]) add(ReadoutRun(fixed(values[0], 1), front))
                    if (shown[0] && shown[1]) add(ReadoutRun("  ", dim, bold = false))
                    if (shown[1]) add(ReadoutRun(fixed(values[1], 1), rear))
                    add(ReadoutRun(" mm", dim, bold = false))
                }
            },
        ),
    )
}

@Composable
private fun MiscTab(store: TelemetryStore, graphs: GraphsController, active: Boolean) {
    val isDark = MaterialTheme.colorScheme.surface.luminance() < 0.5f
    val charts = remember(isDark) { miscCharts(isDark) }
    ChartsTab(store, graphs, active, charts, landscapeRows = listOf(listOf(0), listOf(1)))
}

// ── Trends ──────────────────────────────────────────────────────────────────

/** The desktop's Trends colours, kept apart from each other and the tyre corners. */
private class TrendColors(isDark: Boolean) {
    val lapTime = Color(if (isDark) 0xffb0e0ff else 0xff004d61)
    val used = Color(if (isDark) 0xffff9830 else 0xff4a148c)
    val recharged = Color(if (isDark) 0xffff6ec7 else 0xff880e4f)
}

/** Lap time on an axis: m:ss.s. */
private fun lapAxisText(seconds: Double): String {
    val tenths = Math.round(seconds * 10)
    return String.format(Locale.US, "%d:%04.1f", tenths / 600, (tenths % 600) / 10.0)
}

private fun lapTimeText(lap: TrendLap): String = formatLapTimeMs(lap.lapTimeMs) ?: "—"

/** How a lap reads in a header: its number, and whether it was the fastest or invalid. */
private fun lapLabel(lap: TrendLap, data: TrendData): String =
    "L${lap.lap}" + when {
        lap.valid == false -> " · Invalid"
        lap.lap == data.fastestLap -> " · Fastest"
        else -> ""
    }

/** The highest of [value] over [laps], or NaN. */
private fun highest(laps: List<TrendLap>, value: (TrendLap) -> Double?): Double =
    laps.mapNotNull { value(it)?.takeIf(Double::isFinite) }.maxOrNull() ?: Double.NaN

/** One portrait Trends chart: one value of each lap, on a lap-number axis. */
private class LapTrendSpec(
    /** The desktop's setting for this chart's range, which keys it. */
    val id: String,
    val title: String,
    val series: OverlaySeries,
    val range: (List<TrendLap>) -> Pair<Double, Double>,
    val tick: (Double) -> String,
    val valueText: (TrendLap) -> String,
)

private fun lapTrendSpecs(colors: TrendColors): List<LapTrendSpec> {
    fun energy(value: Double?) = value?.let { "%.2f MJ".format(Locale.US, it) } ?: "—"
    // From 0 MJ to half a megajoule above the highest lap, as on the desktop.
    fun energyRange(value: (TrendLap) -> Double?): (List<TrendLap>) -> Pair<Double, Double> = { laps ->
        val high = highest(laps, value)
        0.0 to (if (high.isFinite()) maxOf(high, 0.0) else 0.0) + 0.5
    }
    return listOf(
        LapTrendSpec(
            id = TREND_CHART_IDS[0],
            title = "Lap Times",
            series = OverlaySeries("Lap time", colors.lapTime, 0) { lap -> lap.lapTimeMs.takeIf { it > 0 }?.div(1000.0) },
            // A tenth of the spread either side, as on the desktop.
            range = { laps ->
                val times = laps.mapNotNull { it.lapTimeMs.takeIf { ms -> ms > 0 }?.div(1000.0) }
                if (times.isEmpty()) 0.0 to 1.0 else {
                    val low = times.min()
                    val high = times.max()
                    val pad = if (high > low) (high - low) * 0.1 else 1.0
                    low - pad to high + pad
                }
            },
            tick = ::lapAxisText,
            valueText = ::lapTimeText,
        ),
        LapTrendSpec(
            id = TREND_CHART_IDS[1],
            title = "ERS Usage",
            series = OverlaySeries("Used", colors.used, 0) { it.deployedMj },
            range = energyRange { it.deployedMj },
            tick = { "%.1f MJ".format(Locale.US, it) },
            valueText = { energy(it.deployedMj) },
        ),
        LapTrendSpec(
            id = TREND_CHART_IDS[2],
            title = "Recharge",
            series = OverlaySeries("Recharged", colors.recharged, 0) { it.harvestedMj },
            range = energyRange { it.harvestedMj },
            tick = { "%.1f MJ".format(Locale.US, it) },
            valueText = { energy(it.harvestedMj) },
        ),
    )
}

/** The Tyres page's wear chart over the stint or every lap, as the Trends page shows it. */
private fun trendTyreWearSpec(isDark: Boolean, id: String, overlay: LapOverlay?): ChartSpec {
    val wear = tyreCharts(isDark)[3]
    return if (overlay == null) {
        ChartSpec(
            id = id, family = wear.family, title = wear.title, series = wear.series, range = wear.range,
            tick = wear.tick, ticks = wear.ticks, readout = wear.readout, windowChoices = TREND_WINDOWS,
        )
    } else {
        // Combined: the wear on the chart's 0-1 scale, labelled by the overlay's tyre axis.
        ChartSpec(
            id = id,
            family = wear.family,
            title = "Lap Times · ERS · Tyre Wear",
            series = wear.series.map { SeriesSpec(it.label, it.field, it.color, scale = 0.01f, style = it.style) },
            range = { _, _ -> 0.0 to 1.0 },
            tick = { "" },
            ticks = { _, _ -> QUARTER_TICKS },
            readoutWidthDp = 260f,
            // Room for "10.04 MJ", as the desktop's wider extra axes.
            rightAxisDp = 56f,
            readout = { values, shown, dim -> wear.readout(FloatArray(values.size) { values[it] * 100f }, shown, dim) },
            windowChoices = TREND_WINDOWS,
            lapOverlay = overlay,
        )
    }
}

/** The combined chart's per-lap lines and its three axes: lap time, MJ and tyre %. */
private fun trendOverlay(data: State<TrendData>, colors: TrendColors): LapOverlay {
    val energyAxis = OverlayAxis(null, { "%.2f MJ".format(Locale.US, it).replace(Regex("\\.?0+ MJ$"), " MJ") }) { laps ->
        val high = maxOf(highest(laps) { it.deployedMj }, highest(laps) { it.harvestedMj })
        0.0 to (if (high.isFinite()) high else 0.5) + 0.5
    }
    return LapOverlay(
        data = data,
        series = listOf(
            OverlaySeries("Used", colors.used, 1) { it.deployedMj },
            OverlaySeries("Recharged", colors.recharged, 1) { it.harvestedMj },
            OverlaySeries("Lap time", colors.lapTime, 0) { lap -> lap.lapTimeMs.takeIf { it > 0 }?.div(1000.0) },
        ),
        axes = listOf(
            // Lap time from 0:00.0 to 3 s above the slowest lap, as on the desktop.
            OverlayAxis(colors.lapTime, ::lapAxisText) { laps ->
                val high = highest(laps) { lap -> lap.lapTimeMs.takeIf { it > 0 }?.div(1000.0) }
                0.0 to if (high.isFinite()) high + 3 else 1.0
            },
            energyAxis,
            OverlayAxis(null, { "${Math.round(it)}%" }) { 0.0 to 100.0 },
        ),
        readout = { lap, dim ->
            val used = lap.deployedMj?.let { "%.2f".format(Locale.US, it) } ?: "—"
            val recharged = lap.harvestedMj?.let { "%.2f".format(Locale.US, it) } ?: "—"
            listOf(
                ReadoutRun(lapLabel(lap, data.value) + "  ", dim, bold = false),
                ReadoutRun(lapTimeText(lap), colors.lapTime),
                ReadoutRun("  ", dim, bold = false),
                ReadoutRun(used, colors.used),
                ReadoutRun(" / ", dim, bold = false),
                ReadoutRun(recharged, colors.recharged),
                ReadoutRun(" MJ", dim, bold = false),
            )
        },
    )
}

@Composable
private fun TrendsTab(store: TelemetryStore, graphs: GraphsController, active: Boolean) {
    val isDark = MaterialTheme.colorScheme.surface.luminance() < 0.5f
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val colors = remember(isDark) { TrendColors(isDark) }
    // Changes only when a lap completes or its figures do.
    val data = remember(graphs) { derivedStateOf(structuralEqualityPolicy()) { graphs.trendData() } }
    // Landscape has the width for the desktop's combined chart; portrait gives each its own.
    val charts = remember(isDark, landscape, colors) {
        if (landscape) listOf(trendTyreWearSpec(isDark, TREND_CHART_IDS[4], trendOverlay(data, colors)))
        else listOf(trendTyreWearSpec(isDark, TREND_CHART_IDS[3], null))
    }
    val lapSpecs = remember(colors) { lapTrendSpecs(colors) }
    var rangePicker by remember { mutableStateOf<LapTrendSpec?>(null) }
    ChartsTab(
        store, graphs, active, charts,
        landscapeRows = listOf(listOf(0)),
        // Each completed lap's recorded ERS counters, in playback.
        extraSync = { graphs.sync(GraphFamily.TREND_ENERGY, 0.0, graphs.trendLaps()) },
        portrait = { chart, board ->
            val divider = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f)
            @Composable
            fun Trend(spec: LapTrendSpec, modifier: Modifier) =
                LapTrendChart(spec, data, windowFor(graphs, spec.id, TREND_WINDOWS), board, { rangePicker = spec }, modifier)
            Column(Modifier.fillMaxSize()) {
                Trend(lapSpecs[0], Modifier.fillMaxWidth().weight(1f))
                HorizontalDivider(color = divider)
                Row(Modifier.fillMaxWidth().weight(1f)) {
                    Trend(lapSpecs[1], Modifier.weight(1f).fillMaxHeight())
                    VerticalDivider(color = divider)
                    Trend(lapSpecs[2], Modifier.weight(1f).fillMaxHeight())
                }
                HorizontalDivider(color = divider)
                chart(0, true, Modifier.fillMaxWidth().weight(1.1f))
            }
        },
    )
    rangePicker?.let { spec ->
        GraphWindowDialog(
            title = "${spec.title} range",
            selected = windowFor(graphs, spec.id, TREND_WINDOWS),
            options = TREND_WINDOWS,
            onSelect = { window ->
                graphs.pinChartWindow(spec.id, window)
                rangePicker = null
            },
            onDismiss = { rangePicker = null },
        )
    }
}

/**
 * One value of each lap, joined, with a dot on each, against lap numbers:
 * the desktop's per-lap trend charts. A few dozen points, drawn directly.
 * Press and drag to read a lap.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun LapTrendChart(
    spec: LapTrendSpec,
    data: State<TrendData>,
    window: GraphWindow,
    board: ReadoutBoard,
    onRange: () -> Unit,
    modifier: Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val dim = colors.onSurfaceVariant
    val density = LocalDensity.current
    val labelPx = with(density) { MaterialTheme.typography.labelSmall.fontSize.toPx() }
    val labelPaint = remember(labelPx) { textPaint(labelPx, FontWeight.Normal.weight) }
    val emptyPx = with(density) { MaterialTheme.typography.bodyMedium.fontSize.toPx() }
    val emptyPaint = remember(emptyPx) { textPaint(emptyPx, FontWeight.Normal.weight) }
    val style = PlotStyle(colors.outlineVariant.copy(alpha = 0.35f), dim, colors.outline, labelPaint, emptyPaint)
    var inspectLap by remember { mutableStateOf<Int?>(null) }
    val laps = remember(window) { derivedStateOf(structuralEqualityPolicy()) { data.value.lapsFor(window) } }

    Column(modifier.padding(start = 12.dp, end = 12.dp, top = 6.dp, bottom = 2.dp)) {
        FlowRow(
            Modifier.fillMaxWidth().staticLayer(),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
            itemVerticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                spec.title,
                style = MaterialTheme.typography.titleSmall,
                color = colors.onSurface,
                maxLines = 1,
                modifier = Modifier.height(24.dp).wrapContentHeight(Alignment.CenterVertically),
            )
            HeaderChip(window.label, emphasised = false, description = "Laps shown", onClick = onRange)
            ReadoutSlot(
                board = board,
                id = spec.id,
                runs = {
                    val shown = laps.value
                    val lap = inspectLap?.let { number -> shown.firstOrNull { it.lap == number } } ?: shown.lastOrNull()
                    if (lap == null) emptyList() else listOf(
                        ReadoutRun(lapLabel(lap, data.value) + "  ", dim, bold = false),
                        ReadoutRun(spec.valueText(lap), spec.series.color),
                    )
                },
                modifier = Modifier.widthIn(min = 90.dp).weight(1f),
            )
        }
        Box(
            Modifier
                .fillMaxWidth()
                .weight(1f)
                .pointerInput(spec.id) {
                    val gutter = Y_GUTTER_DP.dp.toPx()
                    awaitEachGesture {
                        val down = awaitFirstDown(requireUnconsumed = false)
                        fun inspectAt(px: Float) {
                            val shown = laps.value
                            if (shown.isEmpty()) return
                            val first = shown.first().lap - 0.5
                            val span = shown.last().lap + 0.5 - first
                            val fraction = ((px - gutter) / (size.width - gutter).coerceAtLeast(1f)).coerceIn(0f, 1f)
                            val target = first + fraction * span
                            inspectLap = shown.minByOrNull { kotlin.math.abs(it.lap - target) }?.lap
                        }
                        inspectAt(down.position.x)
                        try {
                            while (true) {
                                val event = awaitPointerEvent()
                                val change = event.changes.firstOrNull { it.id == down.id } ?: break
                                if (!change.pressed) break
                                inspectAt(change.position.x)
                                change.consume()
                            }
                        } finally {
                            inspectLap = null
                        }
                    }
                },
        ) {
            Spacer(Modifier.fillMaxSize().staticLayer().drawBehind { drawLapTrend(spec, laps.value, null, style, PlotLayer.AXES) })
            Spacer(Modifier.fillMaxSize().graphicsLayer().drawBehind { drawLapTrend(spec, laps.value, null, style, PlotLayer.LINES) })
            Spacer(Modifier.fillMaxSize().graphicsLayer().drawBehind {
                val lap = inspectLap ?: return@drawBehind
                drawLapTrend(spec, laps.value, lap, style, PlotLayer.OVERLAY)
            })
        }
    }
}

/** A per-lap trend chart's [layer]: axes and lap labels, the joined values, or the lap being read. */
private fun DrawScope.drawLapTrend(spec: LapTrendSpec, laps: List<TrendLap>, inspectLap: Int?, style: PlotStyle, layer: PlotLayer) {
    val left = Y_GUTTER_DP * density
    val top = PLOT_PAD_DP * density
    val bottom = size.height - X_AXIS_DP * density
    if (bottom <= top || size.width <= left) return
    if (laps.isEmpty()) {
        if (layer == PlotLayer.AXES) drawEmpty(floatArrayOf(left, top, size.width, bottom), style)
        return
    }
    val (min, max) = spec.range(laps)
    val area = PlotArea(left, top, size.width, bottom, min, max)
    val first = laps.first().lap - 0.5
    val span = laps.last().lap + 0.5 - first
    fun xPx(lap: Double) = (area.left + (lap - first) / span * (area.right - area.left)).toFloat()
    when (layer) {
        PlotLayer.AXES -> {
            val paint = style.labelPaint
            paint.color = style.label.toArgb()
            paint.textAlign = Paint.Align.RIGHT
            val metrics = paint.fontMetrics
            val centre = -(metrics.ascent + metrics.descent) / 2f
            val canvas = drawContext.canvas.nativeCanvas
            for (value in niceTicks(min, max, 5)) {
                val y = area.yPx(value)
                drawLine(style.grid, Offset(area.left, y), Offset(area.right, y), 1f * density)
                canvas.drawText(spec.tick(value), area.left - 6f * density, y + centre, paint)
            }
            // A label every few laps, so they do not collide.
            paint.textAlign = Paint.Align.CENTER
            val perLap = (area.right - area.left) / span
            val every = maxOf(1, ceil(MIN_LAP_LABEL_SPACING_DP * density / perLap).toInt())
            val baseline = area.bottom + X_AXIS_DP * density - metrics.descent - 2f * density
            for (lap in laps) {
                if (lap.lap % every != 0 && every > 1) continue
                canvas.drawText("L${lap.lap}", xPx(lap.lap.toDouble()), baseline, paint)
            }
        }
        PlotLayer.LINES -> {
            val path = Path()
            var joined = false
            val color = spec.series.color
            for (lap in laps) {
                val value = spec.series.value(lap)
                if (value == null || !value.isFinite()) {
                    joined = false
                    continue
                }
                val point = Offset(xPx(lap.lap.toDouble()), area.yPx(value))
                if (joined) path.lineTo(point.x, point.y) else path.moveTo(point.x, point.y)
                joined = true
            }
            drawPath(path, color, style = Stroke(width = 1.5f * density, join = StrokeJoin.Round))
            for (lap in laps) {
                val value = spec.series.value(lap) ?: continue
                if (value.isFinite()) drawCircle(color, LAP_DOT_RADIUS_DP * density, Offset(xPx(lap.lap.toDouble()), area.yPx(value)))
            }
        }
        PlotLayer.OVERLAY -> {
            val lap = laps.firstOrNull { it.lap == inspectLap } ?: return
            val x = xPx(lap.lap.toDouble())
            drawLine(style.crosshair, Offset(x, area.top), Offset(x, area.bottom), 1f * density)
            val value = spec.series.value(lap)
            if (value != null && value.isFinite()) drawCircle(spec.series.color, 5f * density, Offset(x, area.yPx(value)))
        }
    }
}

/** Steering as the desktop's tooltip writes it: 23% L. */
private fun formatSteering(value: Float): String = when {
    !value.isFinite() -> "—"
    abs(value) < 0.005f -> "0%"
    else -> "${Math.round(abs(value) * 100)}% ${if (value < 0) "L" else "R"}"
}

/** Steering as the desktop's axis writes it: L50, 0, R50. */
private fun formatSteeringAxis(value: Double): String = when {
    abs(value) < 0.005 -> "0"
    else -> "${if (value < 0) "L" else "R"}${Math.round(abs(value) * 100)}"
}

@Composable
private fun InputsTab(store: TelemetryStore, graphs: GraphsController, active: Boolean) {
    val isDark = MaterialTheme.colorScheme.surface.luminance() < 0.5f
    val charts = remember(isDark) { inputCharts(isDark) }
    // Landscape as the desktop's grid: Gear beside the pedals, Steering beneath.
    ChartsTab(store, graphs, active, charts, landscapeRows = listOf(listOf(0, 1), listOf(2)))
}

private fun Float.orZero(): Float = if (isNaN()) 0f else this

private fun Double.roundToLong(): Long = Math.round(this)

/** Grows past [initialUpper] to fit the samples in view, never below it. */
private fun expand(lower: Double, initialUpper: Double, visibleMax: Double): Pair<Double, Double> =
    lower to if (visibleMax.isFinite() && visibleMax > initialUpper) ceil(visibleMax) else initialUpper

@Composable
private fun PowerTab(store: TelemetryStore, graphs: GraphsController, active: Boolean) {
    val isDark = MaterialTheme.colorScheme.surface.luminance() < 0.5f
    val hasMguh by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.cold.hasMguh } }
    val harvestLimit by remember(store) {
        derivedStateOf(structuralEqualityPolicy()) { if (store.cold.presentationYear == 2026) 8000.0 else 4000.0 }
    }
    val charts = remember(isDark, hasMguh, harvestLimit) { powerCharts(isDark, hasMguh, harvestLimit) }
    ChartsTab(store, graphs, active, charts, landscapeRows = listOf(listOf(0, 1), listOf(2, 3)))
}

/** A chart's window: its own pick among [choices], else the first of them; without choices, as any chart's. */
private fun windowFor(graphs: GraphsController, id: String, choices: List<GraphWindow>?): GraphWindow {
    if (choices == null) return graphs.windowOf(id)
    return graphs.windowOverrides[id]?.takeIf { it in choices } ?: choices.first()
}

/** Picks [window] for chart [id]; a chart with its own [choices] keeps the pick whatever the global window. */
private fun selectWindow(graphs: GraphsController, id: String, choices: List<GraphWindow>?, window: GraphWindow) {
    if (choices == null) graphs.selectChartWindow(id, window) else graphs.pinChartWindow(id, window)
}

/**
 * A tab of charts: stacked in portrait, in [landscapeRows] of chart indices
 * in landscape. They share one clock, following the first chart's samples,
 * and one read point.
 */
@Composable
private fun ChartsTab(
    store: TelemetryStore,
    graphs: GraphsController,
    active: Boolean,
    charts: List<ChartSpec>,
    landscapeRows: List<List<Int>>,
    /** More fetching on the tab's slow tick, for data no chart's family covers. */
    extraSync: (() -> Unit)? = null,
    /** A portrait layout of its own, given the charts and the tab's readout board; null to stack the charts. */
    portrait: (@Composable (chart: @Composable (Int, Boolean, Modifier) -> Unit, board: ReadoutBoard) -> Unit)? = null,
) {
    val history = graphs.history(charts.first().family)
    val families = remember(charts) { charts.map { it.family }.distinct() }
    val board = remember { ReadoutBoard() }
    val beat = remember { RasterBeat() }
    LaunchedEffect(active) { if (active) beat.run() }
    val readoutStyle = MaterialTheme.typography.labelLarge
    val readoutPx = with(LocalDensity.current) { readoutStyle.fontSize.toPx() }
    val readoutPaints = remember(readoutPx) { readoutPaints(readoutStyle, readoutPx) }
    val clock = remember { GraphClock() }
    val colors = MaterialTheme.colorScheme
    // Series switched off from a chart's legend, by chart and label.
    val hidden = remember { mutableStateMapOf<String, Boolean>() }
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE

    LaunchedEffect(active) {
        if (!active) return@LaunchedEffect
        while (true) withFrameNanos { clock.follow(store, history, it) }
    }
    LaunchedEffect(active) {
        if (!active) return@LaunchedEffect
        while (true) {
            // Per family: enough history for its longest time window, and every
            // lap its lap modes draw: the distance laps first, then the span laps
            // oldest first, so each arriving lap is added to the end of what is drawn.
            for (family in families) {
                val windows = charts.filter { it.family == family }.map { it.windowId to windowFor(graphs, it.windowId, it.windowChoices) }
                val longest = windows.maxOfOrNull { it.second.seconds } ?: 0
                val laps = buildList {
                    for ((id, window) in windows) {
                        if (!window.isDistance) continue
                        add(graphs.currentLap(family))
                        if (window.hasComparison) add(graphs.comparisonLap(family, window, id))
                    }
                    for ((_, window) in windows) if (window.isSpan) addAll(graphs.spanLaps(window))
                }.filter { it > 0 }.distinct()
                graphs.sync(family, longest.toDouble(), laps)
            }
            extraSync?.invoke()
            delay(BACKFILL_TICK_MS)
        }
    }
    LaunchedEffect(active) {
        if (!active) {
            clock.inspect = null
            clock.inspectDistance = null
        }
    }
    var windowPicker by remember { mutableStateOf<ChartSpec?>(null) }
    var lapPicker by remember { mutableStateOf<ChartSpec?>(null) }

    @Composable
    fun Chart(index: Int, showXAxis: Boolean, modifier: Modifier) {
        val spec = charts[index]
        GraphChart(
            spec, store, graphs, clock, hidden, showXAxis, active, board, beat,
            window = windowFor(graphs, spec.windowId, spec.windowChoices),
            overridden = spec.windowChoices == null && spec.windowId in graphs.windowOverrides,
            onWindow = { windowPicker = spec },
            onReferenceLap = { lapPicker = spec },
            modifier = modifier,
        )
    }

    Box(Modifier.fillMaxSize()) {
        if (landscape) {
            Column(Modifier.fillMaxSize().padding(horizontal = 8.dp)) {
                landscapeRows.forEachIndexed { rowIndex, row ->
                    Row(Modifier.fillMaxWidth().weight(1f)) {
                        row.forEach { index -> Chart(index, rowIndex == landscapeRows.lastIndex, Modifier.weight(1f).fillMaxHeight()) }
                    }
                }
            }
        } else if (portrait != null) {
            portrait({ index, showXAxis, modifier -> Chart(index, showXAxis, modifier) }, board)
        } else {
            Column(Modifier.fillMaxSize()) {
                charts.indices.forEach { index ->
                    if (index > 0) HorizontalDivider(color = colors.outlineVariant.copy(alpha = 0.5f))
                    Chart(index, index == charts.lastIndex, Modifier.fillMaxWidth().weight(1f))
                }
            }
        }
        // Every chart's header values, in one layer over the tab.
        Spacer(
            Modifier
                .matchParentSize()
                .onGloballyPositioned { board.placeRoot(it) }
                .graphicsLayer()
                .drawBehind { with(board) { paint(readoutPaints, readoutPx) } },
        )
    }

    windowPicker?.let { spec ->
        GraphWindowDialog(
            title = "${spec.title} window",
            selected = windowFor(graphs, spec.windowId, spec.windowChoices),
            options = spec.windowChoices ?: GraphWindow.entries.filter(graphs::available),
            onSelect = { window ->
                selectWindow(graphs, spec.windowId, spec.windowChoices, window)
                windowPicker = null
            },
            onDismiss = { windowPicker = null },
        )
    }
    lapPicker?.let { spec ->
        ReferenceLapDialog(
            catalog = store.playbackCatalog,
            selected = graphs.comparisonLap(spec.family, GraphWindow.SELECTED, spec.windowId),
            onSelect = { lapNumber ->
                graphs.selectReferenceLap(spec.windowId, lapNumber)
                lapPicker = null
            },
            onDismiss = { lapPicker = null },
        )
    }
}

/** The window choices as radio lists, laps then time as on the desktop; picking one closes it. */
@Composable
internal fun GraphWindowDialog(
    title: String,
    selected: GraphWindow,
    options: List<GraphWindow>,
    onSelect: (GraphWindow) -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(Modifier.selectableGroup().verticalScroll(rememberScrollState())) {
                listOf("Laps" to GraphWindow.LAPS, "Time" to GraphWindow.TIMES).forEach { (heading, group) ->
                    val shown = group.filter { it in options }
                    if (shown.isEmpty()) return@forEach
                    Text(
                        heading,
                        style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.padding(top = 8.dp, bottom = 4.dp),
                    )
                    shown.forEach { window ->
                        RadioRow(window.label, window == selected) { onSelect(window) }
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** The recording's laps for a Selected chart to compare against. */
@Composable
private fun ReferenceLapDialog(
    catalog: PlaybackCatalog,
    selected: Int,
    onSelect: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Reference lap") },
        text = {
            LazyColumn(Modifier.selectableGroup()) {
                items(catalog.laps, key = { it.lapNumber }) { lap ->
                    val time = formatLapTimeMs(lap.lapTimeMs)
                    val fastest = if (lap.lapNumber == catalog.fastestLap) " · Fastest" else ""
                    RadioRow(
                        "Lap ${lap.lapNumber}",
                        lap.lapNumber == selected,
                        detail = (time ?: "") + fastest,
                    ) { onSelect(lap.lapNumber) }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun RadioRow(label: String, selected: Boolean, detail: String? = null, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .height(48.dp)
            .clip(MaterialTheme.shapes.small)
            .selectable(selected = selected, role = Role.RadioButton, onClick = onClick),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = null)
        Spacer(Modifier.width(12.dp))
        Text(label, style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
        if (!detail.isNullOrEmpty()) {
            Text(detail, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/** What a lap-mode chart draws this frame: the current lap up to the cursor, and its comparison. */
private class LapFrame(
    val current: LapTrace?,
    /** Seconds into the current lap drawn; infinite live, where the trace ends at the newest sample. */
    val cutoff: Double,
    val comparison: LapTrace?,
    val trackLength: Double,
) {
    /** The current lap's last drawn sample, or -1. */
    val lastIndex: Int by lazy {
        val trace = current ?: return@lazy -1
        var lo = 0
        var hi = trace.size
        while (lo < hi) {
            val mid = (lo + hi) ushr 1
            if (trace.times[mid] <= cutoff) lo = mid + 1 else hi = mid
        }
        lo - 1
    }
}

private fun lapFrame(
    store: TelemetryStore,
    graphs: GraphsController,
    family: GraphFamily,
    window: GraphWindow,
    chartId: String,
    right: Double,
): LapFrame {
    graphs.history(family).version
    graphs.lapsVersion
    val currentLap = graphs.currentLap(family)
    val current = graphs.lapTrace(family, currentLap)
    val comparisonLap = if (window.hasComparison) graphs.comparisonLap(family, window, chartId) else 0
    val comparison = if (comparisonLap > 0 && comparisonLap != currentLap) graphs.lapTrace(family, comparisonLap) else null
    val cutoff = if (store.playbackCatalog.active) right - graphs.lapStartTime(currentLap) else Double.POSITIVE_INFINITY
    var trackLength = store.knownTrackLengthM()
    if (trackLength <= 0) {
        trackLength = max(current?.distance?.maxOrNull()?.toDouble() ?: 0.0, comparison?.distance?.maxOrNull()?.toDouble() ?: 0.0)
    }
    return LapFrame(current, if (cutoff.isNaN()) Double.POSITIVE_INFINITY else cutoff, comparison, max(trackLength, 1.0))
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun GraphChart(
    spec: ChartSpec,
    store: TelemetryStore,
    graphs: GraphsController,
    clock: GraphClock,
    hidden: MutableMap<String, Boolean>,
    showXAxis: Boolean,
    /** On the visible tab: only then does the chart draw new rasters. */
    active: Boolean,
    board: ReadoutBoard,
    beat: RasterBeat,
    window: GraphWindow,
    /** The chart chose its own window rather than following the global one. */
    overridden: Boolean,
    onWindow: () -> Unit,
    onReferenceLap: () -> Unit,
    modifier: Modifier,
) {
    val history = graphs.history(spec.family)
    val windowSeconds = window.seconds.toDouble()
    // A time window's or span's edge moves under a pixel at a time; a lap's
    // cut at the cursor in fixed steps, so a lap redraws only as far as it has grown.
    val right = remember(window) {
        derivedStateOf(structuralEqualityPolicy()) {
            val edge = clock.right
            val step = when {
                window.isDistance -> LAP_CUTOFF_STEP_SECONDS
                window.isSpan -> {
                    val from = graphs.spanStart(window, graphs.spanSeries(spec.family, window))
                    max(LAP_CUTOFF_STEP_SECONDS, if (from.isFinite() && edge.isFinite()) (edge - from) / RIGHT_EDGE_STEPS else 0.0)
                }
                else -> windowSeconds / RIGHT_EDGE_STEPS
            }
            if (edge.isFinite()) floor(edge / step) * step else edge
        }
    }
    val colors = MaterialTheme.colorScheme
    val dim = colors.onSurfaceVariant
    val gridColor = colors.outlineVariant.copy(alpha = 0.35f)
    val crosshairColor = colors.outline
    val density = LocalDensity.current
    val labelPx = with(density) { MaterialTheme.typography.labelSmall.fontSize.toPx() }
    val emptyPx = with(density) { MaterialTheme.typography.bodyMedium.fontSize.toPx() }
    val labelPaint = remember(labelPx) { textPaint(labelPx, FontWeight.Normal.weight) }
    val emptyPaint = remember(emptyPx) { textPaint(emptyPx, FontWeight.Normal.weight) }
    fun shown(): BooleanArray = BooleanArray(spec.series.size) { hidden["${spec.title}/${spec.series[it].label}"] != true }
    val style = PlotStyle(gridColor, dim, crosshairColor, labelPaint, emptyPaint)

    var plotSize by remember { mutableStateOf(IntSize.Zero) }
    val rasterLock = remember { Mutex() }
    val lines = remember(spec) { spec.series.map { RasterLine(it.field, it.scale, it.color, it.style) } }

    // Lap charts draw both laps into a bitmap off the main thread, adding to
    // the current lap as the cursor moves.
    val lapRasterizer = remember(spec, density) { LapRasterizer(density.density) }
    var lapRaster by remember(spec, window) { mutableStateOf<LapRaster?>(null) }
    if (active && window.isDistance) {
        LaunchedEffect(spec, window, lapRasterizer) {
            val job = coroutineContext.job
            var lastBeat = -1L
            snapshotFlow {
                val plot = plotSize
                val frame = lapFrame(store, graphs, spec.family, window, spec.windowId, right.value)
                if (plot.width <= 0 || plot.height <= 0 || (frame.current == null && frame.comparison == null)) {
                    null
                } else {
                    LapRasterRequest(
                        current = frame.current,
                        currentLast = frame.lastIndex,
                        comparison = frame.comparison,
                        trackLength = frame.trackLength,
                        shown = shown().toList(),
                        width = plot.width,
                        height = plot.height,
                        fuelPeak = history.fuelPeak,
                    )
                }
            }.distinctUntilChanged().conflate().collect { request ->
                if (request == null) {
                    lapRaster = null
                    lapRasterizer.displayed = null
                    return@collect
                }
                lastBeat = beat.next(lastBeat)
                val result = rasterLock.withLock {
                    withContext(Dispatchers.Default) { lapRasterizer.render(request, lines, spec.range) { !job.isActive } }
                }
                beat.done(lastBeat) {
                    if (result != null) {
                        lapRasterizer.displayed = result
                        lapRaster = result
                    }
                }
            }
        }
    }

    // Time and span charts draw their lines into a bitmap off the main thread;
    // a frame only places that bitmap on the current axis.
    val rasterizer = remember(spec, density) { TraceRasterizer(density.density) }
    var raster by remember(spec, window) { mutableStateOf<TraceRaster?>(null) }
    if (active && !window.isDistance) {
        LaunchedEffect(spec, window, rasterizer) {
            val job = coroutineContext.job
            var lastBeat = -1L
            snapshotFlow {
                val plot = plotSize
                val edge = right.value
                history.version
                val series: SampleSeries = if (window.isSpan) graphs.spanSeries(spec.family, window) else history
                val start = if (window.isSpan) graphs.spanStart(window, series) else edge - windowSeconds
                if (plot.width <= 0 || plot.height <= 0 || !edge.isFinite() || !start.isFinite() ||
                    edge <= start || series.size == 0
                ) {
                    null
                } else {
                    // The bitmap covers a stretch wider than the plot, fixed until the
                    // cursor leaves it, so new samples are only ever added to it.
                    val from: Double
                    val to: Double
                    val width: Int
                    if (window.isSpan) {
                        from = start
                        to = start + spanBucket(edge - start)
                        width = (plot.width * SPAN_HEADROOM).toInt()
                    } else {
                        val step = windowSeconds / 2
                        from = floor((edge - windowSeconds) / step) * step
                        to = from + windowSeconds * 2
                        width = plot.width * 2
                    }
                    RasterRequest(
                        series = series,
                        version = if (series === history) history.sampleVersion else 0,
                        epoch = if (series === history) history.epoch else 0,
                        from = from,
                        to = to,
                        shown = shown().toList(),
                        width = width,
                        height = plot.height,
                        fuelPeak = history.fuelPeak,
                    )
                }
            }.distinctUntilChanged().conflate().collect { request ->
                if (request == null) {
                    raster = null
                    rasterizer.displayed = null
                    return@collect
                }
                lastBeat = beat.next(lastBeat)
                // The history keeps growing on this thread, so the renderer gets a
                // copy of the stretch it draws; recorded laps never change.
                val fields = lines.map { it.field }
                val slice = if (request.series === history) {
                    history.stableSlice(
                        max(0, history.indexAtOrBefore(request.from)),
                        history.indexAtOrBefore(request.to) + 1,
                        fields,
                    )
                } else {
                    request.series
                }
                val result = rasterLock.withLock {
                    withContext(Dispatchers.Default) {
                        rasterizer.render(request, slice, lines, spec.range, GAP_SECONDS) { !job.isActive }
                    }
                }
                beat.done(lastBeat) {
                    if (result != null) {
                        rasterizer.displayed = result
                        raster = result
                    }
                }
            }
        }
    }

    // What the axes draw, as values that only change when the axes do.
    val timeRange = remember(spec, window) {
        derivedStateOf(structuralEqualityPolicy()) { raster?.let { it.yMin to it.yMax } }
    }
    val lapAxis = remember(spec, window) {
        derivedStateOf(structuralEqualityPolicy()) { lapRaster?.let { LapAxis(it.yMin, it.yMax, it.request.trackLength) } }
    }

    Column(modifier.padding(start = 12.dp, end = 12.dp, top = 6.dp, bottom = 2.dp)) {
        // The values keep room to be read; when the title, window and legend
        // leave too little, they move onto a second line.
        FlowRow(
            // Kept as a texture: the values over it are painted by the tab's board.
            Modifier.fillMaxWidth().staticLayer(),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
            itemVerticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                spec.title,
                style = MaterialTheme.typography.titleSmall,
                color = colors.onSurface,
                maxLines = 1,
                modifier = Modifier.height(24.dp).wrapContentHeight(Alignment.CenterVertically),
            )
            // This chart's window, as the desktop's selector beside the title.
            HeaderChip(window.label, emphasised = overridden, description = "Chart window", onClick = onWindow)
            if (window == GraphWindow.SELECTED) {
                val lap = graphs.comparisonLap(spec.family, window, spec.windowId)
                HeaderChip(if (lap > 0) "L$lap" else "Lap", emphasised = false, description = "Reference lap", onClick = onReferenceLap)
            }
            // Tapping a legend entry hides its line, as on the desktop.
            val legend = (spec.lapOverlay?.series?.map { it.label to it.color } ?: emptyList()) +
                spec.series.map { it.label to it.color }
            if (legend.size > 1) legend.forEach { (label, color) ->
                val key = "${spec.title}/$label"
                val off = hidden[key] == true
                Row(
                    Modifier.clip(MaterialTheme.shapes.small).clickable { hidden[key] = !off }.padding(horizontal = 2.dp, vertical = 2.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(
                        Modifier.size(8.dp).clip(CircleShape)
                            .drawBehind { drawRect(if (off) dim.copy(alpha = 0.4f) else color) },
                    )
                    Spacer(Modifier.width(4.dp))
                    Text(
                        label,
                        style = MaterialTheme.typography.labelMedium,
                        color = if (off) dim.copy(alpha = 0.5f) else dim,
                        maxLines = 1,
                    )
                }
            }
            // The values at the read point, or the newest ones; a comparison lap's follow, faded.
            ReadoutSlot(
                board = board,
                id = spec.id,
                runs = {
                    if (window.isDistance) {
                        lapReadout(spec, lapFrame(store, graphs, spec.family, window, spec.windowId, right.value), clock.inspectDistance, shown(), dim)
                    } else {
                        history.version
                        val series = if (window.isSpan) graphs.spanSeries(spec.family, window) else history
                        val inspectTime = clock.inspect
                        val at = inspectTime ?: right.value.takeIf { it.isFinite() } ?: Double.POSITIVE_INFINITY
                        val lapRuns = spec.lapOverlay?.let { overlay ->
                            val laps = overlay.data.value.lapsFor(window)
                            // The lap being read: the one under the finger, else the last completed.
                            val lap = laps.lastOrNull { it.startTime <= at && (inspectTime == null || at < it.endTime) } ?: laps.lastOrNull()
                            lap?.let { overlay.readout(it, dim) + ReadoutRun("   ", dim, bold = false) }
                        } ?: emptyList()
                        val index = series.indexAtOrBefore(at)
                        if (index < 0) {
                            lapRuns
                        } else {
                            lapRuns + spec.readout(FloatArray(spec.series.size) { series.value(spec.series[it].field, index) * spec.series[it].scale }, shown(), dim)
                        }
                    }
                },
                modifier = Modifier.widthIn(min = spec.readoutWidthDp.dp).weight(1f),
            )
        }
        Box(
            Modifier
                .fillMaxWidth()
                .weight(1f)
                .pointerInput(history, window) {
                    val gutter = Y_GUTTER_DP.dp.toPx()
                    val rightGutter = rightGutterDp(spec).dp.toPx()
                    // Press and drag to read the values; letting go follows the clock again.
                    awaitEachGesture {
                        val down = awaitFirstDown(requireUnconsumed = false)
                        fun inspectAt(px: Float) {
                            val span = (size.width - gutter - rightGutter).coerceAtLeast(1f)
                            val fraction = ((px - gutter) / span).coerceIn(0f, 1f).toDouble()
                            val edge = right.value
                            when {
                                window.isDistance ->
                                    clock.inspectDistance = fraction * lapFrame(store, graphs, spec.family, window, spec.windowId, edge).trackLength
                                !edge.isFinite() -> return
                                window.isSpan -> {
                                    val from = graphs.spanStart(window, graphs.spanSeries(spec.family, window))
                                    if (from.isFinite() && edge > from) clock.inspect = from + fraction * (edge - from)
                                }
                                else -> clock.inspect = edge - windowSeconds + fraction * windowSeconds
                            }
                        }
                        inspectAt(down.position.x)
                        try {
                            while (true) {
                                val event = awaitPointerEvent()
                                val change = event.changes.firstOrNull { it.id == down.id } ?: break
                                if (!change.pressed) break
                                inspectAt(change.position.x)
                                change.consume()
                            }
                        } finally {
                            clock.inspect = null
                            clock.inspectDistance = null
                        }
                    }
                }
                .onSizeChanged { size ->
                    val left = (Y_GUTTER_DP * density.density).toInt()
                    val right = (rightGutterDp(spec) * density.density).toInt()
                    val top = (PLOT_PAD_DP * density.density).toInt()
                    val bottom = ((if (showXAxis) X_AXIS_DP else PLOT_PAD_DP) * density.density).toInt()
                    plotSize = IntSize(size.width - left - right, size.height - top - bottom)
                },
        ) {
            // Two layers: the plot, redrawn as the data and window move, and the
            // read point over it, redrawn as the finger moves. Reading values
            // never re-records the axes, labels and lines beneath it. A span's
            // plot changes only every few seconds, so it is also kept as a texture.
            // Each layer reads only what it shows: the axes their range and scale,
            // the lines their raster and the window's edge, the read point
            // nothing until a finger is down. A sample that changes none of
            // these redraws nothing.
            fun DrawScope.plot(layer: PlotLayer) {
                val overlay = layer == PlotLayer.OVERLAY
                val inspect = if (overlay) (if (window.isDistance) clock.inspectDistance else clock.inspect) ?: return else null
                // Only the lines read the raster: a new one must not redraw the cached axes.
                val lines = layer == PlotLayer.LINES
                when {
                    window.isDistance -> {
                        val frame = if (overlay) lapFrame(store, graphs, spec.family, window, spec.windowId, right.value) else null
                        drawLapPlot(spec, frame, if (lines) lapRaster else null, lapAxis.value, inspect, shown(), showXAxis, style, layer)
                    }
                    window.isSpan -> {
                        val series = graphs.spanSeries(spec.family, window)
                        val from = graphs.spanStart(window, series)
                        val laps = if (layer == PlotLayer.AXES) graphs.lapStarts(spec.family) else emptyList()
                        val overlayLaps = if (overlay) null else spec.lapOverlay?.data?.value?.lapsFor(window)
                        drawPlot(
                            spec, if (lines) raster else null, timeRange.value, series, from, right.value, inspect, shown(),
                            showXAxis, style, laps, layer, overlayLaps, { hidden["${spec.title}/$it"] == true },
                        )
                    }
                    // A time window's axes do not move with it: only its labels below do.
                    layer == PlotLayer.AXES -> drawTimeAxes(spec, timeRange.value, showXAxis, style)
                    else -> {
                        val edge = right.value
                        drawPlot(spec, if (lines) raster else null, timeRange.value, history, edge - windowSeconds, edge, inspect, shown(), showXAxis, style, null, layer)
                    }
                }
            }
            Spacer(Modifier.fillMaxSize().staticLayer().drawBehind { plot(PlotLayer.AXES) })
            Spacer(Modifier.fillMaxSize().graphicsLayer().drawBehind { plot(PlotLayer.LINES) })
            Spacer(Modifier.fillMaxSize().graphicsLayer().drawBehind { plot(PlotLayer.OVERLAY) })
        }
    }
}

/**
 * Where a chart's header values go: an empty space in the header, painted
 * by the tab's [ReadoutBoard] from [runs].
 */
@Composable
private fun ReadoutSlot(board: ReadoutBoard, id: String, runs: () -> List<ReadoutRun>, modifier: Modifier) {
    val owner = remember { Any() }
    board.register(id, owner, runs)
    DisposableEffect(board, id) { onDispose { board.unregister(id, owner) } }
    val height = with(LocalDensity.current) { (MaterialTheme.typography.labelLarge.fontSize.toPx() * 1.35f).toDp() }
    Spacer(modifier.height(height).onGloballyPositioned { board.place(id, owner, it) })
}

@Composable
private fun HeaderChip(text: String, emphasised: Boolean, description: String, onClick: () -> Unit) {
    val colors = MaterialTheme.colorScheme
    val tint = if (emphasised) colors.primary else colors.onSurfaceVariant
    Row(
        Modifier.clip(MaterialTheme.shapes.small).clickable(onClick = onClick)
            .padding(start = 4.dp, top = 2.dp, bottom = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(text, style = MaterialTheme.typography.labelMedium, color = tint, maxLines = 1)
        Icon(
            painterResource(R.drawable.ic_expand_more),
            contentDescription = description,
            modifier = Modifier.size(16.dp),
            tint = tint,
        )
    }
}

/** The header values of a lap-mode chart: the current lap, then the comparison lap faded. */
private fun lapReadout(spec: ChartSpec, frame: LapFrame, inspect: Double?, shown: BooleanArray, dim: Color): List<ReadoutRun> {
    val current = frame.current
    val currentIndex = when {
        current == null -> -1
        inspect != null -> current.indexAtDistance(inspect, frame.cutoff)
        else -> frame.lastIndex
    }
    val at = inspect ?: current?.takeIf { currentIndex >= 0 }?.distance?.get(currentIndex)?.toDouble()
    fun values(trace: LapTrace, index: Int) =
        FloatArray(spec.series.size) { trace.value(spec.series[it].field, index) * spec.series[it].scale }
    val runs = ArrayList<ReadoutRun>()
    if (current != null && currentIndex >= 0) runs += spec.readout(values(current, currentIndex), shown, dim)
    val comparison = frame.comparison
    if (comparison != null && at != null) {
        val index = comparison.indexAtDistance(at)
        if (index >= 0) {
            if (runs.isNotEmpty()) runs += ReadoutRun("  │  ", dim.copy(alpha = 0.5f), bold = false)
            runs += spec.readout(values(comparison, index), shown, dim).map { it.copy(color = it.color.copy(alpha = COMPARISON_ALPHA)) }
        }
    }
    return runs
}

/** The plot beneath, or the read point drawn over it. */
/**
 * A chart's plot in three layers, each redrawn only when what it shows
 * changes: the axes (kept as a texture), the lines, and the read point.
 */
private enum class PlotLayer { AXES, LINES, OVERLAY }

/** Colours and paints a plot draws with. */
private class PlotStyle(
    val grid: Color,
    val label: Color,
    val crosshair: Color,
    val labelPaint: Paint,
    val emptyPaint: Paint,
)

/** The plot's frame: where the axes are, and the y scale. */
private class PlotArea(val left: Float, val top: Float, val right: Float, val bottom: Float, val yMin: Double, val yMax: Double) {
    private val ySpan = (yMax - yMin).takeIf { it > 0 } ?: 1.0

    fun yPx(value: Double) = (bottom - (value.coerceIn(yMin, yMax) - yMin) / ySpan * (bottom - top)).toFloat()
}

/** Room right of the plot for [spec]'s right-hand axes. */
private fun rightGutterDp(spec: ChartSpec): Float =
    maxOf(spec.rightAxes.size, (spec.lapOverlay?.axes?.size ?: 1) - 1) * spec.rightAxisDp

/** Plot bounds for this canvas, or null when it is too small to draw in. */
private fun DrawScope.plotBounds(showXAxis: Boolean, spec: ChartSpec): FloatArray? {
    val left = Y_GUTTER_DP * density
    val right = size.width - rightGutterDp(spec) * density
    val top = PLOT_PAD_DP * density
    val bottom = size.height - (if (showXAxis) X_AXIS_DP * density else PLOT_PAD_DP * density)
    if (bottom <= top || right <= left) return null
    return floatArrayOf(left, top, right, bottom)
}

private fun DrawScope.drawEmpty(bounds: FloatArray, style: PlotStyle) {
    style.emptyPaint.color = style.label.toArgb()
    style.emptyPaint.textAlign = Paint.Align.CENTER
    drawContext.canvas.nativeCanvas.drawText("No data", (bounds[0] + bounds[2]) / 2f, (bounds[1] + bounds[3]) / 2f, style.emptyPaint)
}

/**
 * Grid and Y labels: the chart's own ticks, else five even steps as the
 * desktop's fixed ranges; then its guide lines.
 */
private fun DrawScope.drawYAxis(spec: ChartSpec, area: PlotArea, style: PlotStyle, overlayLaps: List<TrendLap>? = null) {
    val overlay = spec.lapOverlay
    if (overlay != null && overlayLaps != null) return drawOverlayAxes(overlay, overlayLaps, area, style, spec.rightAxisDp)
    val paint = style.labelPaint
    paint.color = style.label.toArgb()
    paint.textAlign = Paint.Align.RIGHT
    val metrics = paint.fontMetrics
    val centre = -(metrics.ascent + metrics.descent) / 2f
    val ticks = spec.ticks?.invoke(area.yMin, area.yMax)
        ?: List(Y_TICKS) { area.yMin + (area.yMax - area.yMin) * it / (Y_TICKS - 1) }
    val canvas = drawContext.canvas.nativeCanvas
    for (value in ticks) {
        val y = area.yPx(value)
        drawLine(style.grid, Offset(area.left, y), Offset(area.right, y), 1f * density)
        paint.textAlign = Paint.Align.RIGHT
        paint.color = (spec.axisColor ?: style.label).toArgb()
        canvas.drawText(spec.tick(value), area.left - 6f * density, y + centre, paint)
        // The same ticks for the series on their own scales, to the right.
        paint.textAlign = Paint.Align.LEFT
        spec.rightAxes.forEachIndexed { index, axis ->
            paint.color = axis.color.toArgb()
            canvas.drawText(axis.format(value), area.right + (4f + index * spec.rightAxisDp) * density, y + centre, paint)
        }
    }
    paint.color = style.label.toArgb()
    for (line in spec.refLines) {
        val y = area.yPx(line.value)
        val effect = if (line.dashed) PathEffect.dashPathEffect(floatArrayOf(4f * density, 4f * density)) else null
        drawLine(style.label.copy(alpha = 0.5f), Offset(area.left, y), Offset(area.right, y), 1f * density, pathEffect = effect)
    }
}

/** X labels from [first] in [step]s up to [last], skipping any too close to the edges. */
private fun DrawScope.drawXLabels(area: PlotArea, first: Double, last: Double, step: Double, xPx: (Double) -> Float, format: (Double) -> String, style: PlotStyle) {
    val paint = style.labelPaint
    paint.textAlign = Paint.Align.CENTER
    val baseline = area.bottom + X_AXIS_DP * density - paint.fontMetrics.descent - 2f * density
    var tick = ceil(first / step) * step
    while (tick <= last) {
        val x = xPx(tick)
        if (x > area.left + 12f * density && x < area.right - 12f * density) {
            drawContext.canvas.nativeCanvas.drawText(format(tick), x, baseline, paint)
        }
        tick += step
    }
}

/** The read point's label over the x axis, on a backing so it stays legible. */
private fun DrawScope.drawReadLabel(area: PlotArea, x: Float, text: String, style: PlotStyle) {
    val paint = style.labelPaint
    paint.textAlign = Paint.Align.CENTER
    val centre = x.coerceIn(area.left + 24f * density, area.right - 24f * density)
    val half = paint.measureText(text) / 2f + 4f * density
    val baseline = area.bottom + X_AXIS_DP * density - paint.fontMetrics.descent - 2f * density
    drawRoundRect(
        style.crosshair,
        topLeft = Offset(centre - half, area.bottom + 1f * density),
        size = Size(half * 2f, X_AXIS_DP * density - 2f * density),
        cornerRadius = CornerRadius(4f * density),
    )
    val previous = paint.color
    paint.color = android.graphics.Color.WHITE
    drawContext.canvas.nativeCanvas.drawText(text, centre, baseline, paint)
    paint.color = previous
}

/**
 * [series] across session time [windowStart]..[right], its lines from
 * [raster] stretched onto this span. With [lapStarts] the axis marks each
 * lap's start with its number, as the desktop's lap spans; otherwise it reads
 * m:ss. Nothing here walks the samples: a frame costs the same for a 15 s
 * window as for a whole race.
 */
private fun DrawScope.drawPlot(
    spec: ChartSpec,
    raster: TraceRaster?,
    /** The raster's y range, which the axes follow; null before the first raster. */
    yRange: Pair<Double, Double>?,
    series: SampleSeries,
    windowStart: Double,
    right: Double,
    inspect: Double?,
    shown: BooleanArray,
    showXAxis: Boolean,
    style: PlotStyle,
    lapStarts: List<Pair<Int, Double>>?,
    layer: PlotLayer,
    /** The laps a per-lap overlay shows, or null without one. */
    overlayLaps: List<TrendLap>? = null,
    hidden: (String) -> Boolean = { false },
) {
    val bounds = plotBounds(showXAxis, spec) ?: return
    if (series.size == 0 || !right.isFinite() || !windowStart.isFinite() || right <= windowStart || yRange == null) {
        return if (layer == PlotLayer.AXES) drawEmpty(bounds, style) else Unit
    }
    val windowSeconds = right - windowStart
    // The axis follows the raster's range, so the lines and labels always agree.
    val area = PlotArea(bounds[0], bounds[1], bounds[2], bounds[3], yRange.first, yRange.second)
    val plotWidth = area.right - area.left
    fun xPx(time: Double) = (area.left + (time - windowStart) / windowSeconds * plotWidth).toFloat()

    if (layer == PlotLayer.AXES) {
        drawYAxis(spec, area, style, overlayLaps)
        if (lapStarts != null) drawLapStarts(area, windowStart, right, lapStarts, ::xPx, showXAxis, style)
    }
    // A time window's labels scroll with its lines.
    if (layer == PlotLayer.LINES && lapStarts == null && showXAxis) {
        val step = X_STEPS.firstOrNull { it / windowSeconds * plotWidth >= MIN_X_LABEL_SPACING_DP * density } ?: X_STEPS.last()
        drawXLabels(area, windowStart, right, step, ::xPx, { formatClock(it) }, style)
    }

    clipRect(area.left, 0f, area.right, size.height) {
        if (raster != null && layer == PlotLayer.LINES) {
            // The bitmap runs past the cursor: a recorded lap holds what is still to come.
            clipRect(area.left, 0f, xPx(right), size.height) {
                val destination = RectF(xPx(raster.from), area.top, xPx(raster.to), area.bottom)
                drawContext.canvas.nativeCanvas.drawBitmap(raster.bitmap, null, destination, RASTER_PAINT)
            }
        }
        val overlay = spec.lapOverlay
        if (overlay != null && overlayLaps != null && layer == PlotLayer.LINES) {
            drawOverlayLines(overlay, overlayLaps, area, ::xPx, hidden)
        }
        if (inspect != null) {
            val x = xPx(inspect)
            drawLine(style.crosshair, Offset(x, area.top), Offset(x, area.bottom), 1f * density)
            val index = series.indexAtOrBefore(inspect)
            if (index >= 0) spec.series.forEachIndexed { seriesIndex, line ->
                if (!shown[seriesIndex]) return@forEachIndexed
                val value = (series.value(line.field, index) * line.scale).toDouble()
                if (value.isFinite()) drawCircle(line.color, 3f * density, Offset(x, area.yPx(value)))
            }
        }
    }
    if (inspect != null && showXAxis) drawReadLabel(area, xPx(inspect), formatClock(inspect, tenths = true), style)
}

/** The overlay's quarter lines, labelled on the left by its first axis and on the right by the rest. */
private fun DrawScope.drawOverlayAxes(overlay: LapOverlay, laps: List<TrendLap>, area: PlotArea, style: PlotStyle, columnDp: Float) {
    val paint = style.labelPaint
    val metrics = paint.fontMetrics
    val centre = -(metrics.ascent + metrics.descent) / 2f
    val canvas = drawContext.canvas.nativeCanvas
    val ranges = overlay.axes.map { it.range(laps) }
    for (quarter in QUARTER_TICKS) {
        val y = area.yPx(quarter)
        drawLine(style.grid, Offset(area.left, y), Offset(area.right, y), 1f * density)
        overlay.axes.forEachIndexed { index, axis ->
            val (min, max) = ranges[index]
            paint.color = (axis.color ?: style.label).toArgb()
            paint.textAlign = if (index == 0) Paint.Align.RIGHT else Paint.Align.LEFT
            val x = if (index == 0) area.left - 6f * density else area.right + (4f + (index - 1) * columnDp) * density
            canvas.drawText(axis.format(min + quarter * (max - min)), x, y + centre, paint)
        }
    }
    paint.color = style.label.toArgb()
}

/** The overlay's per-lap lines, each lap's value at its end, with a dot on each. */
private fun DrawScope.drawOverlayLines(overlay: LapOverlay, laps: List<TrendLap>, area: PlotArea, xPx: (Double) -> Float, hidden: (String) -> Boolean) {
    val ranges = overlay.axes.map { it.range(laps) }
    val path = Path()
    for (series in overlay.series) {
        if (hidden(series.label)) continue
        val (min, max) = ranges[series.axis]
        val span = (max - min).takeIf { it > 0 } ?: 1.0
        path.reset()
        var joined = false
        val dots = ArrayList<Offset>()
        for (lap in laps) {
            val value = series.value(lap)
            if (value == null || !value.isFinite()) {
                joined = false
                continue
            }
            val point = Offset(xPx(lap.endTime), area.yPx((value - min) / span))
            if (joined) path.lineTo(point.x, point.y) else path.moveTo(point.x, point.y)
            joined = true
            dots += point
        }
        drawPath(path, series.color, style = Stroke(width = 1.5f * density, join = StrokeJoin.Round))
        for (dot in dots) drawCircle(series.color, LAP_DOT_RADIUS_DP * density, dot)
    }
}

/** A time window's grid and Y labels, which stay put as it scrolls. */
private fun DrawScope.drawTimeAxes(spec: ChartSpec, yRange: Pair<Double, Double>?, showXAxis: Boolean, style: PlotStyle) {
    val bounds = plotBounds(showXAxis, spec) ?: return
    if (yRange == null) return drawEmpty(bounds, style)
    drawYAxis(spec, PlotArea(bounds[0], bounds[1], bounds[2], bounds[3], yRange.first, yRange.second), style)
}

/** What a lap chart's axes show: its y range and the track's length. */
private data class LapAxis(val yMin: Double, val yMax: Double, val trackLength: Double)

/**
 * Each lap's start in view as a faint line, numbered beside it on the axis as
 * the desktop's lap spans; a number that would overlap the previous one is left out.
 */
private fun DrawScope.drawLapStarts(
    area: PlotArea,
    from: Double,
    to: Double,
    lapStarts: List<Pair<Int, Double>>,
    xPx: (Double) -> Float,
    showXAxis: Boolean,
    style: PlotStyle,
) {
    val paint = style.labelPaint
    paint.textAlign = Paint.Align.LEFT
    val baseline = area.bottom + X_AXIS_DP * density - paint.fontMetrics.descent - 2f * density
    var labelEnd = Float.NEGATIVE_INFINITY
    for ((lap, start) in lapStarts) {
        if (start < from || start > to) continue
        val x = xPx(start)
        drawLine(style.grid, Offset(x, area.top), Offset(x, area.bottom), 1f * density)
        if (!showXAxis) continue
        val text = lap.toString()
        val textX = x + LAP_LABEL_OFFSET_DP * density
        if (textX < labelEnd) continue
        val width = paint.measureText(text)
        if (textX + width > area.right) continue
        drawContext.canvas.nativeCanvas.drawText(text, textX, baseline, paint)
        labelEnd = textX + width + LAP_LABEL_GAP_DP * density
    }
}

/** One lap by distance across the track: the comparison lap faded beneath the current one. */
private fun DrawScope.drawLapPlot(
    spec: ChartSpec,
    /** The laps under the read point; needed by the read point only. */
    frame: LapFrame?,
    raster: LapRaster?,
    axis: LapAxis?,
    inspect: Double?,
    shown: BooleanArray,
    showXAxis: Boolean,
    style: PlotStyle,
    layer: PlotLayer,
) {
    val bounds = plotBounds(showXAxis, spec) ?: return
    if (axis == null) return if (layer == PlotLayer.AXES) drawEmpty(bounds, style) else Unit
    val current = frame?.current
    val comparison = frame?.comparison

    // The axis follows the raster's range, so the lines and labels always agree.
    val area = PlotArea(bounds[0], bounds[1], bounds[2], bounds[3], axis.yMin, axis.yMax)
    val plotWidth = area.right - area.left
    val length = axis.trackLength
    fun xPx(metres: Double) = (area.left + metres / length * plotWidth).toFloat()

    if (layer == PlotLayer.AXES) {
        drawYAxis(spec, area, style)
        if (showXAxis) {
            val step = DISTANCE_STEPS.firstOrNull { it / length * plotWidth >= MIN_X_LABEL_SPACING_DP * density } ?: DISTANCE_STEPS.last()
            drawXLabels(area, 0.0, length, step, ::xPx, ::formatDistance, style)
        }
    }

    clipRect(area.left, 0f, area.right, size.height) {
        if (layer == PlotLayer.LINES && raster != null) {
            val destination = RectF(area.left, area.top, area.right, area.bottom)
            drawContext.canvas.nativeCanvas.drawBitmap(raster.bitmap, null, destination, RASTER_PAINT)
        }

        if (inspect != null) {
            val x = xPx(inspect)
            drawLine(style.crosshair, Offset(x, area.top), Offset(x, area.bottom), 1f * density)
            listOfNotNull(current?.let { it to it.indexAtDistance(inspect, frame!!.cutoff) }, comparison?.let { it to it.indexAtDistance(inspect) })
                .forEach { (trace, index) ->
                    if (index < 0) return@forEach
                    spec.series.forEachIndexed { seriesIndex, series ->
                        if (!shown[seriesIndex]) return@forEachIndexed
                        val value = (trace.value(series.field, index) * series.scale).toDouble()
                        val faded = trace === comparison
                        val color = if (faded) series.color.copy(alpha = COMPARISON_ALPHA) else series.color
                        if (value.isFinite()) drawCircle(color, 3f * density, Offset(x, area.yPx(value)))
                    }
                }
        }
    }
    if (inspect != null && showXAxis) drawReadLabel(area, xPx(inspect), formatDistance(inspect), style)
}

/** The smallest of 60 s × 1.25ⁿ that holds [seconds]: a span's bitmap grows in these steps. */
private fun spanBucket(seconds: Double): Double {
    var bucket = SPAN_BASE_SECONDS
    while (bucket < seconds) bucket *= SPAN_HEADROOM
    return bucket
}

/** Lap distance as the desktop's axis writes it. */
private fun formatDistance(metres: Double): String = "${Math.round(metres)} m"

/** Session time as m:ss, or m:ss.s for a read point. */
private fun formatClock(seconds: Double, tenths: Boolean = false): String {
    val clamped = max(0.0, seconds)
    val minutes = floor(clamped / 60).toInt()
    val rest = clamped - minutes * 60
    return if (tenths) {
        String.format(Locale.US, "%d:%04.1f", minutes, floor(rest * 10) / 10)
    } else {
        String.format(Locale.US, "%d:%02d", minutes, floor(rest).toInt())
    }
}
