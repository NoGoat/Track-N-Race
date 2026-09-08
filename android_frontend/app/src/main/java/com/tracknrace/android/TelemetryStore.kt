package com.tracknrace.android

import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.roundToInt

/**
 * Thread boundary between libtnrp/OkHttp and Compose.
 *
 * Hot rows never touch Compose state. Worker threads decode a whole native
 * batch to its latest telemetry sample and publish one immutable atomic value.
 * The dashboard samples that value on the display frame clock. Cold rows are
 * parsed off the UI thread and applied to snapshot state on the main thread.
 */
internal class TelemetryStore {
    private val main = Handler(Looper.getMainLooper())
    private val hot = AtomicReference(HotTelemetry())
    private val mapPositions = AtomicReference(MapPositions())
    private val hotRows = AtomicLong()
    private val malformedReported = AtomicBoolean()
    private val messageIds = AtomicLong()

    var cold by mutableStateOf(DashboardColdState())
        private set
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

    fun latestHot(): HotTelemetry = hot.get()
    fun latestMapPositions(): MapPositions = mapPositions.get()
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

        when (row.optString("type")) {
            "lap" -> post {
                cold = cold.copy(
                    position = row.optInt("position"),
                    lapNumber = row.optInt("lap_num"),
                    currentLapMs = row.optInt("current_lap_ms"),
                    lastLapMs = row.optInt("last_lap_ms"),
                    lapInvalid = row.optBoolean("lap_invalid"),
                )
            }

            "status" -> post {
                cold = cold.copy(
                    statusAvailable = true,
                    ersPercent = row.optDouble("ers_pct").roundToInt(),
                    ersMode = row.optInt("ers_mode"),
                    fuelKg = row.optDouble("fuel_kg"),
                    fuelLaps = row.optDouble("fuel_laps"),
                    brakeBias = row.optInt("front_brake_bias"),
                    tyreCompound = row.optInt("visual_compound"),
                    tyreAgeLaps = row.optInt("tyre_age_laps"),
                )
            }

            "damage" -> post {
                cold = cold.copy(
                    tyreWearFl = row.optDouble("tyre_wear_fl").toFloat(),
                    tyreWearFr = row.optDouble("tyre_wear_fr").toFloat(),
                    tyreWearRl = row.optDouble("tyre_wear_rl").toFloat(),
                    tyreWearRr = row.optDouble("tyre_wear_rr").toFloat(),
                    tyreWearAvailable = true,
                )
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
                post {
                    cold = cold.copy(
                        totalLaps = totalLaps,
                        sessionType = sessionType,
                        trackId = trackId,
                    )
                }
            }

            "tyre_sets" -> {
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
                val cars = row.optJSONArray("cars")?.let { array ->
                    List(array.length()) { arrayIndex ->
                        val car = array.optJSONObject(arrayIndex) ?: JSONObject()
                        TimingCarEntry(
                            index = car.optInt("idx", arrayIndex),
                            position = car.optInt("position"),
                            lapNumber = car.optInt("lap_num"),
                            currentLapMs = car.optInt("current_lap_ms"),
                            lastLapMs = car.optInt("last_lap_ms"),
                            gapMs = car.optInt("gap_ms"),
                            pitStatus = car.optInt("pit_status"),
                            lapInvalid = car.optBoolean("lap_invalid"),
                            penaltiesSeconds = car.optInt("penalties_s"),
                            driveThroughPenalties = car.optInt("num_dt_pens"),
                            stopGoPenalties = car.optInt("num_sg_pens"),
                            resultStatus = car.optInt("result_status"),
                        )
                    }
                }.orEmpty()
                val playerIndex = row.optInt("player_idx", -1)
                post { timing = timing.copy(playerIndex = playerIndex, cars = cars) }
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
                val statuses = row.optJSONArray("cars")?.let { array ->
                    buildMap {
                        repeat(array.length()) { arrayIndex ->
                            val status = array.optJSONObject(arrayIndex) ?: return@repeat
                            put(
                                status.optInt("idx", arrayIndex),
                                TimingTyreStatus(
                                    actualCompound = status.optInt("tyre_compound"),
                                    visualCompound = status.optInt("visual_compound"),
                                    ageLaps = status.optInt("tyre_age_laps"),
                                ),
                            )
                        }
                    }
                }.orEmpty()
                post { timing = timing.copy(tyreStatuses = statuses) }
            }

            "protocol_context" -> {
                mapPositions.lazySet(MapPositions())
                val year = row.optionalInt("protocol_year")
                val formula = row.optionalInt("formula")
                post {
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
                    // A seek/reset may be delivered after its reconstructed
                    // Participants row. Keep that roster; a real game-session
                    // change has its own participants_reset signal.
                    timing = TimingTowerState(
                        drivers = timing.drivers,
                        hasParticipants = timing.hasParticipants,
                    )
                }
            }

            "participants_reset" -> post {
                timing = timing.copy(drivers = emptyMap(), hasParticipants = false)
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

    fun updateSettings(value: AndroidSettings) = post { settings = value }

    fun updateSource(state: String, detail: String? = null) = post {
        sourceStatus = SourceStatus(state, detail)
    }

    fun updatePairingBusy(value: Boolean) = post { pairingBusy = value }

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

    private fun JSONObject.optionalInt(name: String): Int? =
        if (!has(name) || isNull(name)) null else optInt(name)

    private fun resolveAeroMode(year: Int?, formula: Int?, fallback: String?): String = when {
        year == 2026 && (formula == null || formula == 13) -> "slm"
        year != null -> "drs"
        fallback == "slm" -> "slm"
        else -> "drs"
    }
}
