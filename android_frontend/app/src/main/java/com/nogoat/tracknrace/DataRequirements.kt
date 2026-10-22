package com.nogoat.tracknrace

/** libtnrp's logical row families; a family's bit is `1 shl id`. */
internal object DataRow {
    const val TELEMETRY = 1 shl 1
    const val STATUS = 1 shl 2
    const val DAMAGE = 1 shl 3
    const val LAP = 1 shl 4
    const val SESSION = 1 shl 5
    const val TIMING = 1 shl 7
    const val PARTICIPANTS = 1 shl 8
    const val ALL_STATUS = 1 shl 9
    const val TYRE_SETS = 1 shl 10
    const val MOTION = 1 shl 11
    const val MOTION_EX = 1 shl 12
    const val POSITIONS = 1 shl 13
    const val STRATEGY = 1 shl 15
}

/**
 * libtnrp's `V6DataType` ids: the individually loadable fields of a V6
 * recording. A row family says which rows may flow; these say which fields of
 * them the desktop has to read at all. Only the fields the Android UI shows are
 * listed.
 */
internal object V6Data {
    const val SPEED = 1
    const val RPM = 2
    const val GEAR = 3
    const val THROTTLE = 4
    const val BRAKE = 5
    const val STEERING = 6
    const val AERO = 7
    const val TYRE_SURFACE_TEMP = 8
    const val TYRE_INNER_TEMP = 9
    const val BRAKE_TEMP = 10
    const val ENGINE_TEMP = 11
    const val TYRE_WEAR = 12
    const val TYRE_STATE = 13
    const val DAMAGE = 14
    const val FUEL = 15
    const val ERS_STORE = 16
    const val ERS_HARVEST = 17
    const val ERS_DEPLOYMENT = 18
    const val ENGINE_POWER = 19
    const val G_FORCE = 21
    const val RIDE_HEIGHT = 22
    const val BRAKE_BIAS = 20
    const val POSITION = 23
    const val LAP_TIMING = 24
}

/**
 * One piece of UI and exactly the data it needs. A page declares the consumers
 * it shows and the desktop is asked for their union, so a field nothing on the
 * page displays is never loaded or sent. Hand-written for the Android pages;
 * deliberately not shared with the Electron app's consumer table.
 */
internal enum class DataConsumer(
    val streamMask: Int,
    val v6Types: List<Int> = emptyList(),
) {
    /** Roster names: the app bar's selected driver, timing rows. */
    DRIVER_ROSTER(DataRow.PARTICIPANTS),

    /** Session type, lap count and track. */
    SESSION_INFO(DataRow.SESSION),

    /** Speed, gear and the RPM behind the shift lights. */
    DRIVING_INPUTS(
        DataRow.TELEMETRY,
        listOf(V6Data.SPEED, V6Data.RPM, V6Data.GEAR),
    ),

    /** Throttle, brake and the open DRS / SLM flag. */
    PEDALS_AND_AERO(
        DataRow.TELEMETRY,
        listOf(V6Data.THROTTLE, V6Data.BRAKE, V6Data.AERO),
    ),

    /** The four-corner surface and inner (carcass) temperatures. */
    TYRE_TEMPERATURES(
        DataRow.TELEMETRY,
        listOf(V6Data.TYRE_SURFACE_TEMP, V6Data.TYRE_INNER_TEMP),
    ),

    /** The four-corner tyre wear. */
    TYRE_WEAR(DataRow.DAMAGE, listOf(V6Data.TYRE_WEAR)),

    /** Body, tyre and brake damage, faults and power-unit wear. */
    CAR_DAMAGE(DataRow.DAMAGE, listOf(V6Data.DAMAGE)),

    /** ERS level and deploy mode, fuel margin and brake bias. */
    POWER_UNIT(
        DataRow.STATUS,
        listOf(V6Data.ERS_STORE, V6Data.ERS_DEPLOYMENT, V6Data.FUEL, V6Data.BRAKE_BIAS),
    ),

    /**
     * ICE and MGU-K power, ERS harvest and store, and fuel, for the Power
     * graphs; and the fitted tyre, which marks where Stint Laps begins.
     */
    POWER_HISTORY(
        DataRow.STATUS,
        listOf(V6Data.ENGINE_POWER, V6Data.ERS_HARVEST, V6Data.ERS_STORE, V6Data.FUEL, V6Data.TYRE_STATE),
    ),

    /** The four brake temperatures, for the Tyres graphs. */
    BRAKE_TEMPERATURES(DataRow.TELEMETRY, listOf(V6Data.BRAKE_TEMP)),

    /** The ERS store alone, for the Speed graph. */
    ERS_LEVEL(DataRow.STATUS, listOf(V6Data.ERS_STORE)),

    /** Throttle, brake, steering and gear for the Inputs graphs. */
    INPUT_HISTORY(
        DataRow.TELEMETRY,
        listOf(V6Data.THROTTLE, V6Data.BRAKE, V6Data.STEERING, V6Data.GEAR),
    ),

    /** Lateral and longitudinal G for the Misc graphs. */
    G_FORCE_HISTORY(DataRow.MOTION, listOf(V6Data.G_FORCE)),

    /** Front and rear ride height for the Misc graphs. */
    RIDE_HEIGHT_HISTORY(DataRow.MOTION_EX, listOf(V6Data.RIDE_HEIGHT)),

    /** ERS deployed and harvested this lap, and the fitted tyre, for the Trends graphs. */
    LAP_ENERGY(
        DataRow.STATUS,
        listOf(V6Data.ERS_DEPLOYMENT, V6Data.ERS_HARVEST, V6Data.TYRE_STATE),
    ),

    /** Every car's world position, for the dashboard's live map. */
    CAR_POSITIONS(DataRow.POSITIONS, listOf(V6Data.POSITION)),

    /** Fitted compound and tyre age on the dashboard. */
    FITTED_TYRE(DataRow.STATUS, listOf(V6Data.TYRE_STATE)),

    /** Position, lap times, sector and distance for the dashboard and lap delta. */
    LAP_PROGRESS(DataRow.LAP, listOf(V6Data.LAP_TIMING)),

    /** Every car's position, gap and lap on the timing tower. */
    TIMING_TOWER(DataRow.TIMING, listOf(V6Data.LAP_TIMING)),

    /** Each tower row's compound and tyre age. */
    TIMING_TYRES(DataRow.ALL_STATUS, listOf(V6Data.TYRE_STATE)),

    /** The available and fitted tyre sets. */
    TYRE_SETS(DataRow.TYRE_SETS, listOf(V6Data.TYRE_STATE)),

    /**
     * The derived Strategy snapshot. Its fields are the ones the engine's
     * strategy reads from a V6 recording, as the desktop Strategy page asks.
     */
    STRATEGY(
        DataRow.STRATEGY,
        listOf(V6Data.FUEL, V6Data.TYRE_STATE, V6Data.TYRE_WEAR, V6Data.LAP_TIMING),
    ),
}
