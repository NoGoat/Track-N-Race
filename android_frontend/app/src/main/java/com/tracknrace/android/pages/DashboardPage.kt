package com.tracknrace.android.pages

import android.content.res.Configuration
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.TextAutoSize
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.tracknrace.android.DashboardColdState
import com.tracknrace.android.DashboardLapComparisonState
import com.tracknrace.android.HotTelemetry
import com.tracknrace.android.TelemetryStore
import com.tracknrace.android.TimingTowerState
import kotlin.math.abs
import kotlinx.coroutines.delay

// ── Design ──────────────────────────────────────────────────────────────────
// A steering-wheel display, not an app page. It deliberately ignores the
// Material theme: black glass, white numerals, and colour only where it means
// something (gaining, losing, too hot, running short). No cards, no labels on
// anything whose meaning is obvious from where it sits.
//
// Only what a driver acts on during a lap is on screen:
//
//   shift lights
//   position · lap · fitted tyre          (an alert replaces this line)
//   delta to the fastest lap, gear, speed
//   ERS charge
//   interval to the cars ahead and behind (races only)
//   tyre surface temperatures, fuel margin, last lap
//
// Everything else — sectors, pedals, wear, ERS mode — is on the
// Timing and Tyres pages or left out. A completed lap, a new best, an invalid
// lap, a brake-bias change and a fuel shortfall show as a 3 s alert instead.
//
// Each hot field is read inside the smallest composable that shows it, so a
// 60 Hz telemetry tick recomposes that leaf and never the layout.

/** Race, Race 2 and Race 3 in F1 24, F1 25 and the 2026 Season Pack. */
private val RaceSessionTypes = setOf(15, 16, 17)

/** Interval under which a neighbour is within attack range. */
private const val CloseIntervalMs = 1_000

private const val AlertDurationMs = 3_000L

private const val StatusSettleMs = 2_000L

private val Background = Color(0xff000000)
private val Primary = Color(0xffffffff)
private val Secondary = Color(0xff7d8590)
private val Faint = Color(0xff1b1f24)
private val Gain = Color(0xff2ee67a)
private val Loss = Color(0xffff3b4e)
private val Best = Color(0xffb36bff)
private val Warn = Color(0xffffb020)
private val Info = Color(0xff3d9bff)

// The app's own face (TrackNRaceTheme): system sans-serif, tabular figures so
// changing digits do not shift the numbers around them.
private val NumeralStyle = TextStyle(
    fontFamily = FontFamily.SansSerif,
    fontWeight = FontWeight.Bold,
    fontFeatureSettings = "tnum",
    lineHeight = TextUnit.Unspecified,
)

@Composable
internal fun DashboardScreen(
    store: TelemetryStore,
    active: Boolean = true,
) {
    val frame = rememberDashboardFrameState(store, active)
    val alert = rememberDashboardAlert(frame)
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val race by remember(store) {
        derivedStateOf { store.cold.sessionType in RaceSessionTypes }
    }
    // The Scaffold behind this page paints it black (TrackNRaceApp).
    Box(Modifier.fillMaxSize()) {
        if (landscape) {
            LandscapeDashboard(store, frame, alert, race)
        } else {
            PortraitDashboard(store, frame, alert, race)
        }
    }
}

@Composable
private fun PortraitDashboard(
    store: TelemetryStore,
    frame: DashboardFrameState,
    alert: State<DashboardAlert?>,
    race: Boolean,
) {
    Column(
        Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        ShiftLights(frame, Modifier.fillMaxWidth().height(12.dp).graphicsLayer())
        Spacer(Modifier.height(10.dp))
        StatusLine(frame, alert, Modifier.fillMaxWidth().height(34.dp).graphicsLayer())
        GearCluster(frame, Modifier.fillMaxWidth().weight(2.6f))
        Ers(frame, Modifier.fillMaxWidth().weight(1.1f).graphicsLayer())
        if (race) Gaps(store, Modifier.fillMaxWidth().weight(0.9f).graphicsLayer())
        Spacer(Modifier.height(12.dp))
        Row(
            Modifier.fillMaxWidth().weight(1.35f),
            horizontalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            Tyres(frame, Modifier.weight(1f).fillMaxHeight())
            FuelAndLastLap(frame, Modifier.weight(1f).fillMaxHeight().graphicsLayer())
        }
    }
}

@Composable
private fun LandscapeDashboard(
    store: TelemetryStore,
    frame: DashboardFrameState,
    alert: State<DashboardAlert?>,
    race: Boolean,
) {
    Column(Modifier.fillMaxSize().padding(horizontal = 24.dp, vertical = 12.dp)) {
        ShiftLights(frame, Modifier.fillMaxWidth().height(12.dp).graphicsLayer())
        Spacer(Modifier.height(10.dp))
        Row(Modifier.fillMaxWidth().weight(1f), horizontalArrangement = Arrangement.spacedBy(24.dp)) {
            Column(Modifier.weight(1f).fillMaxHeight()) {
                Ers(frame, Modifier.fillMaxWidth().weight(1.4f).graphicsLayer())
                if (race) Gaps(store, Modifier.fillMaxWidth().weight(1f).graphicsLayer())
            }
            Column(
                Modifier.weight(1.1f).fillMaxHeight(),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                StatusLine(frame, alert, Modifier.fillMaxWidth().height(34.dp).graphicsLayer())
                GearCluster(frame, Modifier.fillMaxWidth().weight(1f))
            }
            Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                Tyres(frame, Modifier.fillMaxWidth().weight(1.3f))
                FuelAndLastLap(frame, Modifier.fillMaxWidth().weight(1f).graphicsLayer())
            }
        }
    }
}

/**
 * The gear digit is too large for the glyph atlas, so without a texture of its
 * own it is filled as a path on every frame. It changes rarely, so render it
 * into a cached texture. Not worth it for blocks that change several times a
 * second: each change re-renders the texture in an extra render pass, and an
 * A/B run on a Galaxy M12 showed no gain from caching the other blocks.
 */
private fun Modifier.cachedLayer(): Modifier = graphicsLayer(compositingStrategy = CompositingStrategy.Offscreen)

// ── Frame sampling ──────────────────────────────────────────────────────────

/**
 * Stable holder whose fields are independent Compose states. The dashboard
 * passes this holder without reading it, so a telemetry tick invalidates only
 * the leaf composables that actually consume a changed field.
 */
@Stable
private class DashboardFrameState(initialHot: HotTelemetry) {
    var speedKph by mutableIntStateOf(initialHot.speedKph)
        private set
    var gear by mutableIntStateOf(initialHot.gear)
        private set
    var revLightsBitValue by mutableStateOf(initialHot.revLightsBitValue)
        private set
    var tyreFl by mutableIntStateOf(initialHot.tyreSurfaceFl)
        private set
    var tyreFr by mutableIntStateOf(initialHot.tyreSurfaceFr)
        private set
    var tyreRl by mutableIntStateOf(initialHot.tyreSurfaceRl)
        private set
    var tyreRr by mutableIntStateOf(initialHot.tyreSurfaceRr)
        private set

    var cold by mutableStateOf(DashboardColdState())
        private set
    var lapComparison by mutableStateOf(DashboardLapComparisonState())
        private set

    // Cold rows arrive several times a second, mostly carrying the running lap
    // time. Each readout reads only its own fields, which change far less often,
    // so an unrelated cold update recomposes nothing on screen.
    var position by mutableIntStateOf(0)
        private set
    var lapText by mutableStateOf("L–")
        private set
    var tyreAgeText by mutableStateOf("–")
        private set
    var tyreCompound by mutableIntStateOf(0)
        private set
    var statusAvailable by mutableStateOf(false)
        private set
    var ersPercent by mutableIntStateOf(0)
        private set
    var fuelLaps by mutableDoubleStateOf(0.0)
        private set
    var lastLapMs by mutableIntStateOf(0)
        private set
    var fastestLapMs by mutableIntStateOf(0)
        private set

    /** Delta to the fastest lap, rounded to the displayed millisecond. */
    var lapDeltaSeconds by mutableStateOf<Double?>(null)
        private set

    private var lastHot = initialHot
    private var lastCold = cold
    private var lastLapComparison = lapComparison

    fun update(
        hot: HotTelemetry,
        latestCold: DashboardColdState,
        latestLapComparison: DashboardLapComparisonState,
    ) {
        if (hot !== lastHot) {
            speedKph = hot.speedKph
            gear = hot.gear
            revLightsBitValue = hot.revLightsBitValue
            tyreFl = hot.tyreSurfaceFl
            tyreFr = hot.tyreSurfaceFr
            tyreRl = hot.tyreSurfaceRl
            tyreRr = hot.tyreSurfaceRr
            lastHot = hot
        }
        if (latestCold !== lastCold) {
            cold = latestCold
            lastCold = latestCold
            position = latestCold.position
            lapText = lapLabel(latestCold)
            tyreAgeText = tyreAgeLabel(latestCold)
            tyreCompound = latestCold.tyreCompound
            statusAvailable = latestCold.statusAvailable
            ersPercent = latestCold.ersPercent
            fuelLaps = latestCold.fuelLaps
            lastLapMs = latestCold.lastLapMs
        }
        if (latestLapComparison !== lastLapComparison) {
            lapComparison = latestLapComparison
            lastLapComparison = latestLapComparison
            fastestLapMs = latestLapComparison.fastestLapMs
            lapDeltaSeconds = latestLapComparison.lapDeltaSeconds
                ?.takeIf { it.isFinite() }
                ?.let { kotlin.math.round(it * 1000) / 1000 }
        }
    }
}

@Composable
private fun rememberDashboardFrameState(store: TelemetryStore, active: Boolean): DashboardFrameState {
    val displayed = remember(store) {
        DashboardFrameState(store.latestHot())
    }
    LaunchedEffect(store, active) {
        if (!active) return@LaunchedEffect
        while (true) {
            withFrameNanos { }
            displayed.update(
                store.latestHot(),
                store.cold,
                store.latestLapComparison(),
            )
        }
    }
    return displayed
}

// ── Alerts ──────────────────────────────────────────────────────────────────

@Immutable
private data class DashboardAlert(val id: Long, val text: String, val color: Color)

/**
 * Watches the sampled cold state for the changes worth interrupting for and
 * holds the newest one for [AlertDurationMs]. Collected through snapshotFlow,
 * so a cold update recomposes nothing unless it raises an alert.
 */
@Composable
private fun rememberDashboardAlert(frame: DashboardFrameState): State<DashboardAlert?> {
    val alert = remember(frame) { mutableStateOf<DashboardAlert?>(null) }
    LaunchedEffect(frame) {
        var previousCold: DashboardColdState? = null
        var previousBest = 0
        var nextId = 0L
        // A connect or driver switch fills the status fields one patch at a
        // time; none of those first values is a change the driver made.
        var statusSince = 0L
        snapshotFlow { frame.cold to frame.lapComparison.fastestLapMs }.collect { (cold, best) ->
            val now = System.currentTimeMillis()
            if (!cold.statusAvailable) statusSince = 0L
            else if (statusSince == 0L) statusSince = now
            val statusSettled = statusSince > 0L && now - statusSince > StatusSettleMs
            val raised = previousCold?.let {
                alertFor(it, cold, previousBest, best, statusSettled)
            }
            previousCold = cold
            previousBest = best
            if (raised != null) alert.value = DashboardAlert(++nextId, raised.first, raised.second)
        }
    }
    val current = alert.value
    LaunchedEffect(current?.id) {
        if (current == null) return@LaunchedEffect
        delay(AlertDurationMs)
        alert.value = null
    }
    return alert
}

private fun alertFor(
    previous: DashboardColdState,
    cold: DashboardColdState,
    previousBestMs: Int,
    bestMs: Int,
    statusSettled: Boolean,
): Pair<String, Color>? {
    val statusSteady = statusSettled && previous.statusAvailable
    return when {
        cold.lapNumber > 0 && previous.lapNumber == cold.lapNumber &&
            !previous.lapInvalid && cold.lapInvalid -> "LAP INVALID" to Loss
        previousBestMs > 0 && bestMs in 1 until previousBestMs ->
            "BEST LAP  ${formatTime(bestMs)}" to Best
        cold.lapNumber > previous.lapNumber && previous.lapNumber > 0 && cold.lastLapMs > 0 ->
            "LAP  ${formatTime(cold.lastLapMs)}" to Primary
        statusSteady && previous.brakeBias != cold.brakeBias ->
            "BRAKE BIAS  ${cold.brakeBias}%" to Info
        statusSteady && previous.fuelLaps >= 0.0 && cold.fuelLaps < 0.0 ->
            "FUEL SHORT" to Loss
        else -> null
    }
}

// ── Shift lights ────────────────────────────────────────────────────────────

@Composable
private fun ShiftLights(frame: DashboardFrameState, modifier: Modifier) {
    val bitValue = frame.revLightsBitValue
    Row(
        modifier.semantics { contentDescription = "Shift lights" },
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        repeat(15) { index ->
            val lit = bitValue != null && bitValue and (1 shl index) != 0
            val tone = when {
                index < 5 -> Gain
                index < 10 -> Loss
                else -> Info
            }
            Box(
                Modifier.weight(1f).fillMaxHeight()
                    .background(if (lit) tone else Faint, RoundedCornerShape(2.dp)),
            )
        }
    }
}

// ── Status line ─────────────────────────────────────────────────────────────

/** Position, lap and fitted tyre; an alert takes the whole line while shown. */
@Composable
private fun StatusLine(
    frame: DashboardFrameState,
    alert: State<DashboardAlert?>,
    modifier: Modifier,
) {
    val shown = alert.value
    if (shown != null) {
        Box(
            modifier.clip(RoundedCornerShape(6.dp)).background(shown.color.copy(alpha = 0.18f)),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                shown.text,
                style = NumeralStyle,
                fontSize = 20.sp,
                letterSpacing = 1.5.sp,
                color = shown.color,
                maxLines = 1,
            )
        }
        return
    }
    val position = frame.position
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Text(
            if (position > 0) "P$position" else "P–",
            style = NumeralStyle,
            fontSize = 26.sp,
            color = if (position > 0) Primary else Secondary,
            modifier = Modifier.weight(1f),
            maxLines = 1,
        )
        Text(
            frame.lapText,
            style = NumeralStyle,
            fontSize = 20.sp,
            color = Secondary,
            textAlign = TextAlign.Center,
            modifier = Modifier.weight(1f),
            maxLines = 1,
        )
        Row(
            Modifier.weight(1f),
            horizontalArrangement = Arrangement.End,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val compound = compoundColor(frame.tyreCompound)
            if (frame.statusAvailable && compound != null) {
                Box(Modifier.size(12.dp).background(compound, CircleShape))
                Spacer(Modifier.width(8.dp))
            }
            Text(
                frame.tyreAgeText,
                style = NumeralStyle,
                fontSize = 20.sp,
                color = Secondary,
                maxLines = 1,
            )
        }
    }
}

// ── Gear cluster ────────────────────────────────────────────────────────────

/**
 * Delta, gear and speed stacked on the centre line: the gear fills whatever
 * height is left, and the smaller numbers either side of it need no labels.
 */
@Composable
private fun GearCluster(frame: DashboardFrameState, modifier: Modifier) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        Spacer(Modifier.height(16.dp))
        Delta(frame, Modifier.graphicsLayer())
        Gear(frame, Modifier.fillMaxWidth().weight(1f).cachedLayer())
        Speed(frame, Modifier.graphicsLayer())
    }
}

/** The largest glyph, sized to whatever height the layout leaves it. */
@Composable
private fun Gear(frame: DashboardFrameState, modifier: Modifier) {
    Box(modifier, contentAlignment = Alignment.Center) {
        Text(
            gearLabel(frame.gear),
            style = NumeralStyle,
            fontWeight = FontWeight.Black,
            color = Primary,
            autoSize = TextAutoSize.StepBased(48.sp, 200.sp, 4.sp),
            textAlign = TextAlign.Center,
            maxLines = 1,
        )
    }
}

@Composable
private fun Speed(frame: DashboardFrameState, modifier: Modifier = Modifier) {
    Row(modifier, verticalAlignment = Alignment.Bottom) {
        Text(
            frame.speedKph.toString(),
            style = NumeralStyle,
            fontSize = 34.sp,
            color = Primary,
            maxLines = 1,
        )
        Text(
            " km/h",
            style = NumeralStyle,
            fontSize = 14.sp,
            color = Secondary,
            modifier = Modifier.padding(bottom = 5.dp),
            maxLines = 1,
        )
    }
}

/** Time against the fastest lap at this point of the track. */
@Composable
private fun Delta(frame: DashboardFrameState, modifier: Modifier = Modifier) {
    val delta = frame.lapDeltaSeconds
    val known = delta != null
    Text(
        if (known) formatDelta(delta) else "–",
        modifier = modifier,
        style = NumeralStyle,
        fontSize = 26.sp,
        color = if (known) deltaColor(delta) else Secondary,
        maxLines = 1,
    )
}

// ── ERS ─────────────────────────────────────────────────────────────────────

/**
 * Battery charge: the one resource a driver manages every straight. The bar
 * turns amber under 20%, when a deployment will soon run dry.
 */
@Composable
private fun Ers(frame: DashboardFrameState, modifier: Modifier) {
    val available = frame.statusAvailable
    val ersPercent = frame.ersPercent
    val tone = if (ersPercent < 20) Warn else Info
    Column(
        modifier,
        verticalArrangement = Arrangement.Center,
    ) {
        Row(verticalAlignment = Alignment.Bottom) {
            SmallLabel("ERS", Modifier.weight(1f).padding(bottom = 8.dp))
            Text(
                if (available) "$ersPercent%" else "–",
                style = NumeralStyle,
                fontSize = 48.sp,
                color = if (available) tone else Secondary,
                maxLines = 1,
            )
        }
        Spacer(Modifier.height(6.dp))
        Box(Modifier.fillMaxWidth().height(10.dp).clip(CircleShape).background(Faint)) {
            if (available) {
                Box(
                    Modifier.fillMaxHeight()
                        .fillMaxWidth((ersPercent / 100f).coerceIn(0f, 1f))
                        .background(tone),
                )
            }
        }
    }
}

// ── Race gaps ───────────────────────────────────────────────────────────────

@Immutable
private data class RaceGaps(
    val aheadName: String?,
    val aheadMs: Int?,
    val behindName: String?,
    val behindMs: Int?,
    val leading: Boolean,
    val last: Boolean,
)

/**
 * Intervals to the cars directly ahead and behind, each under the neighbour's
 * name. Within attack range the interval turns green (ahead) or red (behind).
 */
@Composable
private fun Gaps(store: TelemetryStore, modifier: Modifier) {
    val gaps by remember(store) {
        derivedStateOf { raceGaps(store.timing, store.selectedDriverIndex) }
    }
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        GapReadout(
            name = if (gaps.leading) "LEADING" else gaps.aheadName,
            interval = if (gaps.leading) null else gaps.aheadMs,
            blank = gaps.leading,
            sign = "−",
            closeColor = Gain,
            alignment = Alignment.Start,
            modifier = Modifier.weight(1f),
        )
        GapReadout(
            name = if (gaps.last) "LAST" else gaps.behindName,
            interval = if (gaps.last) null else gaps.behindMs,
            blank = gaps.last,
            sign = "+",
            closeColor = Loss,
            alignment = Alignment.End,
            modifier = Modifier.weight(1f),
        )
    }
}

@Composable
private fun GapReadout(
    name: String?,
    interval: Int?,
    sign: String,
    closeColor: Color,
    alignment: Alignment.Horizontal,
    modifier: Modifier,
    blank: Boolean = false,
) {
    val close = interval != null && interval in 1 until CloseIntervalMs
    Column(modifier, horizontalAlignment = alignment) {
        Text(
            name?.uppercase() ?: "–",
            style = NumeralStyle,
            fontSize = 14.sp,
            letterSpacing = 1.sp,
            color = Secondary,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        Text(
            if (blank) "" else formatInterval(interval, sign),
            style = NumeralStyle,
            fontSize = 30.sp,
            color = when {
                interval == null -> Faint
                close -> closeColor
                else -> Primary
            },
            maxLines = 1,
        )
    }
}

private fun raceGaps(timing: TimingTowerState, driverIndex: Int): RaceGaps {
    val cars = timing.cars
    val me = cars.firstOrNull { it.index == driverIndex && it.position > 0 }
        ?: return RaceGaps(null, null, null, null, leading = false, last = false)
    val ahead = cars.firstOrNull { it.position == me.position - 1 }
    val behind = cars.firstOrNull { it.position == me.position + 1 }
    fun name(index: Int?) = index?.let { timing.drivers[it]?.name }
    // The timing row carries each car's gap to the leader, which is zero for
    // the leader and for anyone whose gap is not known yet.
    fun gapKnown(position: Int, gapMs: Int) = position == 1 || gapMs > 0
    val meKnown = gapKnown(me.position, me.gapMs)
    val aheadMs = if (ahead != null && meKnown && gapKnown(ahead.position, ahead.gapMs)) {
        (me.gapMs - ahead.gapMs).takeIf { it > 0 }
    } else {
        null
    }
    val behindMs = if (behind != null && meKnown && gapKnown(behind.position, behind.gapMs)) {
        (behind.gapMs - me.gapMs).takeIf { it > 0 }
    } else {
        null
    }
    return RaceGaps(
        aheadName = name(ahead?.index),
        aheadMs = aheadMs,
        behindName = name(behind?.index),
        behindMs = behindMs,
        leading = me.position == 1,
        last = behind == null,
    )
}

// ── Tyres ───────────────────────────────────────────────────────────────────

/**
 * Surface temperature of each corner, laid out as the corners sit on the car.
 * The block's colour is the reading: blue cold, green in the window, amber
 * and red over it.
 */
@Composable
private fun Tyres(frame: DashboardFrameState, modifier: Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(Modifier.fillMaxWidth().weight(1f), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TyreFl(frame, Modifier.weight(1f).fillMaxHeight().graphicsLayer())
            TyreFr(frame, Modifier.weight(1f).fillMaxHeight().graphicsLayer())
        }
        Row(Modifier.fillMaxWidth().weight(1f), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TyreRl(frame, Modifier.weight(1f).fillMaxHeight().graphicsLayer())
            TyreRr(frame, Modifier.weight(1f).fillMaxHeight().graphicsLayer())
        }
    }
}

@Composable
private fun TyreFl(frame: DashboardFrameState, modifier: Modifier) = TyreBlock(frame.tyreFl, modifier)

@Composable
private fun TyreFr(frame: DashboardFrameState, modifier: Modifier) = TyreBlock(frame.tyreFr, modifier)

@Composable
private fun TyreRl(frame: DashboardFrameState, modifier: Modifier) = TyreBlock(frame.tyreRl, modifier)

@Composable
private fun TyreRr(frame: DashboardFrameState, modifier: Modifier) = TyreBlock(frame.tyreRr, modifier)

@Composable
private fun TyreBlock(temperature: Int, modifier: Modifier) {
    val known = temperature > 0
    val tone = tyreTemperatureColor(temperature)
    Box(
        modifier.clip(RoundedCornerShape(8.dp))
            .background(if (known) tone.copy(alpha = 0.22f) else Faint),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            if (known) "$temperature°" else "–",
            style = NumeralStyle,
            fontSize = 24.sp,
            color = if (known) tone else Secondary,
            maxLines = 1,
        )
    }
}

// ── Fuel and last lap ───────────────────────────────────────────────────────

@Composable
private fun FuelAndLastLap(frame: DashboardFrameState, modifier: Modifier) {
    val available = frame.statusAvailable
    val fuelLaps = frame.fuelLaps
    val lastLapMs = frame.lastLapMs
    val best = frame.fastestLapMs
    Column(modifier, verticalArrangement = Arrangement.SpaceEvenly) {
        Column {
            SmallLabel("FUEL")
            Row(verticalAlignment = Alignment.Bottom) {
                Text(
                    if (available) "%+.1f".format(fuelLaps) else "–",
                    style = NumeralStyle,
                    fontSize = 30.sp,
                    color = if (available) fuelMarginColor(fuelLaps) else Secondary,
                    maxLines = 1,
                )
                if (available) {
                    Text(
                        " laps",
                        style = NumeralStyle,
                        fontSize = 14.sp,
                        color = Secondary,
                        modifier = Modifier.padding(bottom = 5.dp),
                        maxLines = 1,
                    )
                }
            }
        }
        Column {
            SmallLabel("LAST LAP")
            Text(
                formatTime(lastLapMs),
                style = NumeralStyle,
                fontSize = 30.sp,
                // Purple when the last lap is the fastest one, as on timing screens.
                color = when {
                    lastLapMs <= 0 -> Secondary
                    best > 0 && lastLapMs == best -> Best
                    else -> Primary
                },
                maxLines = 1,
            )
        }
    }
}

@Composable
private fun SmallLabel(text: String, modifier: Modifier = Modifier) {
    Text(
        text,
        modifier = modifier,
        style = NumeralStyle,
        fontSize = 14.sp,
        letterSpacing = 1.5.sp,
        color = Secondary,
        maxLines = 1,
    )
}

// ── Colour ──────────────────────────────────────────────────────────────────

private fun deltaColor(seconds: Double?): Color = when {
    seconds == null || !seconds.isFinite() || abs(seconds) < 0.0005 -> Primary
    seconds > 0 -> Loss
    else -> Gain
}

private fun tyreTemperatureColor(value: Int): Color = when {
    value <= 0 -> Secondary
    value < 80 -> Info
    value <= 105 -> Gain
    value <= 120 -> Warn
    else -> Loss
}

private fun fuelMarginColor(fuelLaps: Double): Color = when {
    fuelLaps > 1.0 -> Primary
    fuelLaps >= 0.0 -> Warn
    else -> Loss
}

private fun compoundColor(compound: Int): Color? = when (compound) {
    16 -> Loss
    17 -> Color(0xffffd23f)
    18 -> Primary
    7 -> Gain
    8 -> Info
    else -> null
}

// ── Formatting ──────────────────────────────────────────────────────────────

private fun lapLabel(cold: DashboardColdState): String = when {
    cold.lapNumber <= 0 -> "L–"
    cold.totalLaps > 0 -> "L${cold.lapNumber}/${cold.totalLaps}"
    else -> "L${cold.lapNumber}"
}

private fun tyreAgeLabel(cold: DashboardColdState): String = when {
    !cold.statusAvailable || compoundColor(cold.tyreCompound) == null -> "–"
    else -> "${cold.tyreAgeLaps}L"
}

private fun formatTime(milliseconds: Int): String {
    if (milliseconds <= 0) return "–"
    val minutes = milliseconds / 60_000
    val seconds = milliseconds / 1_000 % 60
    val millis = milliseconds % 1_000
    return "%d:%02d.%03d".format(minutes, seconds, millis)
}

private fun formatDelta(seconds: Double?): String {
    if (seconds == null || !seconds.isFinite()) return "–"
    val normalized = if (abs(seconds) < 0.0005) 0.0 else seconds
    return if (normalized > 0) "+%.3f".format(normalized) else "%.3f".format(normalized)
}

private fun formatInterval(milliseconds: Int?, sign: String): String {
    if (milliseconds == null || milliseconds <= 0) return "–"
    val seconds = milliseconds / 1_000.0
    return if (seconds < 60) {
        "$sign%.1f".format(seconds)
    } else {
        "$sign%d:%04.1f".format((seconds / 60).toInt(), seconds % 60)
    }
}

private fun gearLabel(gear: Int): String = when {
    gear < 0 -> "R"
    gear == 0 -> "N"
    else -> gear.toString()
}
