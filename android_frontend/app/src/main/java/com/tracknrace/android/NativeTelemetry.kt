package com.tracknrace.android

internal class NativeTelemetry(
    private val listener: Listener,
) {
    interface Listener {
        fun onTelemetryRow(json: String)

        fun onTelemetryBinary(bytes: ByteArray)
    }

    companion object {
        init {
            System.loadLibrary("tracknrace_android")
        }

        /**
         * A recording's details read from its index without loading it, as
         * JSON: protocol, trackName, sessionName, sessionType, startTime (ms),
         * duration (s) and drivers. Null for V1–V3 files or an unreadable one.
         */
        @JvmStatic
        external fun nativeSummarize(path: String): String?
    }

    fun start(port: Int): String? = nativeStart(port)

    fun stop() = nativeStop()

    fun setRecording(enabled: Boolean, outputDirectory: String) {
        nativeSetRecording(enabled, outputDirectory)
    }

    // ── A recording played on the phone ─────────────────────────────────────
    // The same engine as the desktop, with no UDP listener; stop() closes it.

    /** Loads [path] paused at its start; an error message, or null when it opened. */
    fun openRecording(path: String): String? = nativeOpenRecording(path)

    fun play() = nativePlay()

    fun pause() = nativePause()

    /** Marks [requestId] as the newest seek, so an older one still running gives way. */
    fun requestSeek(requestId: Long) = nativeRequestSeek(requestId)

    /** Moves to [fraction] of the recording; slow, so called off the UI thread. */
    fun seek(fraction: Float, requestId: Long) = nativeSeek(fraction, requestId)

    fun setSpeed(speed: Float) = nativeSetSpeed(speed)

    fun setDriver(index: Int) = nativeSetDriver(index)

    /** The open page's row families and V6 fields; their current values are restated. */
    fun setRequirements(streamMask: Int, v6Types: IntArray) = nativeSetRequirements(streamMask, v6Types)

    /** While listening for the game: whether the Strategy snapshot is streamed. */
    fun setLiveStrategy(enabled: Boolean) = nativeSetLiveStrategy(enabled)

    /** A paired phone's lap_delta data for the open recording, or null. */
    fun lapDelta(currentLap: Int, comparisonLap: Int, sectorDelta: Boolean): String? =
        nativeLapDelta(currentLap, comparisonLap, sectorDelta)

    /** A paired phone's lap_data data for the open recording, or null. */
    fun lapData(lapNumber: Int, channels: Array<String>): String? = nativeLapData(lapNumber, channels)

    @Suppress("unused") // Invoked by JNI on libtnrp worker threads.
    private fun onNativeRow(json: String) {
        listener.onTelemetryRow(json)
    }

    @Suppress("unused") // Invoked by JNI on libtnrp worker threads.
    private fun onNativeBinary(bytes: ByteArray) {
        listener.onTelemetryBinary(bytes)
    }

    private external fun nativeStart(port: Int): String?

    private external fun nativeStop()

    private external fun nativeSetRecording(enabled: Boolean, outputDirectory: String)

    private external fun nativeOpenRecording(path: String): String?

    private external fun nativePlay()

    private external fun nativePause()

    private external fun nativeRequestSeek(requestId: Long)

    private external fun nativeSeek(fraction: Float, requestId: Long)

    private external fun nativeSetSpeed(speed: Float)

    private external fun nativeSetDriver(index: Int)

    private external fun nativeSetRequirements(streamMask: Int, v6Types: IntArray)

    private external fun nativeSetLiveStrategy(enabled: Boolean)

    private external fun nativeLapDelta(currentLap: Int, comparisonLap: Int, sectorDelta: Boolean): String?

    private external fun nativeLapData(lapNumber: Int, channels: Array<String>): String?
}
