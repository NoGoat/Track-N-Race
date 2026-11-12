package com.nogoat.tracknrace

import androidx.compose.runtime.Immutable
import org.json.JSONArray
import org.json.JSONObject

/** One marshal zone: where it starts as a fraction of the lap, and its flag (-1 invalid, 0 none, 1 green, 2 blue, 3 yellow). */
@Immutable
internal data class MarshalZoneInfo(val start: Double, val flag: Int)

/** One forecast slot for the session being driven: minutes ahead, weather id and rain chance. */
@Immutable
internal data class ForecastSample(val offsetMinutes: Int, val weather: Int, val rainPercent: Int)

/**
 * The Session packet as the Session tab shows it. Null fields were not in the
 * last row; [available] is false until a session row arrives.
 */
@Immutable
internal data class SessionConditions(
    val available: Boolean = false,
    val weather: Int = -1,
    val trackTemp: Int? = null,
    val airTemp: Int? = null,
    val trackLengthM: Int = 0,
    val trackId: Int = -1,
    val sessionType: Int? = null,
    val totalLaps: Int = 0,
    val timeLeftS: Int? = null,
    val pitSpeedLimit: Int? = null,
    val pitIdealLap: Int = 0,
    val pitLatestLap: Int = 0,
    val rejoinPosition: Int = 0,
    val zones: List<MarshalZoneInfo> = emptyList(),
    /** Only the current session's block, in time order; includes the offset-0 slot when sent. */
    val forecast: List<ForecastSample> = emptyList(),
    /** 0 perfect, 1 approximate. */
    val forecastAccuracy: Int? = null,
    /** 0 none, 1 safety car, 2 virtual safety car, 3 formation lap. */
    val safetyCarStatus: Int = 0,
    /** Minutes since midnight at the circuit. */
    val timeOfDayMinutes: Long? = null,
    val safetyCarPeriods: Int = 0,
    val virtualSafetyCarPeriods: Int = 0,
    val redFlagPeriods: Int = 0,
)

/** One race event as the engine reports it, with the lap it happened on (0 before the first lap). */
@Immutable
internal data class RaceEventEntry(
    val sessionTime: Double,
    val code: String,
    val carIndex: Int? = null,
    val lapTimeS: Double? = null,
    val safetyCarType: Int? = null,
    val eventType: Int? = null,
    val penaltyType: Int? = null,
    val infringementType: Int? = null,
    val penaltyTimeS: Int? = null,
    val overtakingCarIndex: Int? = null,
    val overtakenCarIndex: Int? = null,
    val flashbackSessionTime: Double? = null,
    val lap: Int = 0,
) {
    /** Rows can repeat (a resubscription or a seek replays them); this identifies one event. */
    val identity: String
        get() = "$code@${"%.3f".format(java.util.Locale.ROOT, sessionTime)}#${carIndex ?: -1}#${overtakenCarIndex ?: -1}"
}

/** At most this many events are kept, newest last, as on the desktop. */
internal const val MAX_RACE_EVENTS = 500

internal fun parseSessionConditions(row: JSONObject, previous: SessionConditions): SessionConditions {
    val sessionType = row.optionalInt("session_type") ?: previous.sessionType
    return SessionConditions(
        available = true,
        weather = row.optionalInt("weather") ?: previous.weather,
        trackTemp = row.optionalInt("track_temp") ?: previous.trackTemp,
        airTemp = row.optionalInt("air_temp") ?: previous.airTemp,
        trackLengthM = row.optionalInt("track_length_m")?.takeIf { it > 0 } ?: previous.trackLengthM,
        trackId = row.optionalInt("track_id") ?: previous.trackId,
        sessionType = sessionType,
        totalLaps = row.optionalInt("total_laps") ?: previous.totalLaps,
        timeLeftS = row.optionalInt("session_time_left") ?: previous.timeLeftS,
        pitSpeedLimit = row.optionalInt("pit_speed_limit")?.takeIf { it > 0 } ?: previous.pitSpeedLimit,
        pitIdealLap = row.optionalInt("pit_stop_window_ideal_lap") ?: previous.pitIdealLap,
        pitLatestLap = row.optionalInt("pit_stop_window_latest_lap") ?: previous.pitLatestLap,
        rejoinPosition = row.optionalInt("pit_stop_rejoin_position") ?: previous.rejoinPosition,
        zones = row.optJSONArray("marshal_zones")?.let(::parseZones) ?: previous.zones,
        forecast = row.optJSONArray("weather_forecast_samples")?.let { currentForecast(it, sessionType) } ?: previous.forecast,
        forecastAccuracy = row.optionalInt("forecast_accuracy") ?: previous.forecastAccuracy,
        safetyCarStatus = row.optionalInt("safety_car_status") ?: previous.safetyCarStatus,
        timeOfDayMinutes = row.optionalLong("time_of_day") ?: previous.timeOfDayMinutes,
        safetyCarPeriods = row.optionalInt("num_safety_car_periods") ?: previous.safetyCarPeriods,
        virtualSafetyCarPeriods = row.optionalInt("num_virtual_sc_periods") ?: previous.virtualSafetyCarPeriods,
        redFlagPeriods = row.optionalInt("num_red_flag_periods") ?: previous.redFlagPeriods,
    )
}

private fun parseZones(array: JSONArray): List<MarshalZoneInfo> = buildList {
    repeat(array.length()) { index ->
        val zone = array.optJSONObject(index) ?: return@repeat
        val flag = zone.optInt("flag", -1)
        if (flag != -1) add(MarshalZoneInfo(zone.optDouble("zone_start", 0.0), flag))
    }
}

/**
 * The packet carries forecasts for several sessions, each a block starting
 * again at offset 0. The block for the session being driven: matched by
 * session_type when samples carry it, otherwise the first block, as the
 * library's currentSessionForecast does.
 */
private fun currentForecast(array: JSONArray, sessionType: Int?): List<ForecastSample> {
    val all = buildList {
        repeat(array.length()) { index ->
            val sample = array.optJSONObject(index) ?: return@repeat
            add(sample)
        }
    }
    fun JSONObject.toSample() = ForecastSample(
        offsetMinutes = optInt("time_offset"),
        weather = optInt("weather"),
        rainPercent = optInt("rain_percentage"),
    )
    if (sessionType != null) {
        val matched = all.filter { it.optionalInt("session_type") == sessionType }.map { it.toSample() }
        if (matched.isNotEmpty()) return matched
    }
    val first = mutableListOf<ForecastSample>()
    for (sample in all.map { it.toSample() }) {
        if (first.isNotEmpty() && sample.offsetMinutes <= first.last().offsetMinutes) break
        first += sample
    }
    return first
}

/**
 * A live event names its own lap (the engine stamps lap_num on it), which a
 * backfilled one needs; [lap] is used for rows without one, such as a
 * recording's catalogue.
 */
internal fun parseRaceEvent(row: JSONObject, lap: Int): RaceEventEntry? {
    val code = row.optString("code").takeIf { it.isNotEmpty() } ?: return null
    return RaceEventEntry(
        sessionTime = row.optDouble("session_time", 0.0),
        code = code,
        carIndex = row.optionalInt("car_idx"),
        lapTimeS = row.optionalDouble("lap_time_s"),
        safetyCarType = row.optionalInt("safety_car_type"),
        eventType = row.optionalInt("event_type"),
        penaltyType = row.optionalInt("penalty_type"),
        infringementType = row.optionalInt("infringement_type"),
        penaltyTimeS = row.optionalInt("penalty_time_s"),
        overtakingCarIndex = row.optionalInt("overtaking_car_idx"),
        overtakenCarIndex = row.optionalInt("being_overtaken_car_idx"),
        flashbackSessionTime = row.optionalDouble("flashback_session_time"),
        lap = row.optionalInt("lap_num") ?: lap,
    )
}

private fun JSONObject.optionalLong(name: String): Long? =
    if (!has(name) || isNull(name)) null else (opt(name) as? Number)?.toLong()

/** Retirements are reported once per car: a later RTMT is dropped, and a PENA retirement replaces an RTMT. */
internal fun isRetirement(event: RaceEventEntry): Boolean =
    event.code == "RTMT" || (event.code == "PENA" && event.penaltyType == 16)

/**
 * Adds [event] to [events] as the desktop store does: one retirement per car,
 * no repeats, oldest dropped past [MAX_RACE_EVENTS], kept in time order.
 */
internal fun appendRaceEvent(events: List<RaceEventEntry>, event: RaceEventEntry): List<RaceEventEntry> {
    if (events.any { it.identity == event.identity }) return events
    if (isRetirement(event) && event.carIndex != null) {
        val prior = events.indexOfFirst { isRetirement(it) && it.carIndex == event.carIndex }
        if (prior != -1) {
            if (events[prior].code != "RTMT" || event.code != "PENA") return events
            return (events.subList(0, prior) + events.subList(prior + 1, events.size) + event).takeLast(MAX_RACE_EVENTS)
        }
    }
    val next = if (events.isEmpty() || events.last().sessionTime <= event.sessionTime) {
        events + event
    } else {
        (events + event).sortedBy { it.sessionTime }
    }
    return next.takeLast(MAX_RACE_EVENTS)
}

/** The desktop's circuit names by track id (tnrp TRACK_NAMES). */
internal val TrackNames: Map<Int, String> = mapOf(
    0 to "Australian Grand Prix",
    2 to "Chinese Grand Prix",
    3 to "Bahrain Grand Prix",
    4 to "Spanish Grand Prix",
    5 to "Monaco Grand Prix",
    6 to "Canadian Grand Prix",
    7 to "British Grand Prix",
    9 to "Hungarian Grand Prix",
    10 to "Belgian Grand Prix",
    11 to "Italian Grand Prix",
    12 to "Singapore Grand Prix",
    13 to "Japanese Grand Prix",
    14 to "Abu Dhabi Grand Prix",
    15 to "United States Grand Prix",
    16 to "São Paulo Grand Prix",
    17 to "Austrian Grand Prix",
    19 to "Mexico City Grand Prix",
    20 to "Azerbaijan Grand Prix",
    26 to "Dutch Grand Prix",
    27 to "Emilia Romagna Grand Prix",
    29 to "Saudi Arabian Grand Prix",
    30 to "Miami Grand Prix",
    31 to "Las Vegas Grand Prix",
    32 to "Qatar Grand Prix",
    39 to "British Grand Prix",
    40 to "Austrian Grand Prix",
    41 to "Dutch Grand Prix",
    42 to "Madrid Grand Prix",
)

private val WeatherNames = listOf("Clear", "Light Cloud", "Overcast", "Light Rain", "Heavy Rain", "Storm")

/** The weather's name from the desktop's label catalogue, else the built-in one. */
internal fun weatherName(weather: Int, labels: Map<String, String>): String =
    labels["weather.$weather"] ?: WeatherNames.getOrNull(weather) ?: "—"

private val SessionNames = mapOf(
    1 to "Practice 1", 2 to "Practice 2", 3 to "Practice 3", 4 to "Short Practice",
    5 to "Qualifying 1", 6 to "Qualifying 2", 7 to "Qualifying 3", 8 to "Short Qualifying",
    9 to "One-Shot Qualifying", 10 to "Sprint Shootout 1", 11 to "Sprint Shootout 2",
    12 to "Sprint Shootout 3", 13 to "Short Sprint Shootout", 14 to "One-Shot Sprint Shootout",
    15 to "Race", 16 to "Race 2", 17 to "Race 3", 18 to "Time Trial",
)

internal fun sessionName(sessionType: Int?, labels: Map<String, String>): String? =
    sessionType?.takeIf { it > 0 }?.let { labels["session.$it"] ?: SessionNames[it] }

/** What kind of event a row is, for its icon and colour on the Session tab. */
internal enum class RaceEventKind { LAP, OVERTAKE, PENALTY, SERVED, FLAG, RETIRED, START, INFO }

/** One event as the Session tab lists it, or null for codes the desktop does not list either. */
internal data class RaceEventText(val text: String, val kind: RaceEventKind)

/**
 * The desktop Session page's wording for each event (SessionPanel formatEvent).
 * [name] gives a car's display name.
 */
internal fun describeRaceEvent(event: RaceEventEntry, labels: Map<String, String>, name: (Int?) -> String): RaceEventText? =
    when (event.code) {
        "FTLP" -> RaceEventText("Fastest Lap — ${name(event.carIndex)} ${formatLapSeconds(event.lapTimeS ?: 0.0)}", RaceEventKind.LAP)
        "DRSE" -> RaceEventText("DRS Enabled", RaceEventKind.FLAG)
        "DRSD" -> RaceEventText("DRS Disabled", RaceEventKind.INFO)
        "RDFL" -> RaceEventText("Red Flag", RaceEventKind.FLAG)
        "CHQF" -> RaceEventText("Chequered Flag", RaceEventKind.FLAG)
        "LGOT" -> RaceEventText("Lights Out", RaceEventKind.START)
        "SSTA" -> RaceEventText("Session Start", RaceEventKind.START)
        "SEND" -> RaceEventText("Session End", RaceEventKind.START)
        "RTMT" -> RaceEventText("Retired — ${name(event.carIndex)}", RaceEventKind.RETIRED)
        "RCWN" -> RaceEventText("Race Winner — ${name(event.carIndex)}", RaceEventKind.LAP)
        "DTSV" -> RaceEventText("Drive-through served — ${name(event.carIndex)}", RaceEventKind.SERVED)
        "SGSV" -> RaceEventText("Stop-go served — ${name(event.carIndex)}", RaceEventKind.SERVED)
        "SCAR" -> {
            val type = event.safetyCarType ?: 0
            val label = safetyCarTypeLabel(type)
            val action = safetyCarActionLabel(type, event.eventType ?: 0)
            RaceEventText(if (action != null) "$label — $action" else label, RaceEventKind.FLAG)
        }
        "PENA" -> {
            val type = event.penaltyType ?: 0
            labels["penalty.$type"]?.let { penalty ->
                val seconds = if ((type == 1 || type == 4) && (event.penaltyTimeS ?: 0) > 0) " ${event.penaltyTimeS}s" else ""
                val infringement = event.infringementType?.let { labels["infringe.$it"] }
                val text = buildString {
                    append(penalty).append(seconds).append(" — ").append(name(event.carIndex))
                    if (infringement != null) append(" — ").append(infringement)
                }
                RaceEventText(text, if (type == 16) RaceEventKind.RETIRED else RaceEventKind.PENALTY)
            }
        }
        "OVTK" -> RaceEventText("${name(event.overtakingCarIndex)} passed ${name(event.overtakenCarIndex)}", RaceEventKind.OVERTAKE)
        else -> null
    }

// The approved Race 6 design's wording ("VSC — Deployed"); the desktop banner
// leaves a deployment unsuffixed.
private fun safetyCarTypeLabel(type: Int): String = when (type) {
    2 -> "VSC"
    3 -> "Formation Lap"
    else -> "Safety Car"
}

private fun safetyCarActionLabel(type: Int, action: Int): String? {
    if ((type == 1 || type == 2) && action == 0) return "Deployed"
    if ((type == 1 || type == 2) && action == 1) return "Ending"
    return when (action) {
        0 -> "Deployed"
        1 -> "Returning"
        2 -> "Returned"
        3 -> "Resume Race"
        else -> null
    }
}

/** 73.941 → "1:13.941". */
internal fun formatLapSeconds(seconds: Double): String {
    val whole = seconds.coerceAtLeast(0.0)
    val minutes = (whole / 60).toInt()
    val rest = whole - minutes * 60
    return "$minutes:${"%06.3f".format(java.util.Locale.ROOT, rest)}"
}

/** Session clock as mm:ss, or h:mm:ss past an hour. */
internal fun formatSessionClock(seconds: Double): String {
    val total = seconds.coerceAtLeast(0.0).toLong()
    val h = total / 3600
    val m = (total % 3600) / 60
    val s = total % 60
    return if (h > 0) "%d:%02d:%02d".format(java.util.Locale.ROOT, h, m, s) else "%02d:%02d".format(java.util.Locale.ROOT, m, s)
}

/** Minutes since midnight → "2:31 PM". */
internal fun formatTimeOfDay(minutes: Long): String {
    val h = ((minutes / 60) % 24).toInt()
    val m = (minutes % 60).toInt()
    val hour12 = if (h % 12 == 0) 12 else h % 12
    return "%d:%02d %s".format(java.util.Locale.ROOT, hour12, m, if (h >= 12) "PM" else "AM")
}
