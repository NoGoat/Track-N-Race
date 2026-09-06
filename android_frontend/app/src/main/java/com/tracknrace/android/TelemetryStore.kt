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
    private val hotRows = AtomicLong()
    private val malformedReported = AtomicBoolean()
    private val messageIds = AtomicLong()

    var cold by mutableStateOf(DashboardColdState())
        private set
    var settings by mutableStateOf(AndroidSettings())
        private set
    var sourceStatus by mutableStateOf(SourceStatus())
        private set
    var pairingBusy by mutableStateOf(false)
        private set
    var message by mutableStateOf<UiMessage?>(null)
        private set
    val discoveredDesktops = mutableStateListOf<DiscoveredDesktop>()

    fun latestHot(): HotTelemetry = hot.get()
    fun totalHotRows(): Long = hotRows.get()

    fun acceptBinary(bytes: ByteArray) {
        val result = BinaryTelemetryDecoder.decodeLatest(bytes)
        result.latest?.let(hot::lazySet)
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
                    ersPercent = row.optDouble("ers_pct").roundToInt(),
                    fuelLaps = row.optDouble("fuel_laps"),
                    brakeBias = row.optInt("front_brake_bias"),
                    tyreCompound = row.optInt("visual_compound"),
                    tyreAgeLaps = row.optInt("tyre_age_laps"),
                )
            }

            "session" -> {
                val sessionType = row.optionalInt("session_type")
                val totalLaps = row.optInt("total_laps")
                post { cold = cold.copy(totalLaps = totalLaps, sessionType = sessionType) }
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

            "protocol_context" -> {
                val year = row.optionalInt("protocol_year")
                val formula = row.optionalInt("formula")
                post {
                    cold = cold.copy(
                        protocolYear = year,
                        formula = formula,
                        aeroMode = resolveAeroMode(year, formula, null),
                        labels = emptyMap(),
                        sessionType = null,
                        tyreSets = emptyList(),
                    )
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

    fun updateSettings(value: AndroidSettings) = post { settings = value }

    fun updateSource(state: String, detail: String? = null) = post {
        sourceStatus = SourceStatus(state, detail)
    }

    fun updatePairingBusy(value: Boolean) = post { pairingBusy = value }

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
