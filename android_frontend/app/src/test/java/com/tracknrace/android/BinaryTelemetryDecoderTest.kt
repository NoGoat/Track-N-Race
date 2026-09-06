package com.tracknrace.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class BinaryTelemetryDecoderTest {
    @Test
    fun decodesTelemetryAndSkipsUnusedRows() {
        val bytes = ByteBuffer.allocate(29 + 49 + 21).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(2.toByte()).putFloat(1f).putDouble(0.1).putDouble(0.2).putDouble(0.3)
            putTelemetry(speed = 321, rpm = 12_345, gear = 8, revPercent = 87, revBits = 0x5a5a)
            put(4.toByte()).putFloat(2f).putDouble(10.0).putDouble(20.0)
        }.array()

        val result = BinaryTelemetryDecoder.decodeLatest(bytes)

        assertFalse(result.malformed)
        assertEquals(1, result.telemetryRows)
        assertEquals(321, result.latest?.speedKph)
        assertEquals(12_345, result.latest?.rpm)
        assertEquals(8, result.latest?.gear)
        assertEquals(87, result.latest?.revLightsPercent)
        assertEquals(0x5a5a, result.latest?.revLightsBitValue)
        assertEquals(104, result.latest?.engineTemp)
        assertEquals(1, result.latest?.slm)
    }

    @Test
    fun mapsUnavailableRevLightsToNull() {
        val bytes = ByteBuffer.allocate(49).order(ByteOrder.LITTLE_ENDIAN).apply {
            putTelemetry(speed = 0, rpm = 0, gear = 0, revPercent = 0xff, revBits = 0xffff)
        }.array()

        val result = BinaryTelemetryDecoder.decodeLatest(bytes)

        assertFalse(result.malformed)
        assertNull(result.latest?.revLightsPercent)
        assertNull(result.latest?.revLightsBitValue)
    }

    @Test
    fun rejectsTruncatedRecordWithoutThrowing() {
        val result = BinaryTelemetryDecoder.decodeLatest(byteArrayOf(1, 0, 0))
        assertTrue(result.malformed)
        assertEquals(0, result.telemetryRows)
    }

    private fun ByteBuffer.putTelemetry(
        speed: Int,
        rpm: Int,
        gear: Int,
        revPercent: Int,
        revBits: Int,
    ) {
        put(1.toByte())
        putFloat(42.5f)
        putShort(speed.toShort())
        putShort(rpm.toShort())
        put(gear.toByte())
        put(1.toByte())
        put(revPercent.toByte())
        putShort(revBits.toShort())
        putFloat(0.75f)
        putFloat(0.25f)
        putDouble(-0.125)
        repeat(8) { put((80 + it).toByte()) }
        repeat(4) { putShort((500 + it).toShort()) }
        putShort(104)
        put(1.toByte())
    }
}
