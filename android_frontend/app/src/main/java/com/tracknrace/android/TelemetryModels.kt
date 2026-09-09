package com.tracknrace.android

internal data class HotTelemetry(
    val sessionTime: Float = 0f,
    val speedKph: Int = 0,
    val rpm: Int = 0,
    val gear: Int = 0,
    val throttle: Float = 0f,
    val brake: Float = 0f,
    val steering: Double = 0.0,
    val drs: Int = 0,
    val revLightsPercent: Int? = null,
    val revLightsBitValue: Int? = null,
    val slm: Int = 0,
    val engineTemp: Int = 0,
    val tyreSurfaceFl: Int = 0,
    val tyreSurfaceFr: Int = 0,
    val tyreSurfaceRl: Int = 0,
    val tyreSurfaceRr: Int = 0,
    val tyreInnerFl: Int = 0,
    val tyreInnerFr: Int = 0,
    val tyreInnerRl: Int = 0,
    val tyreInnerRr: Int = 0,
)

/**
 * Immutable-by-convention position snapshot indexed by car id.
 *
 * Primitive arrays keep the per-packet hot path to two allocations instead of
 * allocating a list plus one object for every car. A published snapshot is
 * never mutated after it is placed in [TelemetryStore].
 */
internal class MapPositions(
    val playerIndex: Int = -1,
    private val xByCar: DoubleArray = EMPTY_COORDINATES,
    private val zByCar: DoubleArray = EMPTY_COORDINATES,
) {
    init {
        require(xByCar.size == zByCar.size) { "Position coordinate arrays must have equal lengths" }
    }

    val carCount: Int
        get() = xByCar.size

    fun xAt(index: Int): Double = xByCar[index]

    fun zAt(index: Int): Double = zByCar[index]

    fun hasPosition(index: Int): Boolean = index in xByCar.indices

    private companion object {
        val EMPTY_COORDINATES = DoubleArray(0)
    }
}

internal data class TyreSetEntry(
    val index: Int,
    val actualCompound: Int,
    val visualCompound: Int,
    val wear: Float,
    val available: Boolean,
    val recommendedSession: Int,
    val lifeSpan: Int,
    val usableLife: Int,
    val lapDeltaMs: Int,
    val fitted: Boolean,
)

internal data class DashboardColdState(
    val statusAvailable: Boolean = false,
    val position: Int = 0,
    val lapNumber: Int = 0,
    val totalLaps: Int = 0,
    val currentLapMs: Int = 0,
    val lastLapMs: Int = 0,
    val lapInvalid: Boolean = false,
    val ersPercent: Int = 0,
    val ersMode: Int = 0,
    val fuelKg: Double = 0.0,
    val fuelLaps: Double = 0.0,
    val brakeBias: Int = 0,
    val tyreCompound: Int = 0,
    val tyreAgeLaps: Int = 0,
    val tyreWearFl: Float = 0f,
    val tyreWearFr: Float = 0f,
    val tyreWearRl: Float = 0f,
    val tyreWearRr: Float = 0f,
    val tyreWearAvailable: Boolean = false,
    val sessionType: Int? = null,
    val trackId: Int = -1,
    val tyreSets: List<TyreSetEntry> = emptyList(),
    val aeroMode: String = "drs",
    val protocolYear: Int? = null,
    val formula: Int? = null,
    val labels: Map<String, String> = emptyMap(),
)

internal data class DashboardLapComparisonState(
    val fastestLapMs: Int = 0,
    val lapDeltaSeconds: Double? = null,
    val sector1DeltaSeconds: Double? = null,
    val sector2DeltaSeconds: Double? = null,
    val sector3DeltaSeconds: Double? = null,
)

internal data class PlaybackLapDeltaRequest(
    val requestId: Long,
    val currentLap: Int,
    val comparisonLap: Int,
)

internal data class TimingCarEntry(
    val index: Int,
    val position: Int,
    val lapNumber: Int,
    val currentLapMs: Int,
    val lastLapMs: Int,
    val gapMs: Int,
    val pitStatus: Int,
    val lapInvalid: Boolean,
    val penaltiesSeconds: Int,
    val driveThroughPenalties: Int,
    val stopGoPenalties: Int,
    val resultStatus: Int,
)

internal data class TimingDriver(
    val index: Int,
    val name: String,
    val raceNumber: Int,
    val teamColor: String,
)

internal data class TimingTyreStatus(
    val actualCompound: Int,
    val visualCompound: Int,
    val ageLaps: Int,
)

internal data class TimingTowerState(
    val playerIndex: Int = -1,
    val cars: List<TimingCarEntry> = emptyList(),
    val drivers: Map<Int, TimingDriver> = emptyMap(),
    val tyreStatuses: Map<Int, TimingTyreStatus> = emptyMap(),
    val hasParticipants: Boolean = false,
)

internal fun TimingTowerState.needsParticipantsRefresh(): Boolean =
    !hasParticipants || drivers.isEmpty()

internal data class AndroidSettings(
    val source: String = PairedTelemetryClient.SOURCE_DIRECT,
    val recordingEnabled: Boolean = false,
    val timingOneLine: Boolean = false,
    val hasSavedDesktop: Boolean = false,
    val desktopName: String = "",
    val recordingDirectory: String = "",
    val usingCustomDirectory: Boolean = false,
)

internal data class SourceStatus(
    val state: String = "starting",
    val detail: String? = null,
)

internal data class DiscoveredDesktop(
    val serverId: String,
    val name: String,
    val host: String,
    val port: Int,
    val pairing: Boolean,
)

internal data class UiMessage(val id: Long, val text: String)
