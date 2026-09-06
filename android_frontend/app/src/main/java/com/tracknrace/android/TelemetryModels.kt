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
)

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
    val position: Int = 0,
    val lapNumber: Int = 0,
    val totalLaps: Int = 0,
    val currentLapMs: Int = 0,
    val lastLapMs: Int = 0,
    val lapInvalid: Boolean = false,
    val ersPercent: Int = 0,
    val fuelLaps: Double = 0.0,
    val brakeBias: Int = 0,
    val tyreCompound: Int = 0,
    val tyreAgeLaps: Int = 0,
    val sessionType: Int? = null,
    val tyreSets: List<TyreSetEntry> = emptyList(),
    val aeroMode: String = "drs",
    val protocolYear: Int? = null,
    val formula: Int? = null,
    val labels: Map<String, String> = emptyMap(),
)

internal data class AndroidSettings(
    val source: String = PairedTelemetryClient.SOURCE_DIRECT,
    val recordingEnabled: Boolean = false,
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

