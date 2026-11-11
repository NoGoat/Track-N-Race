package com.nogoat.tracknrace

import java.nio.ByteBuffer

internal class NativeTelemetry(
    private val listener: Listener,
) {
    interface Listener {
        fun onTelemetryRow(json: String)

        fun onTelemetryBinary(bytes: ByteArray)

        /**
         * A graph page's history read back from the session the engine keeps,
         * as a V6H1 payload ([V6HistoryDecoder]). [payload] is the engine's
         * memory, valid only until this returns.
         */
        fun onTelemetryHistory(payload: ByteBuffer)
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

        /**
         * Exports the recording at [path] as Parquet tables into a new
         * "<recording>_parquet" folder inside [destDir]. Blocking; call it off
         * the main thread. JSON: {"ok":true,"folder":...} or {"ok":false,"error":...}.
         */
        @JvmStatic
        external fun nativeExportParquet(path: String, destDir: String): String
    }

    /**
     * Listens for the game on [port], copying every datagram unchanged to
     * [forwardTargets]; an error message, or null when the listener is bound.
     */
    fun start(port: Int, forwardTargets: List<UdpForwardTarget> = emptyList()): String? = nativeStart(
        port,
        Array(forwardTargets.size) { forwardTargets[it].address },
        IntArray(forwardTargets.size) { forwardTargets[it].port },
    )

    /**
     * Rebinds the running listener to [port] and [forwardTargets] without
     * ending its session; an error message, or null when it is bound.
     */
    fun restartUdp(port: Int, forwardTargets: List<UdpForwardTarget>): String? = nativeRestartUdp(
        port,
        Array(forwardTargets.size) { forwardTargets[it].address },
        IntArray(forwardTargets.size) { forwardTargets[it].port },
    )

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

    /**
     * While listening for the game: the open graph page's row families
     * ([rowMask] of [DataRow]s, 0 for none) are streamed, and their history is
     * read back from the session the engine keeps, [windowSeconds] behind now
     * or all of it when negative. It arrives at [Listener.onTelemetryHistory].
     */
    fun setLiveHistory(rowMask: Int, windowSeconds: Float) = nativeSetLiveHistory(rowMask, windowSeconds)

    /**
     * A paired phone's lap_delta data for the open recording, or null. Each
     * lap's driver is a car index, -1 the selected driver.
     */
    fun lapDelta(currentLap: Int, comparisonLap: Int, sectorDelta: Boolean, currentDriver: Int, comparisonDriver: Int): String? =
        nativeLapDelta(currentLap, comparisonLap, sectorDelta, currentDriver, comparisonDriver)

    /**
     * A paired phone's lap_data data for the open recording, or null. For
     * Charts, which draw no map, the lap is read from the recording's columns
     * and carries no positions. [driver] is a car index, -1 the selected driver.
     */
    fun lapData(lapNumber: Int, channels: Array<String>, forCharts: Boolean, driver: Int): String? =
        nativeLapData(lapNumber, channels, forCharts, driver)

    /** A paired phone's driver_laps data for one driver of the open recording, or null. */
    fun driverLaps(driver: Int): String? = nativeDriverLaps(driver)

    /**
     * A paired phone's braking_zones data for the open recording, or null:
     * [currentLap]'s braking zones, matched with [comparisonLap]'s (0 = none).
     */
    fun brakingZones(currentLap: Int, comparisonLap: Int, currentDriver: Int, comparisonDriver: Int): String? =
        nativeBrakingZones(currentLap, comparisonLap, currentDriver, comparisonDriver)

    @Suppress("unused") // Invoked by JNI on libtnrp worker threads.
    private fun onNativeRow(json: String) {
        listener.onTelemetryRow(json)
    }

    @Suppress("unused") // Invoked by JNI on libtnrp worker threads.
    private fun onNativeBinary(bytes: ByteArray) {
        listener.onTelemetryBinary(bytes)
    }

    @Suppress("unused") // Invoked by JNI on libtnrp's live read thread.
    private fun onNativeHistory(payload: ByteBuffer) {
        listener.onTelemetryHistory(payload)
    }

    private external fun nativeStart(port: Int, forwardAddresses: Array<String>, forwardPorts: IntArray): String?

    private external fun nativeRestartUdp(port: Int, forwardAddresses: Array<String>, forwardPorts: IntArray): String?

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

    private external fun nativeSetLiveHistory(rowMask: Int, windowSeconds: Float)

    private external fun nativeLapDelta(
        currentLap: Int,
        comparisonLap: Int,
        sectorDelta: Boolean,
        currentDriver: Int,
        comparisonDriver: Int,
    ): String?

    private external fun nativeLapData(lapNumber: Int, channels: Array<String>, forCharts: Boolean, driver: Int): String?

    private external fun nativeDriverLaps(driver: Int): String?

    private external fun nativeBrakingZones(currentLap: Int, comparisonLap: Int, currentDriver: Int, comparisonDriver: Int): String?

    // Analysis map layer colours (tnrp/AnalysisMapPaint.h): {"dark":…,"light":…}.
    // Not wired to any UI yet.
    external fun nativeAnalysisMapPaletteJson(): String

    // Track maps (tnrp/TrackMaps.h), the current session's library. Not wired
    // to any UI yet. Changes return {"ok","id","error"} JSON.
    external fun nativeTrackMapsAddBuiltins(maps: Array<String>): Int

    external fun nativeTrackMapsSetDirectory(directory: String)

    external fun nativeTrackMapsSetOptions(moddedMaps: Boolean, generateMaps: Boolean)

    external fun nativeTrackMapsCatalog(): String

    external fun nativeTrackMapsGet(id: String): String?

    external fun nativeTrackMapsState(): String?

    external fun nativeTrackMapsPin(trackKey: String, mapId: String)

    external fun nativeTrackMapsRename(id: String, name: String): String

    external fun nativeTrackMapsDelete(id: String): String

    external fun nativeTrackMapsEdit(id: String, editJson: String): String

    external fun nativeTrackMapsImport(path: String): String

    external fun nativeTrackMapsExport(id: String, path: String): String

    external fun nativeTrackMapsGenerate(path: String): String

    external fun nativeTrackMapsResolveRecording(path: String): String?
}
