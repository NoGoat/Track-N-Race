package com.tracknrace.android

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Allocation-light decoder for the libtnrp hot-row wire format.
 *
 * Keep this in lockstep with protocol_parser_library/include/tnrp/BinaryRows.h.
 * The Android dashboard only consumes telemetry, so motion/position records are
 * bounds-checked and skipped without constructing intermediate objects.
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
        val telemetryRows: Int,
        val malformed: Boolean,
    )

    fun decodeLatest(bytes: ByteArray): Result {
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        var latest: HotTelemetry? = null
        var telemetryRows = 0

        while (buffer.hasRemaining()) {
            when (buffer.get().toInt() and 0xff) {
                TAG_TELEMETRY -> {
                    if (buffer.remaining() < TELEMETRY_PAYLOAD_BYTES) {
                        return Result(latest, telemetryRows, true)
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
                    buffer.position(buffer.position() + 8) // Surface + inner tyre temperatures.
                    buffer.position(buffer.position() + 8) // Four uint16 brake temperatures.
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
                    )
                    telemetryRows++
                }

                TAG_MOTION -> if (!buffer.skipChecked(MOTION_PAYLOAD_BYTES)) {
                    return Result(latest, telemetryRows, true)
                }

                TAG_MOTION_EX -> if (!buffer.skipChecked(MOTION_EX_PAYLOAD_BYTES)) {
                    return Result(latest, telemetryRows, true)
                }

                TAG_POSITIONS -> {
                    if (buffer.remaining() < 2) return Result(latest, telemetryRows, true)
                    buffer.get() // player index
                    val carCount = buffer.get().toInt() and 0xff
                    if (!buffer.skipChecked(carCount * 16)) {
                        return Result(latest, telemetryRows, true)
                    }
                }

                else -> return Result(latest, telemetryRows, true)
            }
        }
        return Result(latest, telemetryRows, false)
    }

    private fun ByteBuffer.skipChecked(byteCount: Int): Boolean {
        if (byteCount < 0 || remaining() < byteCount) return false
        position(position() + byteCount)
        return true
    }
}

