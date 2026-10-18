package com.tracknrace.android

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Every record of one hot row family in a batch, for the graph tabs: session
 * times and each field a graph draws, by its row key. Filled on the socket
 * thread and handed to the main thread whole.
 */
internal class RowSamples(private val row: RowFamily, private val keys: List<String>) {
    var count = 0
        private set
    var times = DoubleArray(16)
        private set
    private var columns = Array(keys.size) { FloatArray(16) }

    /** [family]'s fields by slot for [SampleHistory.acceptBatch]; NaN for fields these records do not carry. */
    fun columnsFor(family: GraphFamily): Array<FloatArray> = Array(family.fields.size) { slot ->
        val index = keys.indexOf(family.fields[slot].key)
        if (index >= 0 && family.fields[slot].source == row) columns[index] else FloatArray(count) { Float.NaN }
    }

    fun add(sessionTime: Float, values: FloatArray) {
        if (count == times.size) {
            times = times.copyOf(count * 2)
            columns = Array(columns.size) { columns[it].copyOf(count * 2) }
        }
        times[count] = sessionTime.toDouble()
        for (index in keys.indices) columns[index][count] = values[index]
        count++
    }
}

/** The hot rows of a batch the graph tabs keep: null for a family no tab listens to. */
internal class BinarySamples(telemetry: Boolean, motion: Boolean, motionEx: Boolean) {
    val telemetry = if (telemetry) RowSamples(RowFamily.TELEMETRY, TELEMETRY_KEYS) else null
    val motion = if (motion) RowSamples(RowFamily.MOTION, MOTION_KEYS) else null
    val motionEx = if (motionEx) RowSamples(RowFamily.MOTION_EX, MOTION_EX_KEYS) else null

    fun of(row: RowFamily): RowSamples? = when (row) {
        RowFamily.TELEMETRY -> telemetry
        RowFamily.MOTION -> motion
        RowFamily.MOTION_EX -> motionEx
        else -> null
    }

    companion object {
        /** The row keys of each record's fields kept, in the order [RowSamples.add] takes them. */
        val TELEMETRY_KEYS = listOf(
            "throttle", "brake", "steering", "gear", "speed_kph", "rpm",
            "tyre_temp_surface_fl", "tyre_temp_surface_fr", "tyre_temp_surface_rl", "tyre_temp_surface_rr",
            "tyre_temp_inner_fl", "tyre_temp_inner_fr", "tyre_temp_inner_rl", "tyre_temp_inner_rr",
            "brake_temp_fl", "brake_temp_fr", "brake_temp_rl", "brake_temp_rr",
        )
        val MOTION_KEYS = listOf("g_lat", "g_long", "g_vert")
        val MOTION_EX_KEYS = listOf("front_aero_height_mm", "rear_aero_height_mm")
    }
}

/**
 * Allocation-light decoder for the libtnrp hot-row wire format.
 *
 * Keep this in lockstep with protocol_parser_library/include/tnrp/BinaryRows.h.
 * The Android dashboard consumes the player telemetry and the all-car position
 * row; the graph tabs every telemetry, motion and motion-ex record.
 */
internal object BinaryTelemetryDecoder {
    private const val TAG_TELEMETRY = 1
    private const val TAG_MOTION = 2
    private const val TAG_POSITIONS = 3
    private const val TAG_MOTION_EX = 4
    private const val TELEMETRY_PAYLOAD_BYTES = 48
    private const val MOTION_PAYLOAD_BYTES = 28
    private const val MOTION_EX_PAYLOAD_BYTES = 20

    internal data class Result(
        val latest: HotTelemetry?,
        val latestPositions: MapPositions?,
        val telemetryRows: Int,
        val positionRows: Int,
        val malformed: Boolean,
    )

    /** The newest telemetry and positions of a batch; every hot record also goes to [samples] when given. */
    fun decodeLatest(bytes: ByteArray, samples: BinarySamples? = null): Result {
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        var latest: HotTelemetry? = null
        var latestPositions: MapPositions? = null
        var telemetryRows = 0
        var positionRows = 0

        while (buffer.hasRemaining()) {
            when (buffer.get().toInt() and 0xff) {
                TAG_TELEMETRY -> {
                    if (buffer.remaining() < TELEMETRY_PAYLOAD_BYTES) {
                        return Result(latest, latestPositions, telemetryRows, positionRows, true)
                    }
                    val sessionTime = buffer.float
                    val speedKph = buffer.short.toInt() and 0xffff
                    val rpm = buffer.short.toInt() and 0xffff
                    val gear = buffer.get().toInt()
                    val drs = buffer.get().toInt() and 0xff
                    val rawRevPercent = buffer.get().toInt() and 0xff
                    val rawRevBits = buffer.short.toInt() and 0xffff
                    val throttle = buffer.float
                    val brake = buffer.float
                    val steering = buffer.double
                    val tyreSurfaceRl = buffer.get().toInt() and 0xff
                    val tyreSurfaceRr = buffer.get().toInt() and 0xff
                    val tyreSurfaceFl = buffer.get().toInt() and 0xff
                    val tyreSurfaceFr = buffer.get().toInt() and 0xff
                    val tyreInnerRl = buffer.get().toInt() and 0xff
                    val tyreInnerRr = buffer.get().toInt() and 0xff
                    val tyreInnerFl = buffer.get().toInt() and 0xff
                    val tyreInnerFr = buffer.get().toInt() and 0xff
                    // Four uint16 brake temperatures, rears first as the tyres.
                    val brakeRl = buffer.short.toInt() and 0xffff
                    val brakeRr = buffer.short.toInt() and 0xffff
                    val brakeFl = buffer.short.toInt() and 0xffff
                    val brakeFr = buffer.short.toInt() and 0xffff
                    val engineTemp = buffer.short.toInt() and 0xffff
                    val slm = buffer.get().toInt() and 0xff
                    latest = HotTelemetry(
                        sessionTime = sessionTime,
                        speedKph = speedKph,
                        rpm = rpm,
                        gear = gear,
                        throttle = throttle,
                        brake = brake,
                        steering = steering,
                        drs = drs,
                        revLightsPercent = rawRevPercent.takeUnless { it == 0xff },
                        revLightsBitValue = rawRevBits.takeUnless { it == 0xffff },
                        slm = slm,
                        engineTemp = engineTemp,
                        tyreSurfaceFl = tyreSurfaceFl,
                        tyreSurfaceFr = tyreSurfaceFr,
                        tyreSurfaceRl = tyreSurfaceRl,
                        tyreSurfaceRr = tyreSurfaceRr,
                        tyreInnerFl = tyreInnerFl,
                        tyreInnerFr = tyreInnerFr,
                        tyreInnerRl = tyreInnerRl,
                        tyreInnerRr = tyreInnerRr,
                    )
                    samples?.telemetry?.add(
                        sessionTime,
                        floatArrayOf(
                            throttle, brake, steering.toFloat(), gear.toFloat(), speedKph.toFloat(), rpm.toFloat(),
                            tyreSurfaceFl.toFloat(), tyreSurfaceFr.toFloat(), tyreSurfaceRl.toFloat(), tyreSurfaceRr.toFloat(),
                            tyreInnerFl.toFloat(), tyreInnerFr.toFloat(), tyreInnerRl.toFloat(), tyreInnerRr.toFloat(),
                            brakeFl.toFloat(), brakeFr.toFloat(), brakeRl.toFloat(), brakeRr.toFloat(),
                        ),
                    )
                    telemetryRows++
                }

                TAG_MOTION -> {
                    if (buffer.remaining() < MOTION_PAYLOAD_BYTES) {
                        return Result(latest, latestPositions, telemetryRows, positionRows, true)
                    }
                    val motion = samples?.motion
                    if (motion == null) {
                        buffer.position(buffer.position() + MOTION_PAYLOAD_BYTES)
                    } else {
                        val sessionTime = buffer.float
                        motion.add(sessionTime, floatArrayOf(buffer.double.toFloat(), buffer.double.toFloat(), buffer.double.toFloat()))
                    }
                }

                TAG_MOTION_EX -> {
                    if (buffer.remaining() < MOTION_EX_PAYLOAD_BYTES) {
                        return Result(latest, latestPositions, telemetryRows, positionRows, true)
                    }
                    val motionEx = samples?.motionEx
                    if (motionEx == null) {
                        buffer.position(buffer.position() + MOTION_EX_PAYLOAD_BYTES)
                    } else {
                        val sessionTime = buffer.float
                        motionEx.add(sessionTime, floatArrayOf(buffer.double.toFloat(), buffer.double.toFloat()))
                    }
                }

                TAG_POSITIONS -> {
                    if (buffer.remaining() < 2) {
                        return Result(latest, latestPositions, telemetryRows, positionRows, true)
                    }
                    val playerIndex = buffer.get().toInt() and 0xff
                    val carCount = buffer.get().toInt() and 0xff
                    if (buffer.remaining() < carCount * 16) {
                        return Result(latest, latestPositions, telemetryRows, positionRows, true)
                    }
                    val xByCar = DoubleArray(carCount)
                    val zByCar = DoubleArray(carCount)
                    repeat(carCount) { index ->
                        xByCar[index] = buffer.double
                        zByCar[index] = buffer.double
                    }
                    latestPositions = MapPositions(playerIndex, xByCar, zByCar)
                    positionRows++
                }

                else -> return Result(latest, latestPositions, telemetryRows, positionRows, true)
            }
        }
        return Result(latest, latestPositions, telemetryRows, positionRows, false)
    }

    private fun ByteBuffer.skipChecked(byteCount: Int): Boolean {
        if (byteCount < 0 || remaining() < byteCount) return false
        position(position() + byteCount)
        return true
    }
}

