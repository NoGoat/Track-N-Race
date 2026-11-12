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

private const val PREF_ANALYSIS_CONFIG = "analysis.config"
private const val MAX_CACHED_LAPS = 12
private const val LAP_REQUEST_RETRY_MS = 15_000L

/**
 * One lap of the recording playing: its driver (a car index, or -1 for the
 * driver the catalogue follows) and its number.
 */
internal data class AnalysisLapKey(val driver: Int, val lap: Int)

/**
 * State behind the Analysis page: its saved configuration, the chosen laps,
 * and the laps and delta curve fetched from the desktop. Laps are requested
 * one at a time with only the channels the page draws; the desktop reads them
 * from the recording without moving its playback cursor.
 *
 * Each compared lap has its own driver, as on the desktop: -1 is the driver
 * the catalogue follows, and any other driver's laps are listed from a
 * catalogue fetched when the lap sheet first shows that driver.
 *
 * Compose state is written on the main thread only. Replies arrive on the
 * socket thread, are parsed there, and are posted across.
 */
internal class AnalysisController(
    context: Context,
    private val store: TelemetryStore,
    /** Request id, driver, lap number, channels. */
    private val lapRequester: (Long, Int, Int, Collection<String>) -> Boolean,
    private val deltaRequester: (PlaybackLapDeltaRequest) -> Boolean,
    /** Request id, driver. */
    private val driverLapsRequester: (Long, Int) -> Boolean,
    private val driverLapsSupported: () -> Boolean,
    private val brakingRequester: (BrakingZonesRequest) -> Boolean,
    private val mapLayerRequester: (AnalysisMapRequest) -> Boolean,
) {
    private data class PendingLap(
        val requestId: Long,
        val key: AnalysisLapKey,
        val channels: Set<String>,
        val sentAt: Long,
    )

    private data class PendingDriver(val requestId: Long, val sentAt: Long)

    private data class DeltaKey(val current: AnalysisLapKey, val comparison: AnalysisLapKey, val sectorDelta: Boolean)

    /** The laps braking zones are found for; a comparison lap of 0 is none. */
    private data class BrakingKey(val current: AnalysisLapKey, val comparison: AnalysisLapKey)

    /** A painted map layer: its laps, the theme and the lap colours it was painted in. */
    private data class MapLayerKey(
        val layer: MapLayer,
        val current: AnalysisLapKey,
        val comparison: AnalysisLapKey,
        val dark: Boolean,
        val currentColor: Int,
        val comparisonColor: Int,
    )

    private val preferences = RecordingStorage.preferences(context)
    private val main = Handler(Looper.getMainLooper())
    private val requestIds = AtomicLong(ANALYSIS_REQUEST_BASE)

    // Read on the socket thread to know which channels a reply carries.
    private val inFlight = ConcurrentHashMap<Long, PendingLap>()
    private val pendingByLap = HashMap<AnalysisLapKey, PendingLap>()
    private val pendingDrivers = HashMap<Int, PendingDriver>()
    private val lapOrder = ArrayDeque<AnalysisLapKey>()
    private var generation = Long.MIN_VALUE
    private var deltaKey: DeltaKey? = null
    private var deltaRequestId = 0L
    // The current key's request reached the desktop, answered or not.
    private var deltaSent = false
    private var brakingKey: BrakingKey? = null
    private var brakingRequestId = 0L
    private var brakingSentAt = 0L
    private var mapLayerKey: MapLayerKey? = null
    private var mapLayerRequestId = 0L
    private var mapLayerSentAt = 0L

    var config by mutableStateOf(AnalysisConfig.fromJson(preferences.getString(PREF_ANALYSIS_CONFIG, null)))
        private set

    /** Compare against [compareLap] (0 = none) of [compareDriver] while following the desktop's lap. */
    var compareLap by mutableIntStateOf(0)
        private set
    var compareDriver by mutableIntStateOf(-1)
        private set

    /** Fixed comparison: two chosen laps instead of the desktop's current one. */
    var fixedMode by mutableStateOf(false)
        private set
    var lapA by mutableIntStateOf(0)
        private set
    var lapB by mutableIntStateOf(0)
        private set
    var lapADriver by mutableIntStateOf(-1)
        private set
    var lapBDriver by mutableIntStateOf(-1)
        private set

    /** Fetched laps of the current catalogue generation. */
    val laps = mutableStateMapOf<AnalysisLapKey, AnalysisLap>()

    /** Other drivers' lap catalogues of the current generation, by car index. */
    val driverLaps = mutableStateMapOf<Int, DriverLapCatalog>()

    var deltaCurve by mutableStateOf<AnalysisDeltaCurve?>(null)
        private set

    /** The braking zones of the laps last asked for with [syncBraking], once they arrive. */
    var brakingZones by mutableStateOf<BrakingZones?>(null)
        private set

    /** True while braking zones are asked for and not yet answered. */
    var brakingLoading by mutableStateOf(false)
        private set

    /** True when the source cannot find braking zones (an older desktop build). */
    var brakingUnavailable by mutableStateOf(false)
        private set

    /** The map layer last asked for with [syncMapLayer], painted per lap, once it arrives. */
    var mapLayerPaint by mutableStateOf<MapLayerPaint?>(null)
        private set

    /** True while a map layer is asked for and not yet answered. */
    var mapLayerLoading by mutableStateOf(false)
        private set

    /** True when the source cannot paint map layers (an older desktop build). */
    var mapLayerUnavailable by mutableStateOf(false)
        private set

    /** The chart settings sheet, opened from the app bar. */
    var settingsOpen by mutableStateOf(false)

    /** True once the desktop has refused a lap request (an older desktop build). */
    var lapDataUnavailable by mutableStateOf(false)
        private set

    fun updateConfig(transform: (AnalysisConfig) -> AnalysisConfig) {
        val next = transform(config)
        if (next == config) return
        config = next
        preferences.edit().putString(PREF_ANALYSIS_CONFIG, next.toJson()).apply()
    }

    fun selectCompareLap(lapNumber: Int, driver: Int) {
        compareLap = lapNumber
        compareDriver = driver
    }

    fun useFixedLaps(enabled: Boolean) {
        fixedMode = enabled
    }

    fun selectLapA(lapNumber: Int, driver: Int) {
        lapA = lapNumber
        lapADriver = driver
    }

    fun selectLapB(lapNumber: Int, driver: Int) {
        lapB = lapNumber
        lapBDriver = driver
    }

    /** Whether each lap can be given its own driver: a recording of every car, from a source that serves them. */
    fun driverChoiceAvailable(catalog: PlaybackCatalog): Boolean =
        catalog.active && catalog.driverIndex >= 0 && driverLapsSupported()

    /** [driver]'s laps, or null while another driver's catalogue is being fetched. */
    fun lapsOf(catalog: PlaybackCatalog, driver: Int): List<PlaybackLapInfo>? =
        if (driver < 0 || driver == catalog.driverIndex) catalog.laps else driverLaps[driver]?.laps

    fun fastestLapOf(catalog: PlaybackCatalog, driver: Int): Int =
        if (driver < 0 || driver == catalog.driverIndex) catalog.fastestLap else driverLaps[driver]?.fastestLap ?: 0

    /**
     * Makes sure each of [drivers]' lap catalogues is fetched. Idempotent, and
     * called on a slow tick like [sync] so a lost reply is asked for again.
     */
    fun syncDriverLaps(catalog: PlaybackCatalog, drivers: Collection<Int>) {
        resetIfNewCatalogue(catalog)
        if (!catalog.active) return
        val now = SystemClock.elapsedRealtime()
        for (driver in drivers) {
            if (driver < 0 || driver == catalog.driverIndex || driver in driverLaps) continue
            val pending = pendingDrivers[driver]
            if (pending != null && now - pending.sentAt < LAP_REQUEST_RETRY_MS) continue
            val request = PendingDriver(requestIds.incrementAndGet(), now)
            pendingDrivers[driver] = request
            if (!driverLapsRequester(request.requestId, driver)) pendingDrivers.remove(driver)
        }
    }

    /**
     * Makes sure every lap in [wanted] is fetched with at least [channels].
     * Idempotent; the page calls it whenever its needs change and on a slow
     * tick so a lost reply is asked for again.
     */
    fun sync(catalog: PlaybackCatalog, wanted: Collection<AnalysisLapKey>, channels: Set<String>) {
        resetIfNewCatalogue(catalog)
        if (!catalog.active) return
        val now = SystemClock.elapsedRealtime()
        for (key in wanted) {
            // Another driver's lap was chosen from that driver's catalogue.
            if (key.lap <= 0 || (key.driver < 0 && catalog.laps.none { it.lapNumber == key.lap })) continue
            val cached = laps[key]
            if (cached != null && cached.covers(channels)) continue
            val pending = pendingByLap[key]
            if (pending != null && pending.channels.containsAll(channels) &&
                now - pending.sentAt < LAP_REQUEST_RETRY_MS
            ) continue
            // Ask for everything the lap already had too, so the reply replaces it whole.
            val request = PendingLap(
                requestIds.incrementAndGet(),
                key,
                channels + (cached?.channels ?: emptySet()),
                now,
            )
            pending?.let { inFlight.remove(it.requestId) }
            inFlight[request.requestId] = request
            pendingByLap[key] = request
            if (!lapRequester(request.requestId, key.driver, key.lap, request.channels)) {
                inFlight.remove(request.requestId)
                pendingByLap.remove(key)
                lapDataUnavailable = store.sourceStatus.state == "connected" ||
                    store.sourceStatus.state == "paired"
            }
        }
    }

    /** The fetched delta curve, if it is the one between these two laps. */
    fun deltaCurveFor(current: AnalysisLapKey, comparison: AnalysisLapKey, sectorDelta: Boolean): AnalysisDeltaCurve? =
        deltaCurve?.takeIf { deltaKey == DeltaKey(current, comparison, sectorDelta) }

    /** Requests the delta curve for a pair of laps, or clears it for none. */
    fun syncDelta(catalog: PlaybackCatalog, current: AnalysisLapKey, comparison: AnalysisLapKey, sectorDelta: Boolean) {
        resetIfNewCatalogue(catalog)
        val key = if (catalog.active && catalog.deltaAvailable && current.lap > 0 && comparison.lap > 0) {
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
            currentLap = key.current.lap,
            comparisonLap = key.comparison.lap,
            sectorDelta = key.sectorDelta,
            currentDriver = key.current.driver,
            comparisonDriver = key.comparison.driver,
        )
        deltaRequestId = request.requestId
        deltaSent = deltaRequester(request)
        if (!deltaSent) deltaRequestId = 0L
    }

    /** The fetched braking zones, if they are the ones of these laps. */
    fun brakingZonesFor(current: AnalysisLapKey, comparison: AnalysisLapKey): BrakingZones? =
        brakingZones?.takeIf { brakingKey == brakingKeyOf(current, comparison) }

    private fun brakingKeyOf(current: AnalysisLapKey, comparison: AnalysisLapKey) =
        BrakingKey(current, if (comparison.lap > 0) comparison else AnalysisLapKey(-1, 0))

    /**
     * Requests the braking zones of [current], matched with [comparison]'s
     * when it is a lap, while [wanted]. Idempotent, and called on a slow tick
     * so a lost reply is asked for again.
     */
    fun syncBraking(catalog: PlaybackCatalog, current: AnalysisLapKey, comparison: AnalysisLapKey, wanted: Boolean) {
        resetIfNewCatalogue(catalog)
        val key = if (wanted && catalog.active && current.lap > 0) brakingKeyOf(current, comparison) else null
        val now = SystemClock.elapsedRealtime()
        if (key == brakingKey) {
            // Asked again only once a sent request has gone unanswered for a while.
            if (key == null || brakingZones != null) return
            if (brakingRequestId != 0L && now - brakingSentAt < LAP_REQUEST_RETRY_MS) return
        } else {
            brakingZones = null
            brakingKey = key
        }
        brakingRequestId = 0L
        brakingLoading = false
        if (key == null) return
        val request = BrakingZonesRequest(
            requestId = requestIds.incrementAndGet(),
            currentLap = key.current.lap,
            comparisonLap = key.comparison.lap,
            currentDriver = key.current.driver,
            comparisonDriver = key.comparison.driver,
        )
        if (brakingRequester(request)) {
            brakingRequestId = request.requestId
            brakingSentAt = now
            brakingLoading = true
            brakingUnavailable = false
        } else {
            brakingUnavailable = true
        }
    }

    private fun mapLayerKeyOf(
        layer: MapLayer,
        current: AnalysisLapKey,
        comparison: AnalysisLapKey,
        dark: Boolean,
        currentColor: Int,
        comparisonColor: Int,
    ) = MapLayerKey(
        layer,
        current,
        if (comparison.lap > 0) comparison else AnalysisLapKey(-1, 0),
        dark,
        currentColor and 0xffffff,
        comparisonColor and 0xffffff,
    )

    /** The fetched map layer, if it is the one of these laps, theme and colours. */
    fun mapLayerFor(
        layer: MapLayer,
        current: AnalysisLapKey,
        comparison: AnalysisLapKey,
        dark: Boolean,
        currentColor: Int,
        comparisonColor: Int,
    ): MapLayerPaint? = mapLayerPaint?.takeIf {
        mapLayerKey == mapLayerKeyOf(layer, current, comparison, dark, currentColor, comparisonColor)
    }

    /**
     * Requests [layer] painted for [current], and for [comparison] when it is
     * a lap, in the given theme and lap colours, while the layer needs libtnrp.
     * Idempotent, and called on a slow tick so a lost reply is asked for again.
     */
    fun syncMapLayer(
        catalog: PlaybackCatalog,
        layer: MapLayer,
        current: AnalysisLapKey,
        comparison: AnalysisLapKey,
        dark: Boolean,
        currentColor: Int,
        comparisonColor: Int,
    ) {
        resetIfNewCatalogue(catalog)
        val key = if (layer.native != null && catalog.active && current.lap > 0) {
            mapLayerKeyOf(layer, current, comparison, dark, currentColor, comparisonColor)
        } else {
            null
        }
        val now = SystemClock.elapsedRealtime()
        if (key == mapLayerKey) {
            // Asked again only once a sent request has gone unanswered for a while.
            if (key == null || mapLayerPaint != null) return
            if (mapLayerRequestId != 0L && now - mapLayerSentAt < LAP_REQUEST_RETRY_MS) return
        } else {
            mapLayerPaint = null
            mapLayerKey = key
        }
        mapLayerRequestId = 0L
        mapLayerLoading = false
        if (key == null) return
        val request = AnalysisMapRequest(
            requestId = requestIds.incrementAndGet(),
            layer = key.layer.native ?: return,
            currentLap = key.current.lap,
            comparisonLap = key.comparison.lap,
            currentDriver = key.current.driver,
            comparisonDriver = key.comparison.driver,
            dark = key.dark,
            currentColor = hexColor(key.currentColor),
            comparisonColor = hexColor(key.comparisonColor),
        )
        if (mapLayerRequester(request)) {
            mapLayerRequestId = request.requestId
            mapLayerSentAt = now
            mapLayerLoading = true
            mapLayerUnavailable = false
        } else {
            mapLayerUnavailable = true
        }
    }

    /** Socket or lap-reader thread: an `analysis_map` reply. */
    fun onAnalysisMap(row: JSONObject) {
        val requestId = row.optLong("requestId")
        val paint = MapLayerPaint.parse(row.optJSONObject("data"))
        main.post {
            if (requestId != mapLayerRequestId) return@post
            val layer = mapLayerKey?.layer ?: return@post
            mapLayerRequestId = 0L
            mapLayerLoading = false
            // An empty reply (nothing playing, an unreadable lap) shows as an unpainted map.
            mapLayerPaint = paint ?: MapLayerPaint(layer, null, null, 0.0, 0.0, 0.0, 0.0)
        }
    }

    /** Socket or lap-reader thread: a `braking_zones` reply. */
    fun onBrakingZones(row: JSONObject) {
        val requestId = row.optLong("requestId")
        val zones = BrakingZones.parse(row.optJSONObject("data"))
        main.post {
            if (requestId != brakingRequestId) return@post
            brakingRequestId = 0L
            brakingLoading = false
            // An empty reply (nothing playing, an unreadable lap) shows as no zones.
            brakingZones = zones ?: BrakingZones(emptyList(), emptyList(), emptyList())
        }
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
                it.currentLap == key.current.lap && it.comparisonLap == key.comparison.lap
            }
        }
    }

    /** Socket or lap-reader thread: a `driver_laps` reply, one driver's lap catalogue. */
    fun onDriverLaps(row: JSONObject) {
        val requestId = row.optLong("requestId")
        val driver = row.optInt("driverIndex", -1)
        val data = row.optJSONObject("data")
        val catalog = data?.let {
            val lapTimes = buildMap {
                val laps = it.optJSONArray("laps")
                if (laps != null) repeat(laps.length()) { index ->
                    val lap = laps.optJSONObject(index) ?: return@repeat
                    val lapNumber = lap.optInt("lapNum")
                    val lapTimeMs = lap.optInt("lapTimeMs")
                    if (lapNumber > 0 && lapTimeMs > 0) put(lapNumber, lapTimeMs)
                }
            }
            val laps = buildList {
                val blocks = it.optJSONArray("blocks")
                if (blocks != null) repeat(blocks.length()) { index ->
                    val block = blocks.optJSONObject(index) ?: return@repeat
                    val lapNumber = block.optInt("lapNum")
                    if (lapNumber <= 0) return@repeat
                    add(
                        PlaybackLapInfo(
                            lapNumber = lapNumber,
                            lapTimeMs = lapTimes[lapNumber] ?: 0,
                            startSessionTime = block.optDouble("startSessionTime", Double.NaN),
                            endSessionTime = block.optDouble("endSessionTime", Double.NaN),
                            tyreCompound = block.optInt("tyreCompound"),
                            visualCompound = block.optInt("visualCompound"),
                        ),
                    )
                }
            }
            DriverLapCatalog(driver, laps, it.optInt("fastestLapNum"))
        }
        main.post {
            if (pendingDrivers[driver]?.requestId != requestId) return@post
            // An empty reply keeps the request pending, so it is asked again
            // only after the retry interval.
            catalog ?: return@post
            pendingDrivers.remove(driver)
            driverLaps[driver] = catalog
        }
    }

    private fun installLap(request: PendingLap, lap: AnalysisLap?) {
        if (pendingByLap[request.key]?.requestId != request.requestId) return
        // An empty reply (nothing playing, unknown lap) keeps the request
        // pending, so it is asked again only after the retry interval.
        if (lap == null || lap.lapNumber != request.key.lap) return
        pendingByLap.remove(request.key)
        lapDataUnavailable = false
        laps[request.key] = lap
        lapOrder.remove(request.key)
        lapOrder.addLast(request.key)
        while (lapOrder.size > MAX_CACHED_LAPS) laps.remove(lapOrder.removeFirst())
    }

    private fun resetIfNewCatalogue(catalog: PlaybackCatalog) {
        if (catalog.generation == generation) return
        generation = catalog.generation
        laps.clear()
        lapOrder.clear()
        pendingByLap.clear()
        pendingDrivers.clear()
        driverLaps.clear()
        inFlight.clear()
        deltaKey = null
        deltaRequestId = 0L
        deltaSent = false
        deltaCurve = null
        brakingKey = null
        brakingRequestId = 0L
        brakingLoading = false
        brakingZones = null
        mapLayerKey = null
        mapLayerRequestId = 0L
        mapLayerLoading = false
        mapLayerPaint = null
        // Chosen laps belonged to the previous recording or driver.
        compareLap = 0
        compareDriver = -1
        lapA = 0
        lapB = 0
        lapADriver = -1
        lapBDriver = -1
    }
}
