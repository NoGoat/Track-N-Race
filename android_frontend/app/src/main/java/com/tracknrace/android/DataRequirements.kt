package com.tracknrace.android

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
    const val TYRE_SURFACE_TEMP = 8
    const val TYRE_INNER_TEMP = 9
    const val TYRE_WEAR = 12
    const val TYRE_STATE = 13
    const val FUEL = 15
    const val ERS_STORE = 16
    const val BRAKE_BIAS = 20
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

    /** Speed, gear, RPM and the throttle/brake bars. */
    DRIVING_INPUTS(
        DataRow.TELEMETRY,
        listOf(V6Data.SPEED, V6Data.RPM, V6Data.GEAR, V6Data.THROTTLE, V6Data.BRAKE),
    ),

    /** The four-corner surface and inner tyre temperatures. */
    TYRE_TEMPERATURES(
        DataRow.TELEMETRY,
        listOf(V6Data.TYRE_SURFACE_TEMP, V6Data.TYRE_INNER_TEMP),
    ),

    /** Per-corner tyre wear next to the temperatures. */
    TYRE_WEAR(DataRow.DAMAGE, listOf(V6Data.TYRE_WEAR)),

    /** ERS, fuel and brake bias. */
    POWER_UNIT(
        DataRow.STATUS,
        listOf(V6Data.ERS_STORE, V6Data.FUEL, V6Data.BRAKE_BIAS),
    ),

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
}
