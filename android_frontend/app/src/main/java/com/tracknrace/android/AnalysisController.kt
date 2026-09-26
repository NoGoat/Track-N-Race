package com.tracknrace.android

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

private const val PREF_ANALYSIS_CONFIG = "analysis.config"
private const val MAX_CACHED_LAPS = 12
private const val LAP_REQUEST_RETRY_MS = 15_000L

/**
 * State behind the Analysis page: its saved configuration, the chosen laps,
 * and the laps and delta curve fetched from the desktop. Laps are requested
 * one at a time with only the channels the page draws; the desktop reads them
 * from the recording without moving its playback cursor.
 *
 * Compose state is written on the main thread only. Replies arrive on the
 * socket thread, are parsed there, and are posted across.
 */
internal class AnalysisController(
    context: Context,
    private val store: TelemetryStore,
    private val lapRequester: (Long, Int, Collection<String>) -> Boolean,
    private val deltaRequester: (PlaybackLapDeltaRequest) -> Boolean,
) {
    private data class PendingLap(
        val requestId: Long,
        val lapNumber: Int,
        val channels: Set<String>,
        val sentAt: Long,
    )

    private data class DeltaKey(val current: Int, val comparison: Int, val sectorDelta: Boolean)

    private val preferences = RecordingStorage.preferences(context)
    private val main = Handler(Looper.getMainLooper())
    private val requestIds = AtomicLong(ANALYSIS_REQUEST_BASE)

    // Read on the socket thread to know which channels a reply carries.
    private val inFlight = ConcurrentHashMap<Long, PendingLap>()
    private val pendingByLap = HashMap<Int, PendingLap>()
    private val lapOrder = ArrayDeque<Int>()
    private var generation = Long.MIN_VALUE
    private var deltaKey: DeltaKey? = null
    private var deltaRequestId = 0L
    // The current key's request reached the desktop, answered or not.
    private var deltaSent = false

    var config by mutableStateOf(AnalysisConfig.fromJson(preferences.getString(PREF_ANALYSIS_CONFIG, null)))
        private set

    /** Compare against [compareLap] (0 = none) while following the desktop's lap. */
    var compareLap by mutableIntStateOf(0)
        private set

    /** Fixed comparison: two chosen laps instead of the desktop's current one. */
    var fixedMode by mutableStateOf(false)
        private set
    var lapA by mutableIntStateOf(0)
        private set
    var lapB by mutableIntStateOf(0)
        private set

    /** Fetched laps of the current catalogue generation. */
    val laps = mutableStateMapOf<Int, AnalysisLap>()

    var deltaCurve by mutableStateOf<AnalysisDeltaCurve?>(null)
        private set

    /** True once the desktop has refused a lap request (an older desktop build). */
    var lapDataUnavailable by mutableStateOf(false)
        private set

    fun updateConfig(transform: (AnalysisConfig) -> AnalysisConfig) {
        val next = transform(config)
        if (next == config) return
        config = next
        preferences.edit().putString(PREF_ANALYSIS_CONFIG, next.toJson()).apply()
    }

    fun selectCompareLap(lapNumber: Int) {
        compareLap = lapNumber
    }

    fun useFixedLaps(enabled: Boolean) {
        fixedMode = enabled
    }

    fun selectLapA(lapNumber: Int) {
        lapA = lapNumber
    }

    fun selectLapB(lapNumber: Int) {
        lapB = lapNumber
    }

    /**
     * Makes sure every lap in [wanted] is fetched with at least [channels].
     * Idempotent; the page calls it whenever its needs change and on a slow
     * tick so a lost reply is asked for again.
     */
    fun sync(catalog: PlaybackCatalog, wanted: Collection<Int>, channels: Set<String>) {
        resetIfNewCatalogue(catalog)
        if (!catalog.active) return
        val now = SystemClock.elapsedRealtime()
        for (lapNumber in wanted) {
            if (lapNumber <= 0 || catalog.laps.none { it.lapNumber == lapNumber }) continue
            val cached = laps[lapNumber]
            if (cached != null && cached.covers(channels)) continue
            val pending = pendingByLap[lapNumber]
            if (pending != null && pending.channels.containsAll(channels) &&
                now - pending.sentAt < LAP_REQUEST_RETRY_MS
            ) continue
            // Ask for everything the lap already had too, so the reply replaces it whole.
            val request = PendingLap(
                requestIds.incrementAndGet(),
                lapNumber,
                channels + (cached?.channels ?: emptySet()),
                now,
            )
            pending?.let { inFlight.remove(it.requestId) }
            inFlight[request.requestId] = request
            pendingByLap[lapNumber] = request
            if (!lapRequester(request.requestId, lapNumber, request.channels)) {
                inFlight.remove(request.requestId)
                pendingByLap.remove(lapNumber)
                lapDataUnavailable = store.sourceStatus.state == "connected" ||
                    store.sourceStatus.state == "paired"
            }
        }
    }

    /** Requests the delta curve for a pair of laps, or clears it for none. */
    fun syncDelta(catalog: PlaybackCatalog, current: Int, comparison: Int, sectorDelta: Boolean) {
        resetIfNewCatalogue(catalog)
        val key = if (catalog.active && catalog.deltaAvailable && current > 0 && comparison > 0) {
            DeltaKey(current, comparison, sectorDelta)
        } else {
            null
        }
        if (key == deltaKey && (key == null || deltaSent)) return
        if (key != deltaKey) deltaCurve = null
        deltaKey = key
        deltaRequestId = 0L
        deltaSent = false
        if (key == null) return
        val request = PlaybackLapDeltaRequest(
            requestId = requestIds.incrementAndGet(),
            currentLap = key.current,
            comparisonLap = key.comparison,
            sectorDelta = key.sectorDelta,
        )
        deltaRequestId = request.requestId
        deltaSent = deltaRequester(request)
        if (!deltaSent) deltaRequestId = 0L
    }

    /** Socket thread: a `lap_data` frame. */
    fun onLapData(json: String) {
        val requestId = AnalysisLapParser.requestIdOf(json) ?: return
        val request = inFlight.remove(requestId) ?: return
        val reply = AnalysisLapParser.parse(json, request.channels)
        main.post { installLap(request, reply?.lap) }
    }

    /** Socket thread: a `lap_delta` reply to one of this page's requests. */
    fun onDeltaReply(requestId: Long, data: JSONObject?) {
        val curve = AnalysisDeltaCurve.parse(data)
        main.post {
            if (requestId != deltaRequestId) return@post
            deltaRequestId = 0L
            val key = deltaKey ?: return@post
            deltaCurve = curve?.takeIf {
                it.currentLap == key.current && it.comparisonLap == key.comparison
            }
        }
    }

    private fun installLap(request: PendingLap, lap: AnalysisLap?) {
        if (pendingByLap[request.lapNumber]?.requestId != request.requestId) return
        // An empty reply (nothing playing, unknown lap) keeps the request
        // pending, so it is asked again only after the retry interval.
        if (lap == null || lap.lapNumber != request.lapNumber) return
        pendingByLap.remove(request.lapNumber)
        lapDataUnavailable = false
        laps[request.lapNumber] = lap
        lapOrder.remove(request.lapNumber)
        lapOrder.addLast(request.lapNumber)
        while (lapOrder.size > MAX_CACHED_LAPS) laps.remove(lapOrder.removeFirst())
    }

    private fun resetIfNewCatalogue(catalog: PlaybackCatalog) {
        if (catalog.generation == generation) return
        generation = catalog.generation
        laps.clear()
        lapOrder.clear()
        pendingByLap.clear()
        inFlight.clear()
        deltaKey = null
        deltaRequestId = 0L
        deltaSent = false
        deltaCurve = null
        // Chosen laps belonged to the previous recording or driver.
        compareLap = 0
        lapA = 0
        lapB = 0
    }
}
