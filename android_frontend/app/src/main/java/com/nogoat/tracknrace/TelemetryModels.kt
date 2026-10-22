package com.nogoat.tracknrace

import com.nogoat.tracknrace.ui.theme.ThemeMode
import org.json.JSONObject

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
 * Applies one `telemetry` JSON row to the hot sample. A V6 recording streams a
 * single field group per row (speed, or tyre surface temperatures, ...), so
 * every field that is absent keeps its current value.
 */
internal fun HotTelemetry.withPatch(row: JSONObject): HotTelemetry = copy(
    sessionTime = row.optionalDouble("session_time")?.toFloat() ?: sessionTime,
    speedKph = row.optionalInt("speed_kph") ?: speedKph,
    rpm = row.optionalInt("rpm") ?: rpm,
    gear = row.optionalInt("gear") ?: gear,
    throttle = row.optionalDouble("throttle")?.toFloat() ?: throttle,
    brake = row.optionalDouble("brake")?.toFloat() ?: brake,
    steering = row.optionalDouble("steering") ?: steering,
    drs = row.optionalInt("drs") ?: drs,
    revLightsPercent = row.optionalInt("rev_lights_pct") ?: revLightsPercent,
    revLightsBitValue = row.optionalInt("rev_lights_bit_value") ?: revLightsBitValue,
    slm = row.optionalInt("slm") ?: slm,
    engineTemp = row.optionalInt("engine_temp") ?: engineTemp,
    tyreSurfaceFl = row.optionalInt("tyre_temp_surface_fl") ?: tyreSurfaceFl,
    tyreSurfaceFr = row.optionalInt("tyre_temp_surface_fr") ?: tyreSurfaceFr,
    tyreSurfaceRl = row.optionalInt("tyre_temp_surface_rl") ?: tyreSurfaceRl,
    tyreSurfaceRr = row.optionalInt("tyre_temp_surface_rr") ?: tyreSurfaceRr,
    tyreInnerFl = row.optionalInt("tyre_temp_inner_fl") ?: tyreInnerFl,
    tyreInnerFr = row.optionalInt("tyre_temp_inner_fr") ?: tyreInnerFr,
    tyreInnerRl = row.optionalInt("tyre_temp_inner_rl") ?: tyreInnerRl,
    tyreInnerRr = row.optionalInt("tyre_temp_inner_rr") ?: tyreInnerRr,
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

    /** True when car [index] has a known position; unknown cars hold NaN. */
    fun hasPosition(index: Int): Boolean =
        index in xByCar.indices && xByCar[index].isFinite() && zByCar[index].isFinite()

    private companion object {
        val EMPTY_COORDINATES = DoubleArray(0)
    }
}

/**
 * Whether a `tyre_sets` row describes the car this app is displaying.
 *
 * libtnrp no longer drops other cars' tyre-set packets, so every row carries the
 * `car_idx` it was read for. Rows without one come from recordings and desktops
 * that predate that and were already player-only. A tagged row is only trusted
 * once the player index is known; before that it is dropped rather than shown
 * for the wrong car. A row projected from a V6 recording is already scoped to the
 * driver chosen on the desktop, so it needs no such check.
 */
internal fun ownsTyreSets(carIdx: Int?, playerIdx: Int, v6Projected: Boolean = false): Boolean =
    v6Projected || carIdx == null || carIdx < 0 || (playerIdx >= 0 && carIdx == playerIdx)

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
    // Wear per lap on this set in the current session; null until it has
    // done a lap this session.
    val avgWearPerLap: Float? = null,
)

internal data class DashboardColdState(
    val statusAvailable: Boolean = false,
    val position: Int = 0,
    val lapNumber: Int = 0,
    val totalLaps: Int = 0,
    val currentLapMs: Int = 0,
    val lastLapMs: Int = 0,
    val lapInvalid: Boolean = false,
    /** The sector the car is in, 0 to 2, and how far through it, 0 to 1 (NaN while unknown). */
    val sector: Int = 0,
    val sectorFraction: Float = Float.NaN,
    val ersPercent: Int = 0,
    val ersMode: Int = 0,
    val fuelKg: Double = 0.0,
    val fuelLaps: Double = 0.0,
    val brakeBias: Int = 0,
    val tyreCompound: Int = 0,
    /** The actual compound (C1–C6, inter, wet) of [tyreCompound]'s visual one. */
    val tyreActualCompound: Int = 0,
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
    /** 2026 SLM track status: 0 full grip (dry zones), 1 partial (wet zones), -1 unknown. */
    val slmTrackStatus: Int = -1,
    val protocolYear: Int? = null,
    /** The format the desktop presents (2026 after the Formula gate), for its limits. */
    val presentationYear: Int? = null,
    /** The format has an MGU-H, so its harvest is plotted. */
    val hasMguh: Boolean = false,
    val formula: Int? = null,
    val labels: Map<String, String> = emptyMap(),
)

/**
 * The selected car's damage, as the desktop Damage page shows it: percentages,
 * and 0/1 fault flags. Null means not received yet.
 */
internal data class CarDamageState(
    val available: Boolean = false,
    val tyreFl: Int? = null,
    val tyreFr: Int? = null,
    val tyreRl: Int? = null,
    val tyreRr: Int? = null,
    val brakeFl: Int? = null,
    val brakeFr: Int? = null,
    val brakeRl: Int? = null,
    val brakeRr: Int? = null,
    val blistersFl: Int? = null,
    val blistersFr: Int? = null,
    val blistersRl: Int? = null,
    val blistersRr: Int? = null,
    val wingFl: Int? = null,
    val wingFr: Int? = null,
    val wingRear: Int? = null,
    val floor: Int? = null,
    val sidepods: Int? = null,
    val diffuser: Int? = null,
    val gearbox: Int? = null,
    val engine: Int? = null,
    val drsFault: Int? = null,
    val ersFault: Int? = null,
    val iceWear: Int? = null,
    val mguhWear: Int? = null,
    val mgukWear: Int? = null,
    val esWear: Int? = null,
    val ceWear: Int? = null,
    val tcWear: Int? = null,
    val engineBlown: Int? = null,
    val engineSeized: Int? = null,
) {
    /** A live row carries every field; a V6 patch only the ones it changed. */
    fun withPatch(row: JSONObject): CarDamageState {
        fun int(name: String, current: Int?) = row.optionalInt(name) ?: current
        return copy(
            available = true,
            tyreFl = int("tyre_dmg_fl", tyreFl),
            tyreFr = int("tyre_dmg_fr", tyreFr),
            tyreRl = int("tyre_dmg_rl", tyreRl),
            tyreRr = int("tyre_dmg_rr", tyreRr),
            brakeFl = int("brake_dmg_fl", brakeFl),
            brakeFr = int("brake_dmg_fr", brakeFr),
            brakeRl = int("brake_dmg_rl", brakeRl),
            brakeRr = int("brake_dmg_rr", brakeRr),
            blistersFl = int("blisters_fl", blistersFl),
            blistersFr = int("blisters_fr", blistersFr),
            blistersRl = int("blisters_rl", blistersRl),
            blistersRr = int("blisters_rr", blistersRr),
            wingFl = int("wing_fl", wingFl),
            wingFr = int("wing_fr", wingFr),
            wingRear = int("wing_rear", wingRear),
            floor = int("floor_damage", floor),
            sidepods = int("sidepod_damage", sidepods),
            diffuser = int("diffuser_damage", diffuser),
            gearbox = int("gearbox_damage", gearbox),
            engine = int("engine_damage", engine),
            drsFault = int("drs_fault", drsFault),
            ersFault = int("ers_fault", ersFault),
            iceWear = int("engine_ice_wear", iceWear),
            mguhWear = int("engine_mguh_wear", mguhWear),
            mgukWear = int("engine_mguk_wear", mgukWear),
            esWear = int("engine_es_wear", esWear),
            ceWear = int("engine_ce_wear", ceWear),
            tcWear = int("engine_tc_wear", tcWear),
            engineBlown = int("engine_blown", engineBlown),
            engineSeized = int("engine_seized", engineSeized),
        )
    }

    companion object {
        /** Whether a damage row carries any field this state keeps. */
        fun carriesDamage(row: JSONObject): Boolean = DAMAGE_FIELDS.any(row::has)

        private val DAMAGE_FIELDS = setOf(
            "tyre_dmg_fl", "tyre_dmg_fr", "tyre_dmg_rl", "tyre_dmg_rr",
            "brake_dmg_fl", "brake_dmg_fr", "brake_dmg_rl", "brake_dmg_rr",
            "blisters_fl", "blisters_fr", "blisters_rl", "blisters_rr",
            "wing_fl", "wing_fr", "wing_rear", "floor_damage", "sidepod_damage", "diffuser_damage",
            "gearbox_damage", "engine_damage", "drs_fault", "ers_fault",
            "engine_ice_wear", "engine_mguh_wear", "engine_mguk_wear", "engine_es_wear",
            "engine_ce_wear", "engine_tc_wear", "engine_blown", "engine_seized",
        )
    }
}

/** A recording's player: running, where, of how long (seconds from its start), how fast. */
internal data class PlaybackTransport(
    val playing: Boolean = false,
    val currentTime: Double = 0.0,
    val totalTime: Double = 0.0,
    val speed: Float = 1f,
    /** The recording's first session time; currentTime is counted from it. */
    val startTime: Double = 0.0,
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
    val sectorDelta: Boolean = true,
)

/** One lap of the recording the desktop is playing, for the selected driver. */
internal data class PlaybackLapInfo(
    val lapNumber: Int,
    val lapTimeMs: Int,
    val startSessionTime: Double,
    val endSessionTime: Double,
    /** Actual and visual compound the lap ended on, 0 when unknown. */
    val tyreCompound: Int = 0,
    val visualCompound: Int = 0,
)

/**
 * The desktop's playback lap catalogue. [generation] changes only when the
 * recording or its selected driver changes, so cached lap data stays valid
 * across the repeated catalogue row in every subscription snapshot.
 */
internal data class PlaybackCatalog(
    val generation: Long = 0,
    val active: Boolean = false,
    val driverIndex: Int = -1,
    val laps: List<PlaybackLapInfo> = emptyList(),
    val fastestLap: Int = 0,
    val deltaAvailable: Boolean = false,
    val trackLengthM: Double = 0.0,
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
    /**
     * That driver's "Your Telemetry" setting from Participants: 1 public,
     * 0 restricted, null when the packet did not carry one. Null is not
     * "public" -- it means the game has not said yet.
     */
    val yourTelemetry: Int? = null,
)

/**
 * Whether [driverIndex]'s private telemetry is withheld, using the same rule as
 * the desktop app's driver selector: the recording/receiving player always sees
 * their own car, anyone else needs an explicit public setting. Null while the
 * roster has not arrived or carried no setting, which must not be shown as
 * either answer.
 */
internal fun TimingTowerState.restrictionOf(driverIndex: Int, playerIndex: Int): Boolean? {
    if (driverIndex < 0) return null
    if (driverIndex == playerIndex) return false
    val driver = drivers[driverIndex] ?: return null
    return driver.yourTelemetry?.let { it != 1 }
}

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
    val hasSavedDesktop: Boolean = false,
    val desktopName: String = "",
    val recordingDirectory: String = "",
    val usingCustomDirectory: Boolean = false,
    val themeMode: ThemeMode = ThemeMode.SYSTEM,
    /** Landscape hides the bars until tapped; null until chosen, when phones do and tablets don't. */
    val wheelDisplay: Boolean? = null,
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
