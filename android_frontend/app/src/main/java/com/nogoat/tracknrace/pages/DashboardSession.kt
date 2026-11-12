package com.nogoat.tracknrace.pages

import androidx.annotation.DrawableRes
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.material3.VerticalDivider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nogoat.tracknrace.ForecastSample
import com.nogoat.tracknrace.MarshalZoneInfo
import com.nogoat.tracknrace.R
import com.nogoat.tracknrace.RaceEventEntry
import com.nogoat.tracknrace.RaceEventKind
import com.nogoat.tracknrace.SessionConditions
import com.nogoat.tracknrace.TelemetryStore
import com.nogoat.tracknrace.TimingDriver
import com.nogoat.tracknrace.TrackNames
import com.nogoat.tracknrace.describeRaceEvent
import com.nogoat.tracknrace.formatSessionClock
import com.nogoat.tracknrace.formatTimeOfDay
import com.nogoat.tracknrace.sessionName
import com.nogoat.tracknrace.weatherName
import java.util.Locale

// The Dashboard's Session tab: the desktop Session page without its map (the
// Map tab has one), split by a Conditions / Race switch. Conditions is the
// weather now, the forecast and the session facts; Race is the track status,
// the marshal zones and the race control events by lap.

private enum class SessionView(val title: String) { CONDITIONS("Conditions"), RACE("Race") }

/** Small caps section labels, as the other dashboard tabs' captions. */
private val Caption = TextStyle(fontSize = 10.sp, fontWeight = FontWeight.Normal, letterSpacing = 1.sp)

@Composable
internal fun SessionTab(store: TelemetryStore, landscape: Boolean) {
    val conditions by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.sessionConditions } }
    val events by remember(store) { derivedStateOf { store.raceEvents } }
    val labels by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.cold.labels } }
    val drivers by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.timing.drivers } }
    val lap by remember(store) { derivedStateOf { store.sessionLapNumber } }
    var chosen by rememberSaveable { mutableIntStateOf(SessionView.CONDITIONS.ordinal) }
    val view = SessionView.entries[chosen]
    val choose = { next: SessionView -> chosen = next.ordinal }

    if (!conditions.available) {
        SessionMessage()
        return
    }
    if (landscape) {
        LandscapeSession(conditions, events, labels, drivers, lap, view, choose)
    } else {
        PortraitSession(conditions, events, labels, drivers, lap, view, choose)
    }
}

@Composable
private fun SessionMessage() {
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("Waiting for the session", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurface)
        Text(
            "Weather, the forecast, flags and race events appear once the game sends its session data.",
            Modifier.padding(top = 6.dp),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
    }
}

@Composable
private fun PortraitSession(
    conditions: SessionConditions,
    events: List<RaceEventEntry>,
    labels: Map<String, String>,
    drivers: Map<Int, TimingDriver>,
    lap: Int,
    view: SessionView,
    choose: (SessionView) -> Unit,
) {
    Column(Modifier.fillMaxSize()) {
        SessionTitle(conditions, labels, Modifier.padding(horizontal = 16.dp))
        ViewSwitch(view, choose, Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 12.dp))
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        when (view) {
            SessionView.CONDITIONS -> Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                WeatherNow(conditions, labels, Modifier.padding(horizontal = 16.dp))
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                Forecast(conditions, labels, barWidth = 76.dp, Modifier.padding(horizontal = 16.dp))
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                FactsGrid(conditions, lap, Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp, bottom = 12.dp))
            }
            SessionView.RACE -> Column(Modifier.fillMaxSize()) {
                TrackStatus(conditions, Modifier.padding(horizontal = 16.dp))
                MarshalZones(conditions.zones, Modifier.padding(start = 16.dp, end = 16.dp, top = 4.dp, bottom = 12.dp))
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                EventsByLap(events, labels, drivers, Modifier.weight(1f), horizontalPadding = 16.dp)
            }
        }
    }
}

@Composable
private fun LandscapeSession(
    conditions: SessionConditions,
    events: List<RaceEventEntry>,
    labels: Map<String, String>,
    drivers: Map<Int, TimingDriver>,
    lap: Int,
    view: SessionView,
    choose: (SessionView) -> Unit,
) {
    // Two columns inside the 24dp margins the landscape Drive tab uses.
    Row(Modifier.fillMaxSize().padding(horizontal = 24.dp)) {
        Column(Modifier.width(336.dp).fillMaxHeight().verticalScroll(rememberScrollState()).padding(end = 16.dp)) {
            SessionTitle(conditions, labels, Modifier, compact = true)
            ViewSwitch(view, choose, Modifier.fillMaxWidth().padding(bottom = 10.dp))
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            when (view) {
                SessionView.CONDITIONS -> {
                    WeatherNow(conditions, labels, Modifier)
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    FactsGrid(conditions, lap, Modifier.padding(top = 12.dp, bottom = 8.dp))
                }
                SessionView.RACE -> {
                    TrackStatus(conditions, Modifier)
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    MarshalZones(conditions.zones, Modifier.padding(top = 10.dp, bottom = 12.dp))
                }
            }
        }
        VerticalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Box(Modifier.weight(1f).fillMaxHeight()) {
            when (view) {
                SessionView.CONDITIONS -> Forecast(
                    conditions,
                    labels,
                    barWidth = 150.dp,
                    Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp),
                )
                SessionView.RACE -> EventsByLap(events, labels, drivers, Modifier.fillMaxSize(), horizontalPadding = 16.dp)
            }
        }
    }
}

// ── Header ──────────────────────────────────────────────────────────────────

@Composable
private fun SessionTitle(conditions: SessionConditions, labels: Map<String, String>, modifier: Modifier, compact: Boolean = false) {
    val scheme = MaterialTheme.colorScheme
    val circuit = TrackNames[conditions.trackId] ?: "Session"
    val session = sessionName(conditions.sessionType, labels)
    Row(
        modifier.fillMaxWidth().height(if (compact) 44.dp else 52.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            buildString {
                append(circuit)
                if (session != null) append(" · ").append(session)
            },
            Modifier.weight(1f),
            style = MaterialTheme.typography.titleMedium.copy(fontSize = if (compact) 15.sp else 16.sp),
            color = scheme.onSurface,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        conditions.timeLeftS?.takeIf { it > 0 }?.let { left ->
            Row(verticalAlignment = Alignment.Bottom) {
                Text(formatSessionClock(left.toDouble()), style = TextStyle(fontSize = 18.sp, fontWeight = FontWeight.Light), color = scheme.onSurface)
                Text(" left", Modifier.padding(bottom = 2.dp), style = MaterialTheme.typography.labelSmall, color = scheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun ViewSwitch(view: SessionView, choose: (SessionView) -> Unit, modifier: Modifier) {
    SingleChoiceSegmentedButtonRow(modifier) {
        SessionView.entries.forEachIndexed { index, entry ->
            SegmentedButton(
                selected = entry == view,
                onClick = { choose(entry) },
                shape = SegmentedButtonDefaults.itemShape(index, SessionView.entries.size),
            ) {
                Text(entry.title, maxLines = 1)
            }
        }
    }
}

// ── Conditions ──────────────────────────────────────────────────────────────

@DrawableRes
private fun weatherIcon(weather: Int): Int = when (weather) {
    0 -> R.drawable.ic_weather_clear
    1 -> R.drawable.ic_weather_light_cloud
    3 -> R.drawable.ic_weather_light_rain
    4 -> R.drawable.ic_weather_heavy_rain
    5 -> R.drawable.ic_weather_storm
    else -> R.drawable.ic_weather_overcast
}

private fun weatherTint(weather: Int, c: DashPalette): Color = when (weather) {
    0 -> c.caution
    1 -> c.tyreBands[3]
    2 -> c.med
    5 -> c.best
    else -> c.cold
}

/** The rain chance at offset 0 when the forecast carries one. */
private fun rainNow(conditions: SessionConditions): Int? =
    conditions.forecast.firstOrNull { it.offsetMinutes == 0 }?.rainPercent

@Composable
private fun WeatherNow(conditions: SessionConditions, labels: Map<String, String>, modifier: Modifier) {
    val c = LocalDashPalette.current
    val scheme = MaterialTheme.colorScheme
    Row(
        modifier.fillMaxWidth().height(76.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Icon(painterResource(weatherIcon(conditions.weather)), null, Modifier.size(36.dp), tint = weatherTint(conditions.weather, c))
        Column(Modifier.weight(1f)) {
            Text(weatherName(conditions.weather, labels), style = TextStyle(fontSize = 22.sp, fontWeight = FontWeight.Light), color = scheme.onSurface)
            Row(Modifier.padding(top = 2.dp)) {
                val small = MaterialTheme.typography.bodySmall
                Text("Track ", style = small, color = scheme.onSurfaceVariant)
                Text(temperature(conditions.trackTemp), style = small.copy(fontWeight = FontWeight.Medium), color = c.tyreBands[3])
                Text(" · Air ", style = small, color = scheme.onSurfaceVariant)
                Text(temperature(conditions.airTemp), style = small.copy(fontWeight = FontWeight.Medium), color = c.gain)
            }
        }
        Column(horizontalAlignment = Alignment.End) {
            Text("RAIN", style = Caption, color = scheme.onSurfaceVariant)
            Text(rainNow(conditions)?.let { "$it%" } ?: "—", Modifier.padding(top = 2.dp), style = TextStyle(fontSize = 18.sp, fontWeight = FontWeight.Light), color = scheme.onSurface)
        }
    }
}

private fun temperature(value: Int?): String = value?.let { "$it°C" } ?: "—"

@Immutable
private data class ForecastRow(val label: String, val sample: ForecastSample, val now: Boolean)

@Composable
private fun Forecast(conditions: SessionConditions, labels: Map<String, String>, barWidth: Dp, modifier: Modifier) {
    val scheme = MaterialTheme.colorScheme
    // Now, then up to five slots ahead, as the desktop's forecast strip.
    val rows = remember(conditions.weather, conditions.forecast) {
        buildList {
            add(ForecastRow("Now", ForecastSample(0, conditions.weather, rainNow(conditions) ?: 0), now = true))
            conditions.forecast.filter { it.offsetMinutes > 0 }.take(5).forEach {
                add(ForecastRow("+${it.offsetMinutes}m", it, now = false))
            }
        }
    }
    Column(modifier.padding(bottom = 6.dp)) {
        Row(Modifier.fillMaxWidth().height(30.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("FORECAST", Modifier.weight(1f), style = Caption, color = scheme.onSurfaceVariant)
            Text("RAIN CHANCE", style = Caption, color = scheme.onSurfaceVariant)
        }
        rows.forEachIndexed { index, row ->
            val changed = index == 0 || rows[index - 1].sample.weather != row.sample.weather
            ForecastLine(row, labels, first = index == 0, last = index == rows.lastIndex, changed = changed, barWidth = barWidth)
        }
    }
}

@Composable
private fun ForecastLine(row: ForecastRow, labels: Map<String, String>, first: Boolean, last: Boolean, changed: Boolean, barWidth: Dp) {
    val c = LocalDashPalette.current
    val scheme = MaterialTheme.colorScheme
    val weather = row.sample.weather
    val line = scheme.outlineVariant
    val dot = if (weather >= 3) c.cold else weatherTint(weather, c)
    val rain = row.sample.rainPercent.coerceIn(0, 100)
    Row(
        Modifier
            .fillMaxWidth()
            .height(36.dp)
            .then(if (row.now) Modifier.background(scheme.surfaceContainer) else Modifier)
            .semantics(mergeDescendants = true) {
                contentDescription = "${row.label}: ${weatherName(weather, labels)}, rain $rain%"
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            row.label,
            Modifier.width(42.dp),
            style = MaterialTheme.typography.bodyMedium.copy(fontWeight = if (row.now) FontWeight.Bold else FontWeight.Normal),
            color = if (row.now) scheme.primary else scheme.onSurfaceVariant,
        )
        // The timeline: a line joining the slots, a dot per slot in its weather's colour.
        Box(
            Modifier.width(20.dp).fillMaxHeight().drawBehind {
                val x = size.width / 2
                val stroke = 2.dp.toPx()
                if (!first) drawLine(line, Offset(x, 0f), Offset(x, size.height / 2), stroke)
                if (!last) drawLine(line, Offset(x, size.height / 2), Offset(x, size.height), stroke)
                drawCircle(dot, radius = 5.dp.toPx(), center = Offset(x, size.height / 2))
            },
        )
        Icon(painterResource(weatherIcon(weather)), null, Modifier.padding(start = 6.dp, end = 6.dp).size(20.dp), tint = weatherTint(weather, c))
        Text(
            weatherName(weather, labels),
            Modifier.weight(1f),
            style = MaterialTheme.typography.bodyMedium.copy(fontWeight = if (changed) FontWeight.Medium else FontWeight.Normal),
            color = if (changed) scheme.onSurface else scheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        // The rain chance bar, from B's first Conditions design.
        Box(
            Modifier.width(barWidth).height(4.dp).background(scheme.surfaceContainerHighest, RoundedCornerShape(2.dp)),
        ) {
            Box(
                Modifier
                    .fillMaxHeight()
                    .fillMaxWidth(maxOf(rain, 2) / 100f)
                    .background(c.cold, RoundedCornerShape(2.dp)),
            )
        }
        Text(
            "$rain%",
            Modifier.width(44.dp),
            style = MaterialTheme.typography.bodyMedium.copy(fontWeight = FontWeight.Medium),
            color = if (rain >= 50) c.cold else scheme.onSurfaceVariant,
            textAlign = TextAlign.End,
        )
    }
}

@Immutable
private data class Fact(val label: String, val value: String, val color: Color)

/** Eight facts in two rows of four; Track and Air sit where Laps and Time left were in the design. */
@Composable
private fun FactsGrid(conditions: SessionConditions, lap: Int, modifier: Modifier) {
    val c = LocalDashPalette.current
    val scheme = MaterialTheme.colorScheme
    val remaining = if (conditions.totalLaps > 0 && lap > 0) maxOf(conditions.totalLaps - lap + 1, 0).toString() else "—"
    val facts = listOf(
        Fact("Track", temperature(conditions.trackTemp), c.tyreBands[3]),
        Fact("Remaining", remaining, scheme.onSurface),
        Fact(
            "Pit window",
            if (conditions.pitIdealLap > 0) "L${conditions.pitIdealLap}–${conditions.pitLatestLap}" else "—",
            c.caution,
        ),
        Fact("Rejoin", if (conditions.rejoinPosition > 0) "P${conditions.rejoinPosition}" else "—", c.gain),
        Fact("Pit speed", conditions.pitSpeedLimit?.let { "$it km/h" } ?: "—", c.cold),
        Fact(
            "Length",
            if (conditions.trackLengthM > 0) String.format(Locale.ROOT, "%.3f km", conditions.trackLengthM / 1000.0) else "—",
            scheme.onSurface,
        ),
        Fact("Local", conditions.timeOfDayMinutes?.let(::formatTimeOfDay) ?: "—", scheme.onSurface),
        Fact("Air", temperature(conditions.airTemp), c.gain),
    )
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        facts.chunked(4).forEach { row ->
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                row.forEach { fact ->
                    Column(Modifier.weight(1f).semantics(mergeDescendants = true) {}) {
                        Text(fact.label.uppercase(Locale.ROOT), style = Caption.copy(letterSpacing = 0.6.sp), color = scheme.onSurfaceVariant, maxLines = 1)
                        Text(
                            fact.value,
                            Modifier.padding(top = 3.dp),
                            style = TextStyle(fontSize = 15.sp, fontWeight = FontWeight.Medium),
                            color = if (fact.value == "—") scheme.onSurfaceVariant else fact.color,
                            maxLines = 1,
                        )
                    }
                }
            }
        }
    }
}

// ── Race ────────────────────────────────────────────────────────────────────

@Composable
private fun TrackStatus(conditions: SessionConditions, modifier: Modifier) {
    val c = LocalDashPalette.current
    val scheme = MaterialTheme.colorScheme
    val (status, tone) = when (conditions.safetyCarStatus) {
        1 -> "Safety Car" to c.caution
        2 -> "Virtual Safety Car" to c.caution
        3 -> "Formation Lap" to c.cold
        else -> "Green" to c.gain
    }
    Row(
        modifier.fillMaxWidth().height(76.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Icon(painterResource(R.drawable.ic_event_flag), null, Modifier.size(30.dp), tint = tone)
        Column(Modifier.weight(1f)) {
            Text("TRACK STATUS", style = Caption, color = scheme.onSurfaceVariant)
            Text(status, Modifier.padding(top = 2.dp), style = TextStyle(fontSize = 22.sp, fontWeight = FontWeight.Light), color = tone, maxLines = 1)
        }
        PeriodCount("SC", conditions.safetyCarPeriods, c.caution)
        PeriodCount("VSC", conditions.virtualSafetyCarPeriods, c.caution)
        PeriodCount("Red", conditions.redFlagPeriods, c.loss)
    }
}

/** How many periods of one kind the session has had; coloured only once there has been one. */
@Composable
private fun PeriodCount(label: String, count: Int, tone: Color) {
    val scheme = MaterialTheme.colorScheme
    Column(Modifier.width(36.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        Text(label.uppercase(Locale.ROOT), style = Caption, color = scheme.onSurfaceVariant)
        Text(
            count.toString(),
            Modifier.padding(top = 2.dp),
            style = TextStyle(fontSize = 18.sp, fontWeight = FontWeight.Light),
            color = if (count > 0) tone else scheme.onSurface,
        )
    }
}

private fun zoneColor(flag: Int, c: DashPalette, idle: Color): Color = when (flag) {
    1 -> c.gain
    2 -> c.cold
    3 -> c.caution
    else -> idle
}

/** One thin line split at every zone boundary, each zone at its real length on the lap. */
@Composable
private fun MarshalZones(zones: List<MarshalZoneInfo>, modifier: Modifier) {
    val c = LocalDashPalette.current
    val scheme = MaterialTheme.colorScheme
    val yellow = zones.withIndex().filter { it.value.flag == 3 }
    val blue = zones.withIndex().filter { it.value.flag == 2 }
    val (summary, tone) = when {
        yellow.isNotEmpty() -> zoneSummary("Yellow", yellow.map { it.index + 1 }) to c.caution
        blue.isNotEmpty() -> zoneSummary("Blue", blue.map { it.index + 1 }) to c.cold
        zones.isEmpty() -> "No zone data" to scheme.onSurfaceVariant
        else -> "Clear" to scheme.onSurfaceVariant
    }
    Column(modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth().height(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("MARSHAL ZONES", Modifier.weight(1f), style = Caption, color = scheme.onSurfaceVariant)
            if (yellow.isNotEmpty() || blue.isNotEmpty()) {
                Box(Modifier.padding(end = 6.dp).size(6.dp).background(tone, RoundedCornerShape(3.dp)))
            }
            Text(summary, style = MaterialTheme.typography.labelMedium, color = tone)
        }
        if (zones.isNotEmpty()) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(top = 8.dp)
                    .height(4.dp)
                    .semantics { contentDescription = "Marshal zones: $summary" },
                horizontalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                zones.forEachIndexed { index, zone ->
                    val end = zones.getOrNull(index + 1)?.start ?: 1.0
                    val length = (end - zone.start).toFloat()
                    if (length > 0f) {
                        Box(
                            Modifier
                                .weight(length)
                                .fillMaxHeight()
                                .background(zoneColor(zone.flag, c, scheme.outlineVariant), RoundedCornerShape(1.dp)),
                        )
                    }
                }
            }
        }
    }
}

private fun zoneSummary(flag: String, numbers: List<Int>): String =
    if (numbers.size == 1) "$flag · Z${numbers[0]}" else "$flag · Z${numbers[0]} +${numbers.size - 1}"

@Immutable
private data class EventLine(val key: String, val text: String, val kind: RaceEventKind, val code: String, val time: String)

@Immutable
private data class LapGroup(val lap: Int, val events: List<EventLine>)

/** The newest lap first, each lap's events newest first, as the Race 6 design lists them. */
@Composable
private fun EventsByLap(
    events: List<RaceEventEntry>,
    labels: Map<String, String>,
    drivers: Map<Int, TimingDriver>,
    modifier: Modifier,
    horizontalPadding: Dp,
) {
    val scheme = MaterialTheme.colorScheme
    val groups = remember(events, labels, drivers) {
        val name = { index: Int? -> driverName(drivers, index) }
        val lines = events.asReversed().mapNotNull { event ->
            describeRaceEvent(event, labels, name)?.let { text ->
                event.lap to EventLine(event.identity, text.text, text.kind, event.code, formatSessionClock(event.sessionTime))
            }
        }
        val byLap = LinkedHashMap<Int, MutableList<EventLine>>()
        for ((lap, line) in lines) byLap.getOrPut(lap) { mutableListOf() }.add(line)
        byLap.map { (lap, list) -> LapGroup(lap, list) }
    }
    Column(modifier) {
        Row(Modifier.fillMaxWidth().height(30.dp).padding(horizontal = horizontalPadding), verticalAlignment = Alignment.CenterVertically) {
            Text("EVENTS", style = Caption, color = scheme.onSurfaceVariant)
        }
        if (groups.isEmpty()) {
            Text(
                "No race events yet",
                Modifier.padding(horizontal = horizontalPadding, vertical = 8.dp),
                style = MaterialTheme.typography.bodyMedium,
                color = scheme.onSurfaceVariant,
            )
            return@Column
        }
        LazyColumn(Modifier.fillMaxWidth().weight(1f)) {
            groups.forEach { group ->
                item(key = "lap-${group.lap}", contentType = "lap") {
                    LapHeader(group, horizontalPadding)
                }
                items(group.events, key = { it.key }, contentType = { "event" }) { line ->
                    EventRow(line, horizontalPadding)
                }
            }
        }
    }
}

@Composable
private fun LapHeader(group: LapGroup, horizontalPadding: Dp) {
    val scheme = MaterialTheme.colorScheme
    Row(
        Modifier.fillMaxWidth().height(26.dp).background(scheme.surfaceContainer).padding(horizontal = horizontalPadding),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            if (group.lap > 0) "Lap ${group.lap}" else "Before the start",
            Modifier.weight(1f),
            style = MaterialTheme.typography.labelMedium,
            color = scheme.onSurfaceVariant,
        )
        Text(
            if (group.events.size == 1) "1 event" else "${group.events.size} events",
            style = MaterialTheme.typography.labelSmall.copy(fontWeight = FontWeight.Normal),
            color = scheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun EventRow(line: EventLine, horizontalPadding: Dp) {
    val c = LocalDashPalette.current
    val scheme = MaterialTheme.colorScheme
    val tone = eventColor(line, c, scheme.onSurfaceVariant, scheme.onSurface)
    Column {
        Row(
            Modifier.fillMaxWidth().height(34.dp).padding(horizontal = horizontalPadding),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(painterResource(eventIcon(line.kind)), null, Modifier.size(18.dp), tint = tone)
            Spacer(Modifier.width(10.dp))
            Text(
                line.text,
                Modifier.weight(1f),
                style = MaterialTheme.typography.bodyMedium.copy(fontWeight = FontWeight.Medium),
                color = tone,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(line.time, Modifier.padding(start = 8.dp), style = MaterialTheme.typography.labelSmall.copy(fontWeight = FontWeight.Normal), color = scheme.onSurfaceVariant)
        }
        HorizontalDivider(color = scheme.surfaceContainer)
    }
}

@DrawableRes
private fun eventIcon(kind: RaceEventKind): Int = when (kind) {
    RaceEventKind.LAP -> R.drawable.ic_event_lap
    RaceEventKind.OVERTAKE -> R.drawable.ic_event_overtake
    RaceEventKind.PENALTY -> R.drawable.ic_event_penalty
    RaceEventKind.SERVED -> R.drawable.ic_event_served
    RaceEventKind.RETIRED -> R.drawable.ic_event_retired
    RaceEventKind.FLAG, RaceEventKind.START, RaceEventKind.INFO -> R.drawable.ic_event_flag
}

/** The desktop event colours, in the dashboard's light and dark tones. */
private fun eventColor(line: EventLine, c: DashPalette, quiet: Color, plain: Color): Color = when (line.code) {
    "FTLP" -> c.best
    "RCWN" -> c.caution
    "DRSE", "LGOT" -> c.gain
    "RDFL" -> c.loss
    "SCAR" -> c.caution
    "SSTA", "SEND" -> c.cold
    "CHQF" -> plain
    "PENA" -> when (line.kind) {
        RaceEventKind.RETIRED -> quiet
        else -> c.tyreBands[3]
    }
    else -> quiet
}

/** A roster name's last word in capitals, as the desktop's event list shows drivers. */
private fun driverName(drivers: Map<Int, TimingDriver>, index: Int?): String {
    if (index == null) return "Car ?"
    val name = drivers[index]?.name?.trim()?.takeIf { it.isNotEmpty() } ?: return "Car $index"
    return name.split(Regex("\\s+")).last().uppercase(Locale.ROOT)
}
