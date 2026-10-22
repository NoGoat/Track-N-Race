package com.nogoat.tracknrace

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong

/** Graphs numbers its lap requests from here, apart from Analysis. */
internal const val GRAPHS_REQUEST_BASE = 1L shl 41

private const val LAP_REQUEST_RETRY_MS = 15_000L
private const val MIN_CACHED_LAPS = 4
// Lap requests outstanding at once: a race's worth for All Laps goes a few at
// a time, so the newest laps arrive first and the socket stays responsive.
private const val MAX_LAPS_IN_FLIGHT = 2
private const val PREF_GRAPH_WINDOW = "graphs.window_mode"
private const val PREF_GRAPH_WINDOW_OVERRIDES = "graphs.window_mode_overrides"

/**
 * What a chart's x axis spans, as the desktop's window selector: the last
 * [seconds] of session time; one lap by distance with an optional faded
 * comparison lap; or the session time of the current stint or every lap,
 * marked at each lap's start. [id] is the desktop's value ("30", "CL", ...).
 */
internal enum class GraphWindow(val id: String, val label: String, val seconds: Int = 0) {
    CURRENT("CL", "Current"),
    PREVIOUS("PL", "Previous"),
    FASTEST("FL", "Fastest"),
    SELECTED("RL", "Selected"),
    STINT("SL", "Stint Laps"),
    ALL("AL", "All Laps"),
    S15("15", "15s", 15),
    S30("30", "30s", 30),
    M1("60", "1m", 60),
    M2("120", "2m", 120),
    M5("300", "5m", 300),
    M10("600", "10m", 600),
    ;

    /** In the Laps group; live, these need the lap rows. */
    val isLap: Boolean get() = seconds == 0

    /** One lap against lap distance. */
    val isDistance: Boolean get() = this == CURRENT || this == PREVIOUS || this == FASTEST || this == SELECTED

    /** Session time across the stint or every lap. */
    val isSpan: Boolean get() = this == STINT || this == ALL

    /** The lap drawn faded behind the current one. */
    val hasComparison: Boolean get() = this == PREVIOUS || this == FASTEST || this == SELECTED

    /** The label of the comparison lap, as the desktop's tooltip names it. */
    val comparisonLabel: String?
        get() = when (this) {
            PREVIOUS -> "Previous lap"
            FASTEST -> "Fastest lap"
            SELECTED -> "Reference lap"
            else -> null
        }

    companion object {
        val DEFAULT = S30
        val LAPS = entries.filter { it.isLap }
        val TIMES = entries.filter { !it.isLap }

        fun fromId(id: String?): GraphWindow? = entries.firstOrNull { it.id == id }
    }
}

private val FUEL_CHANNEL: Set<String> = setOf(GraphField.FUEL_KG.channel)

private fun Double.orLowest(): Double = if (isFinite()) this else Double.NEGATIVE_INFINITY

/**
 * The Graphs page's window choices, and the recorded laps it draws in
 * playback. The desktop streams only the latest state, so after a page
 * opens or the cursor jumps, the samples already behind the cursor are
 * fetched from the recorded laps that cover the window and put in front of
 * what the stream has delivered since. The lap modes draw recorded laps by
 * distance; the first lap's fuel gives the fuel axis its starting load, as on
 * the desktop. Each [GraphFamily] keeps its own laps and history.
 *
 * Compose state is written on the main thread only; replies are parsed on
 * the socket thread and posted across.
 */
internal class GraphsController(
    context: Context,
    private val store: TelemetryStore,
    private val lapRequester: (Long, Int, Collection<String>) -> Boolean,
) {
    private data class Pending(
        val family: GraphFamily,
        val lapNumber: Int,
        val generation: Long,
        val fuelOnly: Boolean,
        val sentAt: Long,
    )

    private class SpanCache(val laps: List<Int>, val version: Int, val series: RecordedSeries)

    /** One family's recorded laps and backfill progress; main thread only. */
    private class FamilyLaps(family: GraphFamily) {
        val channels: Set<String> = family.fields.map { it.channel }.toSet()
        val laps = LinkedHashMap<Int, LapTrace>()
        val spans = HashMap<GraphWindow, SpanCache>()
        val sentAt = HashMap<Int, Long>()
        // The history epoch and laps last backfilled, so a window the recording
        // cannot fill further is not filled again every tick.
        var appliedEpoch = -1
        var appliedLaps = emptyList<Int>()
        // The laps the page draws or backfills now; never evicted.
        var wantedLaps = emptySet<Int>()

        fun clear() {
            laps.clear()
            spans.clear()
            sentAt.clear()
            appliedEpoch = -1
        }
    }

    private val preferences = RecordingStorage.preferences(context)
    private val main = Handler(Looper.getMainLooper())
    private val requestIds = AtomicLong(GRAPHS_REQUEST_BASE)
    private val inFlight = ConcurrentHashMap<Long, Pending>()

    // Main thread only.
    private var generation = Long.MIN_VALUE
    private val families = GraphFamily.entries.associateWith { FamilyLaps(it) }
    private var initialFuelSentAt = 0L
    private var initialFuel = Float.NaN

    /** Bumped when a recorded lap arrives, so a chart drawing it redraws. */
    var lapsVersion by mutableIntStateOf(0)
        private set

    /** The window every chart shows unless it chooses its own. */
    var globalWindow by mutableStateOf(
        GraphWindow.fromId(preferences.getString(PREF_GRAPH_WINDOW, null)) ?: GraphWindow.DEFAULT,
    )
        private set

    /** Charts that chose their own window, by chart id. */
    val windowOverrides = mutableStateMapOf<String, GraphWindow>().apply {
        val saved = preferences.getString(PREF_GRAPH_WINDOW_OVERRIDES, null)
        if (saved != null) try {
            val json = JSONObject(saved)
            for (key in json.keys()) GraphWindow.fromId(json.optString(key))?.let { put(key, it) }
        } catch (_: Exception) {
            // A damaged entry only loses the per-chart choices.
        }
    }

    /** The lap each Selected chart compares against, by chart id; for this recording only. */
    val referenceLaps = mutableStateMapOf<String, Int>()

    /** The lap Selected charts compare against unless they chose their own; 0 for the fastest. For this recording only. */
    var globalReferenceLap by mutableIntStateOf(0)
        private set

    fun history(family: GraphFamily): SampleHistory = store.graphHistories.getValue(family)

    /** The choices on offer: Selected needs a recording, as on the desktop. */
    fun available(window: GraphWindow): Boolean = window != GraphWindow.SELECTED || store.playbackCatalog.active

    /** The window a chart shows: its own if still on offer, else the global one, else 30 s. */
    fun windowOf(chartId: String): GraphWindow {
        windowOverrides[chartId]?.takeIf(::available)?.let { return it }
        return globalWindow.takeIf(::available) ?: GraphWindow.DEFAULT
    }

    fun selectGlobalWindow(window: GraphWindow) {
        globalWindow = window
        preferences.edit().putString(PREF_GRAPH_WINDOW, window.id).apply()
    }

    /** As on the desktop, choosing the global window again follows it. */
    fun selectChartWindow(chartId: String, window: GraphWindow) {
        if (window == globalWindow) windowOverrides.remove(chartId) else windowOverrides[chartId] = window
        val json = JSONObject()
        for ((key, value) in windowOverrides) json.put(key, value.id)
        preferences.edit().putString(PREF_GRAPH_WINDOW_OVERRIDES, json.toString()).apply()
    }

    /** Keeps [window] for [chartId] whatever the global window: for charts with windows of their own. */
    fun pinChartWindow(chartId: String, window: GraphWindow) {
        windowOverrides[chartId] = window
        val json = JSONObject()
        for ((key, value) in windowOverrides) json.put(key, value.id)
        preferences.edit().putString(PREF_GRAPH_WINDOW_OVERRIDES, json.toString()).apply()
    }

    /** As the window, choosing the global lap again follows it. */
    fun selectReferenceLap(chartId: String, lapNumber: Int) {
        if (lapNumber == globalLap()) referenceLaps.remove(chartId) else referenceLaps[chartId] = lapNumber
    }

    fun selectGlobalReferenceLap(lapNumber: Int) {
        globalReferenceLap = lapNumber
    }

    /** The lap Selected charts compare against unless they chose their own. */
    fun globalLap(): Int = globalReferenceLap.takeIf { it > 0 } ?: store.playbackCatalog.fastestLap

    /** The current lap: under the desktop's cursor in playback, the newest lap row live. */
    fun currentLap(family: GraphFamily): Int =
        if (store.playbackCatalog.active) store.playbackSelectedLap else history(family).currentLap

    /** The lap a chart in [window] compares against, or 0 for none. */
    fun comparisonLap(family: GraphFamily, window: GraphWindow, chartId: String): Int {
        val playback = store.playbackCatalog.active
        return when (window) {
            GraphWindow.PREVIOUS -> (currentLap(family) - 1).coerceAtLeast(0)
            GraphWindow.FASTEST ->
                if (playback) store.playbackCatalog.fastestLap else history(family).fastestLiveLap()
            GraphWindow.SELECTED -> referenceLaps[chartId] ?: globalLap()
            else -> 0
        }
    }

    /**
     * Lap [lapNumber] of [family] by distance: recorded in playback (null
     * until it arrives; [sync] asks for it), built from the held samples live.
     */
    fun lapTrace(family: GraphFamily, lapNumber: Int): LapTrace? {
        if (lapNumber <= 0) return null
        return if (store.playbackCatalog.active) {
            families.getValue(family).laps[lapNumber]
        } else {
            history(family).liveLapTrace(lapNumber)
        }
    }

    /**
     * The recorded laps a [window] span draws, oldest first, up to the
     * cursor's lap: every lap, or back from the current one while the fitted
     * tyre is unchanged. Status samples then find the exact fitting time,
     * which also catches a stop for the same compound.
     */
    fun spanLaps(window: GraphWindow): List<Int> {
        val catalog = store.playbackCatalog
        if (!catalog.active || !window.isSpan) return emptyList()
        val cursor = store.latestPlaybackCursorTime()
        val laps = catalog.laps
            .filter { it.startSessionTime.isFinite() && (!cursor.isFinite() || it.startSessionTime <= cursor) }
            .sortedBy { it.startSessionTime }
        if (window == GraphWindow.ALL || laps.isEmpty()) return laps.map { it.lapNumber }
        val reference = laps.lastOrNull { it.tyreCompound > 0 } ?: return laps.map { it.lapNumber }
        var from = laps.size - 1
        while (from > 0) {
            val before = laps[from - 1]
            val same = before.tyreCompound == 0 ||
                (before.tyreCompound == reference.tyreCompound && before.visualCompound == reference.visualCompound)
            if (!same) break
            from--
        }
        return laps.subList(from, laps.size).map { it.lapNumber }
    }

    /** What a [window] span of [family] draws: the recorded laps joined in playback, the held samples live. */
    fun spanSeries(family: GraphFamily, window: GraphWindow): SampleSeries {
        if (!store.playbackCatalog.active) return history(family)
        val state = families.getValue(family)
        val wanted = spanLaps(window)
        state.spans[window]?.let { if (it.laps == wanted && it.version == lapsVersion) return it.series }
        val series = RecordedSeries(wanted.mapNotNull { state.laps[it] })
        state.spans[window] = SpanCache(wanted, lapsVersion, series)
        return series
    }

    /**
     * Where a [window] span starts: the first lap, or the current stint's
     * fitting, from the status samples where the chart has them and from the
     * status rows otherwise.
     */
    fun spanStart(window: GraphWindow, series: SampleSeries): Double {
        val stint = window == GraphWindow.STINT
        if (series is RecordedSeries) {
            val first = spanLaps(window).firstOrNull() ?: return Double.NaN
            val firstStart = store.playbackCatalog.laps.firstOrNull { it.lapNumber == first }?.startSessionTime ?: Double.NaN
            if (!stint) return firstStart
            val fitted = maxOf(series.stintStart.orLowest(), store.stint.start.orLowest())
            return if (fitted.isFinite() && fitted > firstStart) fitted else firstStart
        }
        val history = series as? SampleHistory ?: return Double.NaN
        if (!stint) return history.firstTime
        val fitted = maxOf(history.stintStart.orLowest(), store.stint.start.orLowest())
        return if (fitted.isFinite() && fitted > history.firstTime) fitted else history.firstTime
    }

    /** Each lap's number and start time, in order, for the span axes. */
    fun lapStarts(family: GraphFamily): List<Pair<Int, Double>> {
        val catalog = store.playbackCatalog
        if (!catalog.active) return history(family).lapStarts()
        return catalog.laps.filter { it.startSessionTime.isFinite() }
            .sortedBy { it.startSessionTime }
            .map { it.lapNumber to it.startSessionTime }
    }

    /**
     * The Trends page's completed laps: from the recording's lap list and
     * each lap's recorded ERS counters in playback, from the lap rows and
     * held status samples live. Validity is known live only.
     */
    fun trendData(): TrendData {
        val catalog = store.playbackCatalog
        // Read so a caller deriving state from this follows each lap's end.
        store.playbackSelectedLap
        lapsVersion
        val hasMguh = store.cold.hasMguh
        val stintStart = store.stint.start
        return if (catalog.active) playbackTrends(catalog, hasMguh, stintStart) else liveTrends(hasMguh, stintStart)
    }

    /** The completed recorded laps the Trends page reads ERS counters from, oldest first; all stay held. */
    fun trendLaps(): List<Int> {
        val catalog = store.playbackCatalog
        if (!catalog.active) return emptyList()
        return completedLaps(catalog).map { it.lapNumber }
    }

    private fun completedLaps(catalog: PlaybackCatalog): List<PlaybackLapInfo> {
        val cursor = store.latestPlaybackCursorTime()
        return catalog.laps
            .filter {
                it.startSessionTime.isFinite() && it.endSessionTime.isFinite() && it.lapTimeMs > 0 &&
                    (!cursor.isFinite() || it.endSessionTime <= cursor)
            }
            .sortedBy { it.startSessionTime }
    }

    private fun playbackTrends(catalog: PlaybackCatalog, hasMguh: Boolean, stintStart: Double): TrendData {
        val energy = families.getValue(GraphFamily.TREND_ENERGY).laps
        val laps = completedLaps(catalog).map { lap ->
            val trace = energy[lap.lapNumber]
            val harvestK = trace?.let { lapTotal(it, GraphField.TREND_HARVEST_MGUK_J) }
            val harvestH = if (hasMguh) trace?.let { lapTotal(it, GraphField.TREND_HARVEST_MGUH_J) } else 0.0
            TrendLap(
                lap = lap.lapNumber,
                startTime = lap.startSessionTime,
                endTime = lap.endSessionTime,
                lapTimeMs = lap.lapTimeMs,
                valid = null,
                deployedMj = trace?.let { lapTotal(it, GraphField.ERS_DEPLOYED_J) }?.div(1e6),
                harvestedMj = if (harvestK != null && harvestH != null) (harvestK + harvestH) / 1e6 else null,
            )
        }
        // The stint by the fitted compound, then by the fitting itself: from the
        // tyre wear starting over, or the status rows when they saw it.
        var stintLap = spanLaps(GraphWindow.STINT).firstOrNull() ?: laps.firstOrNull()?.lap ?: 1
        val fitted = maxOf(
            spanStart(GraphWindow.STINT, spanSeries(GraphFamily.TYRE_WEAR, GraphWindow.STINT)).takeIf { it.isFinite() } ?: Double.NEGATIVE_INFINITY,
            stintStart.takeIf { it.isFinite() } ?: Double.NEGATIVE_INFINITY,
        )
        if (fitted.isFinite()) {
            catalog.laps.lastOrNull { it.startSessionTime <= fitted + 1e-3 }?.let { if (it.lapNumber > stintLap) stintLap = it.lapNumber }
        }
        return TrendData(laps, stintLap, catalog.fastestLap)
    }

    private fun liveTrends(hasMguh: Boolean, stintStart: Double): TrendData {
        val history = store.graphHistories.getValue(GraphFamily.TREND_ENERGY)
        history.version
        val tracks = history.liveLaps().sortedBy { it.startTime }
        val laps = ArrayList<TrendLap>()
        for (index in 0 until tracks.size - 1) {
            val track = tracks[index]
            if (track.lapMs <= 0) continue
            val end = tracks[index + 1].startTime
            val from = history.indexAtOrBefore(track.startTime) + 1
            val until = history.indexAtOrBefore(end - 1e-6) + 1
            fun total(field: GraphField): Double? {
                if (until <= from) return null
                val times = DoubleArray(until - from) { history.time(from + it) }
                val values = FloatArray(until - from) { history.value(field, from + it) }
                return lapTotal(times, values, 0, times.size, track.startTime, end)
            }
            val harvestK = total(GraphField.TREND_HARVEST_MGUK_J)
            val harvestH = if (hasMguh) total(GraphField.TREND_HARVEST_MGUH_J) else 0.0
            laps += TrendLap(
                lap = track.lapNumber,
                startTime = track.startTime,
                endTime = end,
                lapTimeMs = track.lapMs,
                valid = !track.invalid,
                deployedMj = total(GraphField.ERS_DEPLOYED_J)?.div(1e6),
                harvestedMj = if (harvestK != null && harvestH != null) (harvestK + harvestH) / 1e6 else null,
            )
        }
        val wear = history(GraphFamily.TYRE_WEAR)
        val fitted = listOf(stintStart, if (wear.size > 0) spanStart(GraphWindow.STINT, wear) else Double.NaN)
            .filter { it.isFinite() }.maxOrNull()
        val stintLap = if (fitted != null) {
            tracks.lastOrNull { it.startTime <= fitted + 1e-3 }?.lapNumber ?: laps.firstOrNull()?.lap ?: 1
        } else {
            laps.firstOrNull()?.lap ?: 1
        }
        val fastest = laps.filter { it.valid != false }.minByOrNull { it.lapTimeMs }?.lap ?: 0
        return TrendData(laps, stintLap, fastest)
    }

    /** Where the recorded lap [lapNumber] started, for drawing the current lap up to the cursor. */
    fun lapStartTime(lapNumber: Int): Double =
        store.playbackCatalog.laps.firstOrNull { it.lapNumber == lapNumber }?.startSessionTime ?: Double.NaN

    /**
     * Main thread, on a slow tick while a [family] graph page is visible:
     * backfills the longest time window, [windowSeconds] behind the cursor (0
     * for none), and fetches the recorded [lapNumbers] the lap modes draw,
     * most wanted first.
     */
    fun sync(family: GraphFamily, windowSeconds: Double, lapNumbers: List<Int>) {
        val catalog = store.playbackCatalog
        if (catalog.generation != generation) {
            generation = catalog.generation
            families.values.forEach { it.clear() }
            inFlight.clear()
            initialFuelSentAt = 0L
            initialFuel = Float.NaN
            referenceLaps.clear()
            globalReferenceLap = 0
            lapsVersion++
        }
        val history = history(family)
        if (!catalog.active || !history.enabled) return
        val state = families.getValue(family)
        val now = SystemClock.elapsedRealtime()
        if (family == GraphFamily.STATUS) {
            requestInitialFuel(catalog, now)
            if (initialFuel.isFinite()) history.noteFuel(initialFuel)
        }

        val backfill = backfillLaps(history, catalog, windowSeconds)
        val recorded = lapNumbers.filter { lapNumber -> catalog.laps.any { it.lapNumber == lapNumber } }
        state.wantedLaps = backfill.toSet() + recorded
        // A reply that never came no longer holds a slot.
        inFlight.entries.removeIf { now - it.value.sentAt >= LAP_REQUEST_RETRY_MS }
        var missing = false
        for (lapNumber in (backfill + recorded).distinct()) {
            if (state.laps.containsKey(lapNumber)) continue
            if (lapNumber in backfill) missing = true
            val sent = state.sentAt[lapNumber]
            if (sent != null && now - sent < LAP_REQUEST_RETRY_MS) continue
            if (inFlight.values.count { !it.fuelOnly } >= MAX_LAPS_IN_FLIGHT) continue
            request(family, lapNumber, state.channels, fuelOnly = false)
            state.sentAt[lapNumber] = now
        }
        if (missing || backfill.isEmpty()) return
        if (state.appliedEpoch == history.epoch && state.appliedLaps == backfill) return
        state.appliedEpoch = history.epoch
        state.appliedLaps = backfill
        val cursor = store.latestPlaybackCursorTime()
        // Newest first: each lap fills in front of what the previous one left.
        for (lapNumber in backfill) {
            val lap = state.laps[lapNumber] ?: continue
            val times = DoubleArray(lap.size) { lap.startSessionTime + lap.times[it] }
            history.backfill(times, lap.values, cursor)
        }
    }

    /** The recorded laps covering the window behind the cursor that the history lacks, newest first. */
    private fun backfillLaps(history: SampleHistory, catalog: PlaybackCatalog, windowSeconds: Double): List<Int> {
        if (windowSeconds <= 0) return emptyList()
        val cursor = store.latestPlaybackCursorTime()
        if (!cursor.isFinite()) return emptyList()
        val from = cursor - windowSeconds
        if (history.size > 0 && history.firstTime <= from) return emptyList()
        val upTo = if (history.size > 0) history.firstTime else cursor
        return catalog.laps
            .filter {
                it.startSessionTime.isFinite() && it.endSessionTime.isFinite() &&
                    it.endSessionTime >= from && it.startSessionTime <= upTo
            }
            .sortedByDescending { it.startSessionTime }
            .map { it.lapNumber }
    }

    /** Socket or lap-reader thread: a `lap_data` reply numbered from [GRAPHS_REQUEST_BASE]. */
    fun onLapData(json: String) {
        val requestId = AnalysisLapParser.requestIdOf(json) ?: return
        val pending = inFlight.remove(requestId) ?: return
        val channels = if (pending.fuelOnly) FUEL_CHANNEL else families.getValue(pending.family).channels
        val recorded = AnalysisLapParser.parse(json, channels)?.lap
            ?.takeIf { it.lapNumber == pending.lapNumber }
            ?.lapTrace(pending.family)
        main.post { install(pending, recorded) }
    }

    private fun requestInitialFuel(catalog: PlaybackCatalog, now: Long) {
        if (initialFuel.isFinite()) return
        if (initialFuelSentAt != 0L && now - initialFuelSentAt < LAP_REQUEST_RETRY_MS) return
        val first = catalog.laps.minByOrNull { it.lapNumber } ?: return
        initialFuelSentAt = now
        request(GraphFamily.STATUS, first.lapNumber, FUEL_CHANNEL, fuelOnly = true)
    }

    private fun request(family: GraphFamily, lapNumber: Int, channels: Set<String>, fuelOnly: Boolean) {
        val requestId = requestIds.incrementAndGet()
        inFlight[requestId] = Pending(family, lapNumber, generation, fuelOnly, SystemClock.elapsedRealtime())
        if (!lapRequester(requestId, lapNumber, channels)) inFlight.remove(requestId)
    }

    private fun install(pending: Pending, lap: LapTrace?) {
        // A different recording or driver since; an empty reply is asked again later.
        if (pending.generation != generation || lap == null) return
        if (pending.fuelOnly) {
            initialFuel = lap.values[GraphField.FUEL_KG.slot].firstOrNull { it.isFinite() } ?: return
            store.statusHistory.noteFuel(initialFuel)
            return
        }
        val state = families.getValue(pending.family)
        state.sentAt.remove(pending.lapNumber)
        state.laps.remove(pending.lapNumber)
        state.laps[pending.lapNumber] = lap
        val evictable = state.laps.keys.filter { it !in state.wantedLaps }.toMutableList()
        while (state.laps.size > maxOf(MIN_CACHED_LAPS, state.wantedLaps.size) && evictable.isNotEmpty()) {
            state.laps.remove(evictable.removeAt(0))
        }
        lapsVersion++
    }
}
