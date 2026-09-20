package com.tracknrace.android

import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.roundToInt

private const val MAX_LIVE_LAP_PROGRESS_SAMPLES = 20_000
private const val MAX_PLAYBACK_DELTA_CURVES = 8

// Car indices are 0..21 on the wire (24-slot arrays); anything above is the
// game's 255 "no such car" marker, e.g. a spectator's player index.
private const val MAX_CARS = 24

private data class LiveLapSample(
    val sessionTime: Double,
    val lapNumber: Int,
    val lastLapMs: Int,
    val currentLapMs: Int,
    val lapDistanceM: Double,
    val sector1Ms: Int,
    val sector2Ms: Int,
    val sector: Int,
    val invalid: Boolean,
)

private val BLANK_LIVE_LAP = LiveLapSample(
    sessionTime = Double.NaN,
    lapNumber = 0,
    lastLapMs = 0,
    currentLapMs = 0,
    lapDistanceM = Double.NaN,
    sector1Ms = 0,
    sector2Ms = 0,
    sector = 0,
    invalid = false,
)

/** One car's timing fields as a patch row carried them; null means unchanged. */
private class TimingCarPatch(
    val index: Int,
    val position: Int?,
    val lapNumber: Int?,
    val currentLapMs: Int?,
    val lastLapMs: Int?,
    val gapMs: Int?,
    val pitStatus: Int?,
    val lapInvalid: Boolean?,
    val penaltiesSeconds: Int?,
    val driveThroughPenalties: Int?,
    val stopGoPenalties: Int?,
    val resultStatus: Int?,
)

private class TyreStatusPatch(
    val index: Int,
    val actualCompound: Int?,
    val visualCompound: Int?,
    val ageLaps: Int?,
    /** False when that car's tyre state stopped being readable. */
    val available: Boolean?,
)

private fun mergeTimingCars(
    current: List<TimingCarEntry>,
    patches: List<TimingCarPatch>,
): List<TimingCarEntry> {
    val merged = LinkedHashMap<Int, TimingCarEntry>()
    for (car in current) merged[car.index] = car
    for (patch in patches) {
        val base = merged[patch.index] ?: TimingCarEntry(
            index = patch.index,
            position = 0,
            lapNumber = 0,
            currentLapMs = 0,
            lastLapMs = 0,
            gapMs = 0,
            pitStatus = 0,
            lapInvalid = false,
            penaltiesSeconds = 0,
            driveThroughPenalties = 0,
            stopGoPenalties = 0,
            resultStatus = 0,
        )
        merged[patch.index] = base.copy(
            position = patch.position ?: base.position,
            lapNumber = patch.lapNumber ?: base.lapNumber,
            currentLapMs = patch.currentLapMs ?: base.currentLapMs,
            lastLapMs = patch.lastLapMs ?: base.lastLapMs,
            gapMs = patch.gapMs ?: base.gapMs,
            pitStatus = patch.pitStatus ?: base.pitStatus,
            lapInvalid = patch.lapInvalid ?: base.lapInvalid,
            penaltiesSeconds = patch.penaltiesSeconds ?: base.penaltiesSeconds,
            driveThroughPenalties = patch.driveThroughPenalties ?: base.driveThroughPenalties,
            stopGoPenalties = patch.stopGoPenalties ?: base.stopGoPenalties,
            resultStatus = patch.resultStatus ?: base.resultStatus,
        )
    }
    return merged.values.toList()
}

private data class LapProgressSample(
    val elapsedMs: Int,
    val distanceM: Double,
)

private data class CompletedLap(
    val lapMs: Int,
    val sector1Ms: Int,
    val sector2Ms: Int,
    val progress: List<LapProgressSample>,
)

private data class PlaybackLapMeta(
    val lapNumber: Int,
    val lapTimeMs: Int,
    val startSessionTime: Double,
    val endSessionTime: Double,
)

private data class DeltaSample(
    val distanceM: Double,
    val deltaSeconds: Double,
)

private data class LapDeltaCurve(
    val currentLap: Int,
    val comparisonLap: Int,
    val sectors: List<List<DeltaSample>>,
    val progress: List<PlaybackProgressSample>,
)

private data class PlaybackProgressSample(
    val sessionTime: Double,
    val elapsedMs: Int,
    val distanceM: Double,
)

private data class LapDeltaSummary(
    val lapSeconds: Double?,
    val sector1Seconds: Double?,
    val sector2Seconds: Double?,
    val sector3Seconds: Double?,
)

/**
 * Thread boundary between libtnrp/OkHttp and Compose.
 *
 * Hot rows never touch Compose state. Worker threads decode a whole native
 * batch to its latest telemetry sample and publish one immutable atomic value.
 * The dashboard samples hot telemetry and lap comparison values on the display
 * frame clock. Other cold rows are parsed off the UI thread and applied to
 * snapshot state on the main thread.
 */
internal class TelemetryStore {
    private val main = Handler(Looper.getMainLooper())
    private val hot = AtomicReference(HotTelemetry())
    private val mapPositions = AtomicReference(MapPositions())
    private val hotRows = AtomicLong()
    private val malformedReported = AtomicBoolean()
    private val messageIds = AtomicLong()

    // Written by whichever source thread is delivering rows (libtnrp or OkHttp).
    @Volatile private var playerIndex = -1
    // Merge base for lap rows; main thread only, like the comparison state.
    private var liveLapState = BLANK_LIVE_LAP
    private var previousLiveLap: LiveLapSample? = null
    private var currentLiveLapNumber = 0
    private val currentLiveProgress = mutableListOf<LapProgressSample>()
    private var fastestLiveLap: CompletedLap? = null
    private var trackLengthM = 0.0
    private var playbackActive = false
    private var playbackFastestLap = 0
    private var playbackDeltaAvailable = false
    private var playbackLaps: List<PlaybackLapMeta> = emptyList()
    private var playbackSelectedLap = 0
    private var playbackCursorSessionTime = Double.NaN
    private var playbackCursorSample: LiveLapSample? = null
    private var playbackRequestId = 0L
    private var activePlaybackRequest: PlaybackLapDeltaRequest? = null
    private val playbackDeltaCache = mutableMapOf<Pair<Int, Int>, LapDeltaCurve>()
    private val playbackDeltaUnavailable = mutableSetOf<Pair<Int, Int>>()
    private var lapDeltaRequester: ((PlaybackLapDeltaRequest) -> Boolean)? = null

    var cold by mutableStateOf(DashboardColdState())
        private set
    private val lapComparison = AtomicReference(DashboardLapComparisonState())
    var timing by mutableStateOf(TimingTowerState())
        private set
    var settings by mutableStateOf(AndroidSettings())
        private set
    var sourceStatus by mutableStateOf(SourceStatus())
        private set
    var pairingBusy by mutableStateOf(false)
        private set
    var pairingSuccessId by mutableStateOf(0L)
        private set
    var message by mutableStateOf<UiMessage?>(null)
        private set
    val discoveredDesktops = mutableStateListOf<DiscoveredDesktop>()

    /**
     * Car index whose data the stream currently describes: the player live, or
     * the driver chosen on the desktop while a V6 recording is playing back.
     * libtnrp stamps that choice into `player_idx` on every projected row.
     */
    var selectedDriverIndex by mutableIntStateOf(-1)
        private set

    /** Roster name for [selectedDriverIndex]; null until Participants arrives. */
    val selectedDriverName: String?
        get() = timing.drivers[selectedDriverIndex]?.name

    // What the desktop last stated, and the driver it stated it for. During V6
    // playback the roster cannot answer this: a rival's restriction is an
    // absence of chunks, and the participants row replays the recorded setting
    // rather than the one in force at the cursor.
    private var statedRestriction by mutableStateOf<Boolean?>(null)
    private var statedRestrictionDriver by mutableIntStateOf(-1)
    // Set once a V6 recording is playing on the desktop, which is the only
    // situation where a stated restriction is required.
    private var v6PlaybackActive by mutableStateOf(false)

    /**
     * Whether the displayed driver's private telemetry is withheld, or null
     * while that is genuinely unknown. Null must be shown as "unknown", never
     * collapsed into "public".
     */
    val selectedDriverRestricted: Boolean?
        get() {
            if (statedRestrictionDriver >= 0 && statedRestrictionDriver == selectedDriverIndex) {
                return statedRestriction
            }
            // Playback owes a stated value; until it arrives this is unknown.
            if (v6PlaybackActive) return null
            // Live (direct or paired): the roster's setting is the current one,
            // exactly as on the desktop. timing.playerIndex is only populated on
            // pages that subscribe to the timing row, but live every row
            // describes the receiving player, so the selected driver is them.
            val player = if (timing.playerIndex >= 0) timing.playerIndex else selectedDriverIndex
            return timing.restrictionOf(selectedDriverIndex, player)
        }

    /**
     * True when the desktop owes a `driver_restriction` the app has not
     * received, so the caller should ask again.
     */
    fun needsDriverRestrictionRefresh(): Boolean =
        v6PlaybackActive && selectedDriverIndex >= 0 &&
            statedRestrictionDriver != selectedDriverIndex

    fun latestHot(): HotTelemetry = hot.get()
    fun latestMapPositions(): MapPositions = mapPositions.get()
    fun latestLapComparison(): DashboardLapComparisonState = lapComparison.get()
    fun totalHotRows(): Long = hotRows.get()

    fun acceptBinary(bytes: ByteArray) {
        val result = BinaryTelemetryDecoder.decodeLatest(bytes)
        result.latest?.let(hot::lazySet)
        result.latestPositions?.let(mapPositions::lazySet)
        if (result.telemetryRows > 0) hotRows.addAndGet(result.telemetryRows.toLong())
        if (result.malformed && malformedReported.compareAndSet(false, true)) {
            showMessage("A malformed native telemetry batch was discarded")
        }
    }

    fun acceptColdRow(json: String) {
        val row = try {
            JSONObject(json)
        } catch (_: Exception) {
            return
        }

        // Most V6 rows name the player's car index; the player-scoped rows the
        // dashboard shows (tyre sets) are matched against it below.
        row.optionalInt("player_idx")?.takeIf { it in 0 until MAX_CARS }?.let { setSelectedDriver(it) }

        when (row.optString("type")) {
            // A V6 recording arrives as one field group per row, so the hot sample
            // merges them. Live and paired-live telemetry stay on the binary path.
            "telemetry" -> {
                if (row.optionalBoolean("available") == false) {
                    hot.set(HotTelemetry())
                } else {
                    hot.updateAndGet { it.withPatch(row) }
                }
            }

            "lap" -> {
                val sessionTime = row.optionalDouble("session_time")
                val lapNumber = row.optionalInt("lap_num")
                val lastLapMs = row.optionalInt("last_lap_ms")
                val currentLapMs = row.optionalInt("current_lap_ms")
                val lapDistanceM = row.optionalDouble("lap_distance_m")
                val sector1Ms = row.optionalInt("s1_ms")
                val sector2Ms = row.optionalInt("s2_ms")
                val sector = row.optionalInt("sector")
                val invalid = row.optionalBoolean("lap_invalid")
                val position = row.optionalInt("position")
                post {
                    // Merge into the last sample: a patch omits what did not change.
                    val previous = liveLapState
                    val sample = LiveLapSample(
                        sessionTime = sessionTime ?: previous.sessionTime,
                        lapNumber = lapNumber ?: previous.lapNumber,
                        lastLapMs = lastLapMs ?: previous.lastLapMs,
                        currentLapMs = currentLapMs ?: previous.currentLapMs,
                        lapDistanceM = lapDistanceM ?: previous.lapDistanceM,
                        sector1Ms = sector1Ms ?: previous.sector1Ms,
                        sector2Ms = sector2Ms ?: previous.sector2Ms,
                        sector = sector ?: previous.sector,
                        invalid = invalid ?: previous.invalid,
                    )
                    liveLapState = sample
                    cold = cold.copy(
                        position = position ?: cold.position,
                        lapNumber = sample.lapNumber,
                        currentLapMs = sample.currentLapMs,
                        lastLapMs = sample.lastLapMs,
                        lapInvalid = sample.invalid,
                    )
                    if (playbackActive) {
                        updatePlaybackReference(sample)
                    } else {
                        updateLiveLapComparison(sample)
                    }
                }
            }

            "playback_loaded" -> post {
                resetAllLapComparison()
                playbackActive = row.optBoolean("ok")
            }

            "playback_lap_blocks" -> {
                // Sent again whenever the desktop switches driver, and before
                // that driver's first projected row, so tyre sets and the top
                // bar follow the switch immediately.
                // A driver index here means a V6 recording, the only case where
                // the desktop owes an explicit restriction.
                val playbackDriver = row.optionalInt("playbackDriverIndex")
                    ?.takeIf { it in 0 until MAX_CARS }
                playbackDriver?.let { setSelectedDriver(it) }
                post { v6PlaybackActive = playbackDriver != null }
                val lapTimes = buildMap {
                    val laps = row.optJSONArray("laps")
                    if (laps != null) repeat(laps.length()) { index ->
                        val lap = laps.optJSONObject(index) ?: return@repeat
                        val lapNumber = lap.optInt("lapNum")
                        val lapTimeMs = lap.optInt("lapTimeMs")
                        if (lapNumber > 0 && lapTimeMs > 0) put(lapNumber, lapTimeMs)
                    }
                }
                val laps = buildList {
                    val blocks = row.optJSONArray("blocks")
                    if (blocks != null) repeat(blocks.length()) { index ->
                        val block = blocks.optJSONObject(index) ?: return@repeat
                        val lapNumber = block.optInt("lapNum")
                        if (lapNumber <= 0) return@repeat
                        add(
                            PlaybackLapMeta(
                                lapNumber = lapNumber,
                                lapTimeMs = lapTimes[lapNumber] ?: 0,
                                startSessionTime = block.optDouble(
                                    "startSessionTime",
                                    Double.NaN,
                                ),
                                endSessionTime = block.optDouble(
                                    "endSessionTime",
                                    Double.NaN,
                                ),
                            ),
                        )
                    }
                }
                val fastestLap = row.optInt("fastestLapNum")
                val deltaAvailable = row.optBoolean("deltaAvailable")
                val playbackTrackLengthM = row.optDouble("trackLengthM")
                post {
                    installPlaybackMetadata(
                        laps,
                        fastestLap,
                        deltaAvailable,
                        playbackTrackLengthM,
                    )
                }
            }

            "lap_delta" -> {
                val requestId = row.optLong("requestId")
                val data = row.optJSONObject("data")
                val currentLap = data?.optInt("currentLapNum") ?: 0
                val comparisonLap = data?.optInt("comparisonLapNum") ?: 0
                val curve = data?.let(::readLapDeltaCurve)
                post {
                    acceptPlaybackDelta(
                        requestId,
                        currentLap,
                        comparisonLap,
                        curve,
                    )
                }
            }

            "playback_state" -> {
                val startTime = row.optDouble("start_time", Double.NaN)
                val currentTime = row.optDouble("current_time", Double.NaN)
                if (startTime.isFinite() && currentTime.isFinite()) {
                    post { updatePlaybackCursor(startTime + currentTime) }
                }
            }

            // The desktop states this per selected driver, because restricted
            // data arrives as missing rows rather than as a value. driverIndex
            // -1 is the reply to a request made while nothing is playing.
            "driver_restriction" -> {
                val driver = row.optionalInt("driverIndex") ?: -1
                val restricted = row.optionalBoolean("restricted")
                val known = row.optionalBoolean("known") ?: false
                post {
                    if (driver < 0) {
                        statedRestriction = null
                        statedRestrictionDriver = -1
                        v6PlaybackActive = false
                    } else {
                        statedRestriction = if (known) restricted else null
                        statedRestrictionDriver = driver
                    }
                }
            }

            "playback_close" -> post {
                resetAllLapComparison()
                statedRestriction = null
                statedRestrictionDriver = -1
                v6PlaybackActive = false
            }

            "status" -> {
                // Restricted telemetry is an explicit {"available":false}, not an
                // omission: it must clear the values, not leave the last ones up.
                val unavailable = row.optionalBoolean("available") == false
                val ersPercent = row.optionalDouble("ers_pct")?.roundToInt()
                val ersMode = row.optionalInt("ers_mode")
                val fuelKg = row.optionalDouble("fuel_kg")
                val fuelLaps = row.optionalDouble("fuel_laps")
                val brakeBias = row.optionalInt("front_brake_bias")
                val tyreCompound = row.optionalInt("visual_compound")
                val tyreAgeLaps = row.optionalInt("tyre_age_laps")
                val carriesFields = ersPercent != null || ersMode != null || fuelKg != null ||
                    fuelLaps != null || brakeBias != null || tyreCompound != null ||
                    tyreAgeLaps != null
                post {
                    cold = when {
                        unavailable -> cold.copy(statusAvailable = false)
                        !carriesFields -> cold
                        else -> cold.copy(
                            statusAvailable = true,
                            ersPercent = ersPercent ?: cold.ersPercent,
                            ersMode = ersMode ?: cold.ersMode,
                            fuelKg = fuelKg ?: cold.fuelKg,
                            fuelLaps = fuelLaps ?: cold.fuelLaps,
                            brakeBias = brakeBias ?: cold.brakeBias,
                            tyreCompound = tyreCompound ?: cold.tyreCompound,
                            tyreAgeLaps = tyreAgeLaps ?: cold.tyreAgeLaps,
                        )
                    }
                }
            }

            "damage" -> {
                val unavailable = row.optionalBoolean("available") == false
                val wearFl = row.optionalDouble("tyre_wear_fl")?.toFloat()
                val wearFr = row.optionalDouble("tyre_wear_fr")?.toFloat()
                val wearRl = row.optionalDouble("tyre_wear_rl")?.toFloat()
                val wearRr = row.optionalDouble("tyre_wear_rr")?.toFloat()
                val carriesWear = wearFl != null || wearFr != null || wearRl != null ||
                    wearRr != null
                post {
                    cold = when {
                        unavailable -> cold.copy(tyreWearAvailable = false)
                        !carriesWear -> cold
                        else -> cold.copy(
                            tyreWearFl = wearFl ?: cold.tyreWearFl,
                            tyreWearFr = wearFr ?: cold.tyreWearFr,
                            tyreWearRl = wearRl ?: cold.tyreWearRl,
                            tyreWearRr = wearRr ?: cold.tyreWearRr,
                            tyreWearAvailable = true,
                        )
                    }
                }
            }

            // Indexed/legacy TNRD playback keeps Positions as JSON rather than
            // placing it in the packed telemetry lanes. Live direct/paired
            // positions still arrive through acceptBinary().
            "positions" -> {
                val cars = row.optJSONArray("cars")
                val carCount = cars?.length() ?: 0
                val xByCar = DoubleArray(carCount)
                val zByCar = DoubleArray(carCount)
                if (cars != null) {
                    repeat(carCount) { arrayIndex ->
                        val car = cars.optJSONObject(arrayIndex) ?: return@repeat
                        val carIndex = car.optInt("idx", arrayIndex)
                        if (carIndex in 0 until carCount) {
                            xByCar[carIndex] = car.optDouble("x")
                            zByCar[carIndex] = car.optDouble("z")
                        }
                    }
                }
                mapPositions.lazySet(
                    MapPositions(
                        playerIndex = row.optInt("player_idx", -1),
                        xByCar = xByCar,
                        zByCar = zByCar,
                    ),
                )
            }

            "session" -> {
                val sessionType = row.optionalInt("session_type")
                val totalLaps = row.optInt("total_laps")
                val trackId = row.optInt("track_id", -1)
                val sessionTrackLengthM = row.optDouble("track_length_m")
                post {
                    val changedSession =
                        (cold.trackId >= 0 && trackId >= 0 && cold.trackId != trackId) ||
                            (cold.sessionType != null && sessionType != null &&
                                cold.sessionType != sessionType)
                    if (changedSession && !playbackActive) resetLiveLapComparison()
                    if (sessionTrackLengthM > 0.0) trackLengthM = sessionTrackLengthM
                    cold = cold.copy(
                        totalLaps = totalLaps,
                        sessionType = sessionType,
                        trackId = trackId,
                    )
                }
            }

            "tyre_sets" -> {
                val carIdx = row.optionalInt("car_idx")
                // Rows projected from a V6 recording are scoped to the driver the
                // desktop selected, and say so before any row names the player.
                val v6Projected = row.has("_v6_type")
                if (!ownsTyreSets(carIdx, playerIndex, v6Projected)) return
                if (v6Projected && carIdx != null && carIdx in 0 until MAX_CARS) {
                    setSelectedDriver(carIdx)
                }
                // The desktop rebroadcasts this type when the driver's access
                // changes. A withdrawal carries no "sets", and the sets it last
                // sent are no longer readable, so drop them.
                if (row.optionalBoolean("available") == false) {
                    post { cold = cold.copy(tyreSets = emptyList()) }
                    return
                }
                if (row.optJSONArray("sets") == null) return
                val sets = row.optJSONArray("sets")?.let { array ->
                    List(array.length()) { index ->
                        val set = array.optJSONObject(index) ?: JSONObject()
                        TyreSetEntry(
                            index = set.optInt("idx", index),
                            actualCompound = set.optInt("actual_compound"),
                            visualCompound = set.optInt("visual_compound"),
                            wear = set.optDouble("wear").toFloat(),
                            available = set.optBoolean("available"),
                            recommendedSession = set.optInt("recommended_session"),
                            lifeSpan = set.optInt("life_span"),
                            usableLife = set.optInt("usable_life"),
                            lapDeltaMs = set.optInt("lap_delta_ms"),
                            fitted = set.optBoolean("fitted"),
                        )
                    }
                }.orEmpty()
                post { cold = cold.copy(tyreSets = sets) }
            }

            "timing" -> {
                val patches = row.optJSONArray("cars")?.let { array ->
                    List(array.length()) { arrayIndex ->
                        val car = array.optJSONObject(arrayIndex) ?: JSONObject()
                        TimingCarPatch(
                            index = car.optInt("idx", arrayIndex),
                            position = car.optionalInt("position"),
                            lapNumber = car.optionalInt("lap_num"),
                            currentLapMs = car.optionalInt("current_lap_ms"),
                            lastLapMs = car.optionalInt("last_lap_ms"),
                            gapMs = car.optionalInt("gap_ms"),
                            pitStatus = car.optionalInt("pit_status"),
                            lapInvalid = car.optionalBoolean("lap_invalid"),
                            penaltiesSeconds = car.optionalInt("penalties_s"),
                            driveThroughPenalties = car.optionalInt("num_dt_pens"),
                            stopGoPenalties = car.optionalInt("num_sg_pens"),
                            resultStatus = car.optionalInt("result_status"),
                        )
                    }
                }.orEmpty()
                val rowPlayer = row.optionalInt("player_idx")?.takeIf { it >= 0 }
                // A V6 patch names one car and merges into the tower; a complete
                // row is the whole grid and replaces it.
                val partial = row.has("_v6_type")
                post {
                    timing = timing.copy(
                        playerIndex = rowPlayer ?: timing.playerIndex,
                        cars = mergeTimingCars(if (partial) timing.cars else emptyList(), patches),
                    )
                }
            }

            "participants" -> {
                val drivers = row.optJSONArray("drivers")?.let { array ->
                    buildMap {
                        repeat(array.length()) { arrayIndex ->
                            val driver = when (val value = array.opt(arrayIndex)) {
                                is JSONObject -> value
                                is String -> runCatching { JSONObject(value) }.getOrNull()
                                else -> null
                            } ?: return@repeat
                            val index = driver.optInt("idx", arrayIndex)
                            val name = driver.optString("name").trim()
                            if (name.isEmpty()) return@repeat
                            put(
                                index,
                                TimingDriver(
                                    index = index,
                                    name = name,
                                    raceNumber = driver.optInt("race_number"),
                                    teamColor = driver.optString("livery_color", "#8e8e8e"),
                                    yourTelemetry = driver.optionalInt("your_telemetry"),
                                ),
                            )
                        }
                    }
                }.orEmpty()
                post {
                    // A malformed/empty roster is not a successful Participants
                    // update. Keep a valid roster if one is already displayed and
                    // leave the missing flag set so paired mode retries after 3 s.
                    if (drivers.isNotEmpty()) {
                        timing = timing.copy(drivers = drivers, hasParticipants = true)
                    } else if (timing.drivers.isEmpty()) {
                        timing = timing.copy(hasParticipants = false)
                    }
                }
            }

            "all_status" -> {
                val patches = row.optJSONArray("cars")?.let { array ->
                    List(array.length()) { arrayIndex ->
                        val car = array.optJSONObject(arrayIndex) ?: JSONObject()
                        TyreStatusPatch(
                            index = car.optInt("idx", arrayIndex),
                            actualCompound = car.optionalInt("tyre_compound"),
                            visualCompound = car.optionalInt("visual_compound"),
                            ageLaps = car.optionalInt("tyre_age_laps"),
                            available = car.optionalBoolean("available"),
                        )
                    }
                }.orEmpty()
                val partial = row.has("_v6_type")
                post {
                    val merged: MutableMap<Int, TimingTyreStatus> = if (partial) {
                        timing.tyreStatuses.toMutableMap()
                    } else {
                        mutableMapOf()
                    }
                    for (patch in patches) {
                        // The desktop rebroadcasts the type when a driver's
                        // access changes, so the tower drops the compound and
                        // age it last saw rather than showing them as current.
                        if (patch.available == false) {
                            merged.remove(patch.index)
                            continue
                        }
                        // Other all-car status fields (DRS, fuel, ...) are not shown
                        // here; a patch without tyre fields is not a tyre status.
                        if (patch.actualCompound == null && patch.visualCompound == null &&
                            patch.ageLaps == null
                        ) continue
                        val base = merged[patch.index] ?: TimingTyreStatus(0, 0, 0)
                        merged[patch.index] = base.copy(
                            actualCompound = patch.actualCompound ?: base.actualCompound,
                            visualCompound = patch.visualCompound ?: base.visualCompound,
                            ageLaps = patch.ageLaps ?: base.ageLaps,
                        )
                    }
                    timing = timing.copy(tyreStatuses = merged)
                }
            }

            "protocol_context" -> {
                setSelectedDriver(-1)
                post {
                    statedRestriction = null
                    statedRestrictionDriver = -1
                    v6PlaybackActive = false
                }
                mapPositions.lazySet(MapPositions())
                val year = row.optionalInt("protocol_year")
                val formula = row.optionalInt("formula")
                post {
                    resetAllLapComparison()
                    cold = cold.copy(
                        protocolYear = year,
                        formula = formula,
                        aeroMode = resolveAeroMode(year, formula, null),
                        labels = emptyMap(),
                        trackId = -1,
                        sessionType = null,
                        tyreSets = emptyList(),
                        statusAvailable = false,
                        tyreWearFl = 0f,
                        tyreWearFr = 0f,
                        tyreWearRl = 0f,
                        tyreWearRr = 0f,
                        tyreWearAvailable = false,
                    )
                    timing = TimingTowerState(
                        drivers = timing.drivers,
                        hasParticipants = timing.hasParticipants,
                    )
                }
            }

            "timeline_reset" -> {
                mapPositions.lazySet(MapPositions())
                post {
                    resetPlaybackCursorComparison()
                    // A seek/reset may be delivered after its reconstructed
                    // Participants row. Keep that roster; a real game-session
                    // change has its own participants_reset signal.
                    timing = TimingTowerState(
                        drivers = timing.drivers,
                        hasParticipants = timing.hasParticipants,
                    )
                }
            }

            "participants_reset" -> {
                setSelectedDriver(-1)
                post {
                    resetAllLapComparison()
                    timing = timing.copy(drivers = emptyMap(), hasParticipants = false)
                }
            }

            "protocol_status" -> {
                val year = row.optionalInt("active_format") ?: row.optionalInt("detected_format")
                val formula = row.optionalInt("formula")
                val fallbackAero = row.optString("aero_mode").takeIf { it.isNotEmpty() }
                val labelsObject = row.optJSONObject("labels")
                val labels = buildMap {
                    if (labelsObject != null) {
                        for (key in labelsObject.keys()) {
                            val value = labelsObject.opt(key)
                            if (value is String) put(key, value)
                        }
                    }
                }
                post {
                    cold = cold.copy(
                        protocolYear = year,
                        formula = formula,
                        aeroMode = resolveAeroMode(year, formula, fallbackAero),
                        labels = labels,
                    )
                }
            }

            "recording_error" -> {
                val operation = row.optString("operation", "save")
                val detail = row.optString("message", "Unknown error")
                updateSource("error", "Recording $operation failed: $detail")
                showMessage("Recording $operation failed: $detail")
            }
        }
    }

    private fun setSelectedDriver(index: Int) {
        if (playerIndex == index) return
        playerIndex = index
        // What is on screen belongs to the previous driver, and a patch stream
        // only carries changes, so start the new driver from blank. The desktop
        // re-sends this driver's current state right after switching.
        hot.set(HotTelemetry())
        post {
            selectedDriverIndex = index
            liveLapState = BLANK_LIVE_LAP
            cold = cold.copy(
                statusAvailable = false,
                tyreWearAvailable = false,
                tyreSets = emptyList(),
            )
        }
    }

    fun updateSettings(value: AndroidSettings) = post { settings = value }

    fun updateSource(state: String, detail: String? = null) = post {
        sourceStatus = SourceStatus(state, detail)
    }

    fun updatePairingBusy(value: Boolean) = post { pairingBusy = value }

    fun setLapDeltaRequester(requester: ((PlaybackLapDeltaRequest) -> Boolean)?) {
        lapDeltaRequester = requester
    }

    fun resetLapComparison() = post { resetAllLapComparison() }

    fun notifyPairingSucceeded() = post { pairingSuccessId++ }

    fun clearDiscovery() = post { discoveredDesktops.clear() }

    fun discovered(value: DiscoveredDesktop) = post {
        val index = discoveredDesktops.indexOfFirst { it.serverId == value.serverId }
        if (index >= 0) discoveredDesktops[index] = value else discoveredDesktops.add(value)
    }

    fun showMessage(text: String) = post {
        message = UiMessage(messageIds.incrementAndGet(), text)
    }

    private fun post(block: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) block() else main.post(block)
    }

    private fun publishLapComparison(value: DashboardLapComparisonState) {
        if (lapComparison.get() != value) lapComparison.lazySet(value)
    }

    private fun resetLiveLapComparison() {
        liveLapState = BLANK_LIVE_LAP
        previousLiveLap = null
        currentLiveLapNumber = 0
        currentLiveProgress.clear()
        fastestLiveLap = null
        publishLapComparison(DashboardLapComparisonState())
    }

    private fun resetAllLapComparison() {
        resetLiveLapComparison()
        playbackActive = false
        playbackFastestLap = 0
        playbackDeltaAvailable = false
        playbackLaps = emptyList()
        playbackSelectedLap = 0
        playbackCursorSessionTime = Double.NaN
        playbackCursorSample = null
        activePlaybackRequest = null
        playbackDeltaCache.clear()
        playbackDeltaUnavailable.clear()
        trackLengthM = 0.0
    }

    private fun resetPlaybackCursorComparison() {
        previousLiveLap = null
        activePlaybackRequest = null
        playbackSelectedLap = 0
        playbackCursorSessionTime = Double.NaN
        playbackCursorSample = null
        if (playbackActive) {
            val fastestTime = playbackLaps.firstOrNull {
                it.lapNumber == playbackFastestLap
            }?.lapTimeMs ?: 0
            publishLapComparison(DashboardLapComparisonState(fastestLapMs = fastestTime))
        } else {
            resetLiveLapComparison()
        }
    }

    private fun updateLiveLapComparison(sample: LiveLapSample) {
        var previous = previousLiveLap
        if (currentLiveLapNumber > 0 && sample.lapNumber < currentLiveLapNumber) {
            resetLiveLapComparison()
            previous = null
        }

        if (currentLiveLapNumber == 0 || sample.lapNumber != currentLiveLapNumber) {
            if (previous != null && sample.lapNumber == currentLiveLapNumber + 1 &&
                !previous.invalid && previous.sector1Ms > 0 && previous.sector2Ms > 0 &&
                sample.lastLapMs > previous.sector1Ms + previous.sector2Ms &&
                sample.lastLapMs < 300_000) {
                val completed = CompletedLap(
                    lapMs = sample.lastLapMs,
                    sector1Ms = previous.sector1Ms,
                    sector2Ms = previous.sector2Ms,
                    progress = buildCompletedProgress(currentLiveProgress, sample.lastLapMs),
                )
                if (fastestLiveLap == null || completed.lapMs < fastestLiveLap!!.lapMs) {
                    fastestLiveLap = completed
                }
            }
            currentLiveLapNumber = sample.lapNumber
            currentLiveProgress.clear()
        }

        appendLiveProgress(sample)
        previousLiveLap = sample
        publishLiveLapComparison(sample)
    }

    private fun appendLiveProgress(sample: LiveLapSample) {
        if (sample.currentLapMs < 0 || !sample.lapDistanceM.isFinite() ||
            currentLiveProgress.size >= MAX_LIVE_LAP_PROGRESS_SAMPLES) return
        currentLiveProgress.add(
            LapProgressSample(
                elapsedMs = sample.currentLapMs,
                distanceM = sample.lapDistanceM,
            ),
        )
    }

    private fun publishLiveLapComparison(sample: LiveLapSample) {
        val fastest = fastestLiveLap
        if (fastest == null) {
            publishLapComparison(DashboardLapComparisonState())
            return
        }
        val comparisonElapsed = interpolateElapsed(fastest.progress, sample.lapDistanceM)
        if (comparisonElapsed == null || sample.currentLapMs < 0) {
            publishLapComparison(DashboardLapComparisonState(fastestLapMs = fastest.lapMs))
            return
        }

        val lapDelta = sample.currentLapMs / 1000.0 - comparisonElapsed
        var sector1Delta: Double? = null
        var sector2Delta: Double? = null
        var sector3Delta: Double? = null
        when (sample.sector) {
            0 -> sector1Delta = lapDelta
            1 -> {
                sector1Delta = deltaSeconds(sample.sector1Ms, fastest.sector1Ms)
                sector2Delta = sector1Delta?.let { lapDelta - it }
            }
            2 -> {
                sector1Delta = deltaSeconds(sample.sector1Ms, fastest.sector1Ms)
                sector2Delta = deltaSeconds(sample.sector2Ms, fastest.sector2Ms)
                if (sector1Delta != null && sector2Delta != null) {
                    sector3Delta = lapDelta - sector1Delta - sector2Delta
                }
            }
        }
        publishLapComparison(DashboardLapComparisonState(
            fastestLapMs = fastest.lapMs,
            lapDeltaSeconds = lapDelta,
            sector1DeltaSeconds = sector1Delta,
            sector2DeltaSeconds = sector2Delta,
            sector3DeltaSeconds = sector3Delta,
        ))
    }

    private fun installPlaybackMetadata(
        laps: List<PlaybackLapMeta>,
        fastestLap: Int,
        deltaAvailable: Boolean,
        playbackTrackLengthM: Double,
    ) {
        resetAllLapComparison()
        playbackActive = true
        playbackLaps = laps
        playbackFastestLap = fastestLap
        playbackDeltaAvailable = deltaAvailable
        trackLengthM = playbackTrackLengthM.takeIf { it.isFinite() && it > 0.0 } ?: 0.0
        val fastestTime = playbackLaps.firstOrNull {
            it.lapNumber == fastestLap
        }?.lapTimeMs ?: 0
        publishLapComparison(DashboardLapComparisonState(fastestLapMs = fastestTime))
    }

    private fun updatePlaybackReference(sample: LiveLapSample) {
        if (!playbackActive || sample.lapNumber <= 0 || !sample.lapDistanceM.isFinite()) return
        if (sample.sessionTime.isFinite()) playbackCursorSessionTime = sample.sessionTime
        playbackSelectedLap = sample.lapNumber
        playbackCursorSample = sample
        publishPlaybackAtCursor(
            playbackLaps.firstOrNull { it.lapNumber == sample.lapNumber },
        )
    }

    private fun updatePlaybackCursor(sessionTime: Double) {
        if (!playbackActive || !sessionTime.isFinite()) return
        playbackCursorSessionTime = sessionTime
        val current = playbackLaps.asSequence()
            .filter {
                it.startSessionTime.isFinite() && it.endSessionTime.isFinite() &&
                    sessionTime >= it.startSessionTime && sessionTime <= it.endSessionTime
            }
            .maxByOrNull { it.startSessionTime }
        if (current == null) {
            playbackSelectedLap = 0
            playbackCursorSample = null
        } else if (playbackSelectedLap != current.lapNumber) {
            playbackSelectedLap = current.lapNumber
            playbackCursorSample = null
        }
        publishPlaybackAtCursor(current)
    }

    private fun publishPlaybackAtCursor(current: PlaybackLapMeta?) {
        val fastest = playbackLaps.firstOrNull { it.lapNumber == playbackFastestLap }
        if (fastest == null || current == null) {
            publishLapComparison(
                DashboardLapComparisonState(fastestLapMs = fastest?.lapTimeMs ?: 0),
            )
            return
        }

        val key = current.lapNumber to fastest.lapNumber
        val cached = playbackDeltaCache[key]
        if (cached != null) {
            val cursorDistance = interpolateDistanceAtSessionTime(
                cached.progress,
                playbackCursorSessionTime,
            ) ?: playbackCursorSample
                ?.takeIf { it.lapNumber == current.lapNumber }
                ?.lapDistanceM
            if (cursorDistance != null && cursorDistance.isFinite()) {
                applyPlaybackDelta(fastest.lapTimeMs, cached, cursorDistance)
            } else {
                publishLapComparison(DashboardLapComparisonState(fastestLapMs = fastest.lapTimeMs))
            }
            return
        }

        publishLapComparison(DashboardLapComparisonState(fastestLapMs = fastest.lapTimeMs))
        if (!playbackDeltaAvailable || key in playbackDeltaUnavailable) return
        val requester = lapDeltaRequester ?: return
        if (activePlaybackRequest?.let {
                it.currentLap == current.lapNumber && it.comparisonLap == fastest.lapNumber
            } == true) return
        val request = PlaybackLapDeltaRequest(
            requestId = ++playbackRequestId,
            currentLap = current.lapNumber,
            comparisonLap = fastest.lapNumber,
        )
        activePlaybackRequest = request
        if (!requester(request)) activePlaybackRequest = null
    }

    private fun acceptPlaybackDelta(
        requestId: Long,
        currentLap: Int,
        comparisonLap: Int,
        curve: LapDeltaCurve?,
    ) {
        val active = activePlaybackRequest
        if (active == null || active.requestId != requestId) return
        activePlaybackRequest = null
        if (!playbackActive) return
        if (curve == null) {
            playbackDeltaUnavailable += active.currentLap to active.comparisonLap
            return
        }
        if (currentLap <= 0 || comparisonLap <= 0) return
        if (active.currentLap != currentLap || active.comparisonLap != comparisonLap) return
        if (curve.currentLap != currentLap || curve.comparisonLap != comparisonLap) return
        val key = currentLap to comparisonLap
        playbackDeltaCache.remove(key)
        playbackDeltaCache[key] = curve
        while (playbackDeltaCache.size > MAX_PLAYBACK_DELTA_CURVES) {
            playbackDeltaCache.remove(playbackDeltaCache.keys.first())
        }
        if (playbackSelectedLap == currentLap && playbackFastestLap == comparisonLap) {
            publishPlaybackAtCursor(
                playbackLaps.firstOrNull { it.lapNumber == currentLap },
            )
        }
    }

    private fun applyPlaybackDelta(
        fastestLapMs: Int,
        curve: LapDeltaCurve,
        cursorDistanceM: Double,
    ) {
        val summary = summarizeDelta(curve, cursorDistanceM)
        publishLapComparison(DashboardLapComparisonState(
            fastestLapMs = fastestLapMs,
            lapDeltaSeconds = summary.lapSeconds,
            sector1DeltaSeconds = summary.sector1Seconds,
            sector2DeltaSeconds = summary.sector2Seconds,
            sector3DeltaSeconds = summary.sector3Seconds,
        ))
    }

    private fun readLapDeltaCurve(data: JSONObject): LapDeltaCurve? {
        val sectors = mutableListOf(mutableListOf<DeltaSample>())
        val samples = data.optJSONArray("samples")
        if (samples != null) repeat(samples.length()) { index ->
            val sample = samples.optJSONObject(index) ?: return@repeat
            if (!sample.optBoolean("valid", true)) {
                if (sectors.last().isNotEmpty()) sectors.add(mutableListOf())
                return@repeat
            }
            val delta = sample.optDouble("delta_seconds", Double.NaN)
            val distance = sample.optDouble("lap_distance_m", Double.NaN)
            if (delta.isFinite() && distance.isFinite()) {
                sectors.last().add(DeltaSample(distance, delta))
            }
        }
        val values = sectors.filter { it.isNotEmpty() }
        if (values.isEmpty()) return null
        val startSessionTime = data.optDouble("currentStartSessionTime", Double.NaN)
        val endSessionTime = data.optDouble("currentEndSessionTime", Double.NaN)
        val rawProgress = buildList {
            val samples = data.optJSONArray("currentProgress")
            if (samples != null) repeat(samples.length()) { index ->
                val sample = samples.optJSONObject(index) ?: return@repeat
                add(
                    PlaybackProgressSample(
                        sessionTime = sample.optDouble("session_time", Double.NaN),
                        elapsedMs = sample.optInt("current_lap_ms", -1),
                        distanceM = sample.optDouble("lap_distance_m", Double.NaN),
                    ),
                )
            }
        }
        return LapDeltaCurve(
            currentLap = data.optInt("currentLapNum"),
            comparisonLap = data.optInt("comparisonLapNum"),
            sectors = values,
            progress = buildPlaybackProgress(
                rawProgress,
                startSessionTime,
                endSessionTime,
            ),
        )
    }

    private fun buildPlaybackProgress(
        raw: List<PlaybackProgressSample>,
        startSessionTime: Double,
        endSessionTime: Double,
    ): List<PlaybackProgressSample> {
        if (raw.isEmpty() || !startSessionTime.isFinite() || !endSessionTime.isFinite()) {
            return emptyList()
        }
        var originDistance = Double.POSITIVE_INFINITY
        for (point in raw) {
            if (point.sessionTime > endSessionTime) break
            if (point.sessionTime < startSessionTime || point.elapsedMs != 0 ||
                !point.distanceM.isFinite() || point.distanceM < 0.0) continue
            originDistance = minOf(originDistance, point.distanceM)
        }
        if (!originDistance.isFinite()) originDistance = 0.0
        val points = mutableListOf(
            PlaybackProgressSample(startSessionTime, 0, originDistance),
        )
        var lastTime = startSessionTime
        var lastDistance = originDistance
        for (point in raw) {
            if (point.sessionTime > endSessionTime) break
            if (!point.sessionTime.isFinite() || !point.distanceM.isFinite() ||
                point.sessionTime < lastTime || point.distanceM < lastDistance ||
                point.elapsedMs < 0) continue
            if (point.sessionTime == startSessionTime) continue
            if (points.size == 1) {
                if (point.distanceM == originDistance) continue
                points.add(point)
            } else if (point.distanceM == lastDistance) {
                continue
            } else if (point.sessionTime == lastTime) {
                points[points.lastIndex] = point
            } else {
                points.add(point)
            }
            lastTime = point.sessionTime
            lastDistance = point.distanceM
        }
        return points.takeIf { it.size >= 2 } ?: emptyList()
    }

    private fun interpolateDistanceAtSessionTime(
        points: List<PlaybackProgressSample>,
        sessionTime: Double,
    ): Double? {
        if (points.size < 2 || !sessionTime.isFinite() ||
            sessionTime < points.first().sessionTime ||
            sessionTime > points.last().sessionTime) return null
        var low = 1
        var high = points.size
        while (low < high) {
            val middle = (low + high) ushr 1
            if (points[middle].sessionTime < sessionTime) low = middle + 1 else high = middle
        }
        if (low >= points.size) return points.last().distanceM
        val before = points[low - 1]
        val after = points[low]
        val span = after.sessionTime - before.sessionTime
        val ratio = if (span > 0.0) (sessionTime - before.sessionTime) / span else 1.0
        return before.distanceM + (after.distanceM - before.distanceM) * ratio
    }

    private fun summarizeDelta(curve: LapDeltaCurve, cursorDistanceM: Double): LapDeltaSummary {
        val sectorValues = (0..2).map { index ->
            val samples = curve.sectors.getOrNull(index)
            if (samples.isNullOrEmpty() || cursorDistanceM < samples.first().distanceM) {
                null
            } else {
                interpolateDelta(samples, cursorDistanceM.coerceAtMost(samples.last().distanceM))
            }
        }
        val visible = sectorValues.filterNotNull()
        return LapDeltaSummary(
            lapSeconds = visible.takeIf { it.isNotEmpty() }?.sum(),
            sector1Seconds = sectorValues[0],
            sector2Seconds = sectorValues[1],
            sector3Seconds = sectorValues[2],
        )
    }

    private fun interpolateDelta(samples: List<DeltaSample>, distanceM: Double): Double? {
        if (distanceM < samples.first().distanceM || distanceM > samples.last().distanceM) return null
        var low = 1
        var high = samples.size
        while (low < high) {
            val middle = (low + high) ushr 1
            if (samples[middle].distanceM < distanceM) low = middle + 1 else high = middle
        }
        if (low >= samples.size) return samples.last().deltaSeconds
        val before = samples[low - 1]
        val after = samples[low]
        val span = after.distanceM - before.distanceM
        if (span <= 0.0) return after.deltaSeconds
        val ratio = (distanceM - before.distanceM) / span
        return before.deltaSeconds + (after.deltaSeconds - before.deltaSeconds) * ratio
    }

    private fun buildCompletedProgress(
        raw: List<LapProgressSample>,
        lapTimeMs: Int,
    ): List<LapProgressSample> {
        if (raw.isEmpty()) return emptyList()
        val originDistance = raw.asSequence()
            .filter { it.elapsedMs == 0 && it.distanceM.isFinite() && it.distanceM >= 0.0 }
            .minOfOrNull { it.distanceM } ?: 0.0
        val points = mutableListOf(LapProgressSample(0, originDistance))
        var lastElapsed = 0
        var lastDistance = originDistance
        for (point in raw) {
            if (point.elapsedMs < lastElapsed || !point.distanceM.isFinite() ||
                point.distanceM < lastDistance || point.elapsedMs < 0) continue
            if (point.elapsedMs == 0 || point.distanceM == lastDistance) continue
            points.add(point)
            lastElapsed = point.elapsedMs
            lastDistance = point.distanceM
        }
        if (trackLengthM > lastDistance && lapTimeMs > lastElapsed) {
            points.add(LapProgressSample(lapTimeMs, trackLengthM))
        }
        return points.takeIf { it.size >= 2 } ?: emptyList()
    }

    private fun interpolateElapsed(
        points: List<LapProgressSample>,
        distanceM: Double,
    ): Double? {
        if (points.size < 2 || !distanceM.isFinite() ||
            distanceM < points.first().distanceM || distanceM > points.last().distanceM) return null
        var low = 1
        var high = points.size
        while (low < high) {
            val middle = (low + high) ushr 1
            if (points[middle].distanceM < distanceM) low = middle + 1 else high = middle
        }
        if (low >= points.size) return points.last().elapsedMs / 1000.0
        val before = points[low - 1]
        val after = points[low]
        val span = after.distanceM - before.distanceM
        val ratio = if (span > 0.0) (distanceM - before.distanceM) / span else 1.0
        return (before.elapsedMs + (after.elapsedMs - before.elapsedMs) * ratio) / 1000.0
    }

    private fun deltaSeconds(currentMs: Int?, comparisonMs: Int?): Double? =
        if (currentMs != null && comparisonMs != null && currentMs > 0 && comparisonMs > 0) {
            (currentMs - comparisonMs) / 1000.0
        } else {
            null
        }

    private fun resolveAeroMode(year: Int?, formula: Int?, fallback: String?): String = when {
        year == 2026 && (formula == null || formula == 13) -> "slm"
        year != null -> "drs"
        fallback == "slm" -> "slm"
        else -> "drs"
    }
}
