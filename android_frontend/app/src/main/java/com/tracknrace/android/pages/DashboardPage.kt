package com.tracknrace.android.pages

import android.content.res.Configuration
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
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
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.tracknrace.android.DashboardColdState
import com.tracknrace.android.DashboardLapComparisonState
import com.tracknrace.android.HotTelemetry
import com.tracknrace.android.TelemetryStore
import kotlin.math.abs
import kotlin.math.roundToInt

private val DashboardBackground = Color(0xff07090c)
private val DashboardCard = Color(0xff10151b)
private val DashboardDivider = Color(0xff26313b)
private val DashboardPrimary = Color.White
private val DashboardSecondary = Color(0xff8f9aa6)
private val DashboardFastest = Color(0xffb877db)

@Composable
internal fun DashboardScreen(
    store: TelemetryStore,
    active: Boolean = true,
) {
    val frame = rememberDashboardFrameState(store, active)
    val configuration = LocalConfiguration.current
    val landscape = configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
    val wideLandscape = landscape && configuration.screenWidthDp >= 600
    Surface(color = DashboardBackground, modifier = Modifier.fillMaxSize()) {
        Column(
            Modifier.fillMaxSize().padding(
                start = 16.dp,
                top = if (landscape) 8.dp else 16.dp,
                end = 16.dp,
                bottom = 16.dp,
            ),
        ) {
            if (landscape) {
                Box(Modifier.fillMaxWidth().height(30.dp).padding(bottom = 8.dp)) {
                    RpmLights(frame, Modifier.fillMaxSize())
                }
                HorizontalDashboardDivider()
                LandscapeDashboard(
                    frame,
                    wideLandscape,
                    Modifier.weight(1f),
                )
            } else {
                PortraitDashboard(frame, Modifier.fillMaxWidth().weight(1f))
            }
        }
    }
}

/**
 * Stable holder whose fields are independent Compose states. The dashboard
 * passes this holder without reading it, so a telemetry tick invalidates only
 * the leaf composables that actually consume a changed field.
 */
@Stable
private class DashboardFrameState(initialHot: HotTelemetry) {
    var speedKph by mutableIntStateOf(initialHot.speedKph)
        private set
    var rpm by mutableIntStateOf(initialHot.rpm)
        private set
    var gear by mutableIntStateOf(initialHot.gear)
        private set
    var throttle by mutableFloatStateOf(initialHot.throttle)
        private set
    var brake by mutableFloatStateOf(initialHot.brake)
        private set
    var revLightsBitValue by mutableStateOf(initialHot.revLightsBitValue)
        private set
    var tyreSurfaceFl by mutableIntStateOf(initialHot.tyreSurfaceFl)
        private set
    var tyreSurfaceFr by mutableIntStateOf(initialHot.tyreSurfaceFr)
        private set
    var tyreSurfaceRl by mutableIntStateOf(initialHot.tyreSurfaceRl)
        private set
    var tyreSurfaceRr by mutableIntStateOf(initialHot.tyreSurfaceRr)
        private set
    var tyreInnerFl by mutableIntStateOf(initialHot.tyreInnerFl)
        private set
    var tyreInnerFr by mutableIntStateOf(initialHot.tyreInnerFr)
        private set
    var tyreInnerRl by mutableIntStateOf(initialHot.tyreInnerRl)
        private set
    var tyreInnerRr by mutableIntStateOf(initialHot.tyreInnerRr)
        private set

    var cold by mutableStateOf(DashboardColdState())
        private set
    var lapComparison by mutableStateOf(DashboardLapComparisonState())
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
            rpm = hot.rpm
            gear = hot.gear
            throttle = hot.throttle
            brake = hot.brake
            revLightsBitValue = hot.revLightsBitValue
            tyreSurfaceFl = hot.tyreSurfaceFl
            tyreSurfaceFr = hot.tyreSurfaceFr
            tyreSurfaceRl = hot.tyreSurfaceRl
            tyreSurfaceRr = hot.tyreSurfaceRr
            tyreInnerFl = hot.tyreInnerFl
            tyreInnerFr = hot.tyreInnerFr
            tyreInnerRl = hot.tyreInnerRl
            tyreInnerRr = hot.tyreInnerRr
            lastHot = hot
        }
        if (latestCold !== lastCold) {
            cold = latestCold
            lastCold = latestCold
        }
        if (latestLapComparison !== lastLapComparison) {
            lapComparison = latestLapComparison
            lastLapComparison = latestLapComparison
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

@Composable
private fun RpmLights(frame: DashboardFrameState, modifier: Modifier = Modifier) {
    val bitValue = frame.revLightsBitValue
    val off = Color(0xff2a3540)
    val green = Color(0xff32d583)
    val red = Color(0xffff4d5e)
    val blue = Color(0xff43a5ff)
    Row(
        modifier.semantics { contentDescription = "RPM shift lights" },
        horizontalArrangement = Arrangement.SpaceEvenly,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        repeat(15) { index ->
            val lit = bitValue != null && bitValue and (1 shl index) != 0
            val active = when {
                index < 5 -> green
                index < 10 -> red
                else -> blue
            }
            Box(
                Modifier.size(14.dp).background(
                    color = if (lit) active else off,
                    shape = CircleShape,
                ),
            )
        }
    }
}

@Composable
private fun LandscapeDashboard(
    frame: DashboardFrameState,
    unframed: Boolean,
    modifier: Modifier,
) {
    Row(modifier.fillMaxWidth()) {
        Column(
            Modifier.weight(1f).fillMaxHeight()
                .background(if (unframed) Color.Transparent else DashboardCard),
        ) {
            LandscapeLapCell(frame, Modifier.weight(0.54f))
            HorizontalDashboardDivider()
            FastestLapComparisonCell(
                frame.lapComparison,
                Modifier.weight(0.96f).fillMaxWidth(),
            )
            HorizontalDashboardDivider()
            SideTyreBlock(
                frame = frame,
                leftSide = true,
                showDividers = true,
                modifier = Modifier.weight(0.86f),
            )
        }
        VerticalDashboardDivider()
        CenterReadout(
            frame,
            Modifier.weight(1.95f).fillMaxHeight()
                .background(if (unframed) Color.Transparent else DashboardCard),
        )
        VerticalDashboardDivider()
        Column(
            Modifier.weight(1f).fillMaxHeight()
                .background(if (unframed) Color.Transparent else DashboardCard),
        ) {
            LandscapeFuelCell(frame, Modifier.weight(0.54f))
            HorizontalDashboardDivider()
            EmptyDashboardCell(Modifier.weight(0.96f))
            HorizontalDashboardDivider()
            SideTyreBlock(
                frame = frame,
                leftSide = false,
                showDividers = true,
                modifier = Modifier.weight(0.86f),
            )
        }
    }
}

@Composable
private fun LandscapeLapCell(frame: DashboardFrameState, modifier: Modifier = Modifier) {
    ValueCell(lapLabel(frame.cold), DashboardPrimary, modifier)
}

@Composable
private fun LandscapeFuelCell(frame: DashboardFrameState, modifier: Modifier = Modifier) {
    FuelCell(frame.cold, modifier)
}

// ── Portrait ────────────────────────────────────────────────────────────────
// Portrait is read at a glance from a phone stand beside the wheel, so it is
// built as an instrument cluster rather than as a page of data.
//
// Two decisions carry the whole layout:
//
//  1. There is exactly one container. The hero holds the gear and the speed;
//     everything else sits flush on the backdrop and is grouped by space.
//     Giving each readout its own card — as this page used to — flattens the
//     hierarchy, because a container is the strongest signal on a dark screen
//     and nine of them all say "look here" at once.
//
//  2. The deltas are bars, not numbers. A signed millisecond figure has to be
//     read; a bar that grows left for green and right for red is recognised in
//     peripheral vision, which is all the attention a driver has to spare. The
//     numbers stay alongside for when there is time to read them.
//
// This is the Material 3 Expressive reading of the same two ideas: emphasis by
// shape and size contrast, grouping by space, colour used to mean something
// rather than to decorate. The expressive component set — MaterialShapes, the
// wavy indicators, the *Emphasized type styles — is absent or internal in
// material3 1.4.0, so the emphasis here is built from those primitives.
//
// Nothing animates. Both the delta and the tyre temperatures are republished
// at sample rate, so a spring on either would only add lag over a value that
// has already moved on.
//
// Each hot field is read inside the smallest composable that shows it, so a
// 60 Hz telemetry tick recomposes that leaf and never a zone or the column.

private val PortraitHeroSurface = Color(0xff161f2a)
private val PortraitBarTrack = Color(0xff141c25)
private val PortraitLightOff = Color(0xff111821)
private val PortraitCenterTick = Color(0xff4c5967)
private val PortraitPlaceholder = Color(0xff39434f)
private val PortraitTileSurface = Color(0xff10161e)
private val PortraitGreen = Color(0xff35d07f)
private val PortraitRed = Color(0xffff5566)
private val PortraitAmber = Color(0xffffb02e)
private val PortraitBlue = Color(0xff4c9fff)

private val HeroShape = RoundedCornerShape(40.dp)
private val TileShape = RoundedCornerShape(18.dp)
private val BarShape = RoundedCornerShape(50)
private val SegmentShape = RoundedCornerShape(3.dp)

/** Full-scale deflection of the lap delta bar, in seconds. */
private const val LapDeltaRangeSeconds = 1.5

@Composable
private fun PortraitDashboard(
    frame: DashboardFrameState,
    modifier: Modifier,
) {
    // Every zone is weighted, so the five of them always divide exactly the
    // height that exists. Mixing one weighted zone with intrinsic siblings
    // looks tidier but starves the weighted one the moment the siblings
    // outgrow the viewport, and here that zone is the hero.
    Column(modifier, verticalArrangement = Arrangement.spacedBy(14.dp)) {
        PortraitRevBand(frame, Modifier.fillMaxWidth().height(10.dp))
        PortraitHero(frame, Modifier.fillMaxWidth().weight(2.25f))
        PortraitDeltaBlock(frame, Modifier.fillMaxWidth().weight(0.85f))
        PortraitInputs(frame, Modifier.fillMaxWidth().weight(0.95f))
        PortraitDataRail(frame, Modifier.fillMaxWidth().weight(1.5f))
        PortraitTyreGrid(frame, Modifier.fillMaxWidth().weight(2f))
    }
}

// ── Rev band ────────────────────────────────────────────────────────────────

/**
 * The shift lights, spanning the full width above the hero rather than sitting
 * inside it as a detail. Unlit segments are pitched barely above the backdrop:
 * fifteen visible slots would be the loudest thing on the screen at the exact
 * moment the lights mean nothing.
 */
@Composable
private fun PortraitRevBand(frame: DashboardFrameState, modifier: Modifier) {
    val bitValue = frame.revLightsBitValue
    Row(
        modifier.semantics { contentDescription = "RPM shift lights" },
        horizontalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        repeat(15) { index ->
            val lit = bitValue != null && bitValue and (1 shl index) != 0
            val tone = when {
                index < 5 -> PortraitGreen
                index < 10 -> PortraitRed
                else -> PortraitBlue
            }
            Box(
                Modifier.weight(1f).fillMaxHeight()
                    .background(if (lit) tone else PortraitLightOff, SegmentShape),
            )
        }
    }
}

// ── Hero ────────────────────────────────────────────────────────────────────

/**
 * The only container on the page, and so the only thing that reads as "look
 * here first". It holds the two values a driver takes from a dashboard mid
 * corner — gear and speed — plus the race context that frames them, kept
 * deliberately small so it cannot compete.
 */
@Composable
private fun PortraitHero(frame: DashboardFrameState, modifier: Modifier) {
    Surface(color = PortraitHeroSurface, shape = HeroShape, modifier = modifier) {
        Column(
            Modifier.fillMaxSize().padding(horizontal = 22.dp, vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            PortraitContextStrip(frame)
            Row(
                Modifier.fillMaxWidth().weight(1f),
                horizontalArrangement = Arrangement.spacedBy(34.dp, Alignment.CenterHorizontally),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                PortraitGear(frame, Modifier.fillMaxHeight())
                Column(horizontalAlignment = Alignment.Start) {
                    PortraitSpeed(frame)
                    PortraitRpm(frame)
                }
            }
        }
    }
}

@Composable
private fun PortraitContextStrip(frame: DashboardFrameState) {
    val cold = frame.cold
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(14.dp)) {
        PortraitInline(
            "POS",
            if (cold.position > 0) "P${cold.position}" else "—",
            PortraitBlue,
            Modifier.weight(0.85f),
        )
        PortraitInline("LAP", lapLabel(cold), DashboardPrimary, Modifier.weight(1f))
        PortraitInline("TYRE", tyreLabel(cold), tyreTone(cold.tyreCompound), Modifier.weight(1.25f))
    }
}

/**
 * The single largest glyph on the page. It auto-sizes into whatever height the
 * hero was given rather than to a fixed point size, so a short phone shrinks
 * the gear instead of clipping its descender.
 */
@Composable
private fun PortraitGear(frame: DashboardFrameState, modifier: Modifier) {
    Text(
        gearLabel(frame.gear),
        modifier = modifier,
        color = DashboardPrimary,
        autoSize = TextAutoSize.StepBased(44.sp, 128.sp, 2.sp),
        fontFamily = FontFamily.Monospace,
        fontWeight = FontWeight.Black,
        maxLines = 1,
    )
}

@Composable
private fun PortraitSpeed(frame: DashboardFrameState) {
    Row(verticalAlignment = Alignment.Bottom) {
        Text(
            frame.speedKph.toString(),
            color = DashboardPrimary,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.Bold,
            fontSize = 48.sp,
            lineHeight = 48.sp,
            maxLines = 1,
        )
        Text(
            " KM/H",
            color = DashboardSecondary,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold,
            letterSpacing = 1.sp,
            maxLines = 1,
            modifier = Modifier.padding(bottom = 8.dp),
        )
    }
}

@Composable
private fun PortraitRpm(frame: DashboardFrameState) {
    Text(
        "${frame.rpm} RPM",
        color = DashboardSecondary,
        fontFamily = FontFamily.Monospace,
        fontWeight = FontWeight.SemiBold,
        fontSize = 12.sp,
        maxLines = 1,
    )
}

// ── Delta ───────────────────────────────────────────────────────────────────

/**
 * Lap delta over the fastest lap, then the three sectors that make it up. All
 * four use the same centre-out encoding, so the sector strip reads as a
 * breakdown of the bar above it rather than as three more numbers.
 */
@Composable
private fun PortraitDeltaBlock(frame: DashboardFrameState, modifier: Modifier) {
    val comparison = frame.lapComparison
    Column(modifier, verticalArrangement = Arrangement.SpaceEvenly) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            PortraitLabel("DELTA", Modifier.width(48.dp))
            PortraitCenterOutBar(
                comparison.lapDeltaSeconds,
                LapDeltaRangeSeconds,
                Modifier.weight(1f).height(10.dp),
            )
            PortraitValue(
                formatDelta(comparison.lapDeltaSeconds),
                deltaColor(comparison.lapDeltaSeconds),
                maxFontSize = 20.sp,
                minFontSize = 13.sp,
                modifier = Modifier.width(96.dp).padding(start = 12.dp),
                weight = FontWeight.Black,
                textAlign = TextAlign.End,
            )
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            PortraitSectorCell("S1", comparison.sector1DeltaSeconds, Modifier.weight(1f))
            PortraitSectorCell("S2", comparison.sector2DeltaSeconds, Modifier.weight(1f))
            PortraitSectorCell("S3", comparison.sector3DeltaSeconds, Modifier.weight(1f))
        }
    }
}

@Composable
private fun PortraitSectorCell(label: String, delta: Double?, modifier: Modifier) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        PortraitLabel(label)
        PortraitValue(
            formatDelta(delta),
            deltaColor(delta),
            maxFontSize = 13.sp,
            minFontSize = 8.sp,
            modifier = Modifier.padding(start = 7.dp),
        )
    }
}

/**
 * A bar that grows out from a fixed centre: left and green when the driver is
 * up on the reference, right and red when down, clamped at [rangeSeconds].
 * The centre tick stays visible at zero so the bar still reads as an
 * instrument when there is no delta yet.
 */
@Composable
private fun PortraitCenterOutBar(delta: Double?, rangeSeconds: Double, modifier: Modifier) {
    val signed = when {
        delta == null || !delta.isFinite() -> 0f
        else -> (delta / rangeSeconds).coerceIn(-1.0, 1.0).toFloat()
    }
    Row(modifier.clip(BarShape).background(PortraitBarTrack)) {
        Row(
            Modifier.weight(1f).fillMaxHeight(),
            horizontalArrangement = Arrangement.End,
        ) {
            if (signed < 0f) {
                Box(
                    Modifier.fillMaxHeight().fillMaxWidth(-signed)
                        .background(PortraitGreen, BarShape),
                )
            }
        }
        Box(Modifier.width(2.dp).fillMaxHeight().background(PortraitCenterTick))
        Row(Modifier.weight(1f).fillMaxHeight()) {
            if (signed > 0f) {
                Box(
                    Modifier.fillMaxHeight().fillMaxWidth(signed)
                        .background(PortraitRed, BarShape),
                )
            }
        }
    }
}

// ── Inputs ──────────────────────────────────────────────────────────────────

@Composable
private fun PortraitInputs(frame: DashboardFrameState, modifier: Modifier) {
    Column(modifier, verticalArrangement = Arrangement.SpaceEvenly) {
        PortraitThrottleBar(frame)
        PortraitBrakeBar(frame)
        PortraitErsBar(frame)
    }
}

@Composable
private fun PortraitThrottleBar(frame: DashboardFrameState) {
    val value = frame.throttle
    PortraitInputRow("THR", value, PortraitGreen, formatPercent(value), DashboardSecondary)
}

@Composable
private fun PortraitBrakeBar(frame: DashboardFrameState) {
    val value = frame.brake
    PortraitInputRow("BRK", value, PortraitRed, formatPercent(value), DashboardSecondary)
}

@Composable
private fun PortraitErsBar(frame: DashboardFrameState) {
    val cold = frame.cold
    PortraitInputRow(
        label = "ERS",
        value = cold.ersPercent / 100f,
        color = if (cold.ersPercent < 20) PortraitAmber else PortraitGreen,
        trailingText = ersModeLabel(cold),
        trailingColor = ersModeColor(cold.ersMode),
    )
}

/**
 * Both side columns are fixed width, so the three bars start and end at the
 * same x whatever their labels and values read.
 */
@Composable
private fun PortraitInputRow(
    label: String,
    value: Float,
    color: Color,
    trailingText: String,
    trailingColor: Color,
) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        PortraitLabel(label, Modifier.width(36.dp))
        Box(
            Modifier.weight(1f).height(14.dp).clip(BarShape).background(PortraitBarTrack),
        ) {
            Box(
                Modifier.fillMaxHeight().fillMaxWidth(value.coerceIn(0f, 1f))
                    .background(color, BarShape),
            )
        }
        PortraitValue(
            trailingText,
            trailingColor,
            maxFontSize = 13.sp,
            minFontSize = 8.sp,
            modifier = Modifier.width(68.dp).padding(start = 10.dp),
            textAlign = TextAlign.End,
        )
    }
}

// ── Data rail ───────────────────────────────────────────────────────────────

/**
 * The numbers a driver reads on a straight rather than in a corner: two flush
 * rows on a shared four-column grid, so the values line up down the page
 * without a container drawn around any of them.
 */
@Composable
private fun PortraitDataRail(frame: DashboardFrameState, modifier: Modifier) {
    Column(modifier) {
        PortraitTimingRow(frame, Modifier.fillMaxWidth().weight(1f))
        PortraitCarRow(frame, Modifier.fillMaxWidth().weight(1f))
    }
}

@Composable
private fun PortraitTimingRow(frame: DashboardFrameState, modifier: Modifier) {
    val cold = frame.cold
    val comparison = frame.lapComparison
    Row(
        modifier,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        PortraitStat(
            label = if (cold.lapInvalid) "LAP · INVALID" else "LAP",
            value = formatTime(cold.currentLapMs),
            valueColor = if (cold.lapInvalid) PortraitRed else DashboardPrimary,
            modifier = Modifier.weight(1f),
            labelColor = if (cold.lapInvalid) PortraitRed else DashboardSecondary,
        )
        PortraitStat("LAST", formatTime(cold.lastLapMs), DashboardPrimary, Modifier.weight(1f))
        PortraitStat(
            "BEST",
            formatTime(comparison.fastestLapMs),
            if (comparison.fastestLapMs > 0) DashboardFastest else DashboardSecondary,
            Modifier.weight(1f),
        )
    }
}

@Composable
private fun PortraitCarRow(frame: DashboardFrameState, modifier: Modifier) {
    val cold = frame.cold
    Row(
        modifier,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        PortraitStat(
            "FUEL",
            if (cold.statusAvailable) "%.1f KG".format(cold.fuelKg) else "—",
            DashboardPrimary,
            Modifier.weight(1f),
        )
        PortraitStat(
            "MARGIN",
            if (cold.statusAvailable) "%+.1f L".format(cold.fuelLaps) else "—",
            if (cold.statusAvailable) fuelMarginColor(cold.fuelLaps) else DashboardSecondary,
            Modifier.weight(1f),
        )
        PortraitStat(
            "BIAS",
            if (cold.statusAvailable) "${cold.brakeBias}%" else "—",
            DashboardPrimary,
            Modifier.weight(1f),
        )
    }
}

// ── Tyres ───────────────────────────────────────────────────────────────────

private enum class TyreCorner(val label: String) {
    FrontLeft("FL"), FrontRight("FR"), RearLeft("RL"), RearRight("RR")
}

/**
 * The four corners laid out as they sit on the car, so a hot or worn corner is
 * found by its position rather than by reading a label. This is the one place
 * containers earn their keep: the tile boundary is what makes "this corner"
 * legible, and their tint turns the block into a heat map that resolves before
 * any of the numbers in it do.
 */
@Composable
private fun PortraitTyreGrid(frame: DashboardFrameState, modifier: Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(
            Modifier.fillMaxWidth().weight(1f),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            PortraitTyreTile(frame, TyreCorner.FrontLeft, Modifier.weight(1f).fillMaxHeight())
            PortraitTyreTile(frame, TyreCorner.FrontRight, Modifier.weight(1f).fillMaxHeight())
        }
        Row(
            Modifier.fillMaxWidth().weight(1f),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            PortraitTyreTile(frame, TyreCorner.RearLeft, Modifier.weight(1f).fillMaxHeight())
            PortraitTyreTile(frame, TyreCorner.RearRight, Modifier.weight(1f).fillMaxHeight())
        }
    }
}

@Composable
private fun PortraitTyreTile(
    frame: DashboardFrameState,
    corner: TyreCorner,
    modifier: Modifier,
) {
    val surfaceTemperature = when (corner) {
        TyreCorner.FrontLeft -> frame.tyreSurfaceFl
        TyreCorner.FrontRight -> frame.tyreSurfaceFr
        TyreCorner.RearLeft -> frame.tyreSurfaceRl
        TyreCorner.RearRight -> frame.tyreSurfaceRr
    }
    val innerTemperature = when (corner) {
        TyreCorner.FrontLeft -> frame.tyreInnerFl
        TyreCorner.FrontRight -> frame.tyreInnerFr
        TyreCorner.RearLeft -> frame.tyreInnerRl
        TyreCorner.RearRight -> frame.tyreInnerRr
    }
    val cold = frame.cold
    val wear = when (corner) {
        TyreCorner.FrontLeft -> cold.tyreWearFl
        TyreCorner.FrontRight -> cold.tyreWearFr
        TyreCorner.RearLeft -> cold.tyreWearRl
        TyreCorner.RearRight -> cold.tyreWearRr
    }
    val temperatureTone = tyreTemperatureColor(surfaceTemperature)
    Surface(color = PortraitTileSurface, shape = TileShape, modifier = modifier) {
        Column(
            Modifier.fillMaxSize().padding(horizontal = 14.dp, vertical = 7.dp),
            verticalArrangement = Arrangement.SpaceEvenly,
        ) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                PortraitLabel(corner.label, Modifier.weight(1f))
                Text(
                    if (cold.tyreWearAvailable) formatWear(wear) else "—",
                    color = if (cold.tyreWearAvailable) {
                        tyreWearColor(wear)
                    } else {
                        PortraitPlaceholder
                    },
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                )
            }
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom) {
                PortraitValue(
                    formatTemperature(surfaceTemperature),
                    temperatureTone,
                    maxFontSize = 24.sp,
                    minFontSize = 14.sp,
                )
                Text(
                    " ${formatTemperature(innerTemperature)}",
                    color = tyreTemperatureColor(innerTemperature),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    modifier = Modifier.padding(bottom = 3.dp),
                )
            }
        }
    }
}

// ── Shared portrait primitives ──────────────────────────────────────────────

/** A label and value on one line, for rows too short to stack them. */
@Composable
private fun PortraitInline(
    label: String,
    value: String,
    valueColor: Color,
    modifier: Modifier = Modifier,
) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        PortraitLabel(label)
        PortraitValue(
            value,
            valueColor,
            maxFontSize = 14.sp,
            minFontSize = 9.sp,
            modifier = Modifier.weight(1f).padding(start = 7.dp),
        )
    }
}

/** A flush label-over-value pair. The page's only unit of tabular data. */
@Composable
private fun PortraitStat(
    label: String,
    value: String,
    valueColor: Color,
    modifier: Modifier = Modifier,
    labelColor: Color = DashboardSecondary,
) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(3.dp)) {
        PortraitLabel(label, color = labelColor)
        PortraitValue(
            value,
            valueColor,
            maxFontSize = 15.sp,
            minFontSize = 10.sp,
            modifier = Modifier.fillMaxWidth(),
        )
    }
}

@Composable
private fun PortraitLabel(
    text: String,
    modifier: Modifier = Modifier,
    color: Color = DashboardSecondary,
) {
    Text(
        text,
        modifier = modifier,
        color = color,
        fontSize = 9.sp,
        fontWeight = FontWeight.Bold,
        letterSpacing = 1.sp,
        maxLines = 1,
    )
}

/**
 * A monospace readout that shrinks to its container instead of clipping. Every
 * portrait value whose width depends on live data goes through this: at a
 * fixed size a signed delta or a lap time silently truncated to nothing in the
 * narrow cells this layout uses.
 *
 * An absent value is rendered small and dim rather than auto-sized. The
 * formatters return an em dash for "no data", which is the narrowest string
 * any of them produce, so auto-sizing drove it to the maximum and a dashboard
 * with no telemetry attached came up as a column of thick white bars.
 */
@Composable
private fun PortraitValue(
    text: String,
    color: Color,
    maxFontSize: TextUnit,
    minFontSize: TextUnit,
    modifier: Modifier = Modifier,
    weight: FontWeight = FontWeight.Bold,
    textAlign: TextAlign? = null,
) {
    val absent = isAbsentValue(text)
    Text(
        text,
        modifier = modifier,
        color = if (absent) PortraitPlaceholder else color,
        fontSize = if (absent) minFontSize else TextUnit.Unspecified,
        autoSize = if (absent) null else TextAutoSize.StepBased(minFontSize, maxFontSize, 1.sp),
        fontFamily = FontFamily.Monospace,
        fontWeight = if (absent) FontWeight.Normal else weight,
        textAlign = textAlign,
        maxLines = 1,
    )
}

/**
 * True for the strings the formatters use to stand in for missing telemetry —
 * an em dash on its own, or the "—.---" a null delta formats to.
 */
private fun isAbsentValue(text: String): Boolean =
    text.isBlank() || text.all { it == '—' || it == '-' || it == '.' || it == ' ' }

private fun formatPercent(value: Float): String =
    "${(value.coerceIn(0f, 1f) * 100f).roundToInt()}%"


@Composable
private fun CenterReadout(frame: DashboardFrameState, modifier: Modifier = Modifier) {
    val cold = frame.cold
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val gearSize = if (landscape) 100.sp else 72.sp
    val speedSize = if (landscape) 26.sp else 22.sp
    val rpmSize = if (landscape) 14.sp else 12.sp
    Column(
        modifier.fillMaxSize().padding(if (landscape) 12.dp else 10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.SpaceBetween,
    ) {
        Row(Modifier.fillMaxWidth()) {
            HeaderMetric(
                if (cold.position > 0) "P${cold.position}" else "—",
                Color(0xff43a5ff),
                Modifier.weight(1f),
                Alignment.Start,
            )
            HeaderMetric(
                formatTime(cold.lastLapMs),
                DashboardPrimary,
                Modifier.weight(1.2f),
                Alignment.CenterHorizontally,
            )
            HeaderMetric(
                tyreLabel(cold),
                tyreTone(cold.tyreCompound),
                Modifier.weight(1f),
                Alignment.End,
            )
        }
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                "${frame.speedKph} KM/H",
                color = DashboardPrimary,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold,
                fontSize = speedSize,
                lineHeight = speedSize,
            )
            Text(
                gearLabel(frame.gear),
                color = DashboardPrimary,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Black,
                fontSize = gearSize,
                lineHeight = gearSize,
            )
            Text(
                "${frame.rpm} RPM",
                color = DashboardSecondary,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.SemiBold,
                fontSize = rpmSize,
            )
        }
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                InputMeter(frame.throttle, Color(0xff32d583), Modifier.weight(1f))
                Column(
                    Modifier.padding(horizontal = 10.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text(
                        ersModeLabel(cold),
                        color = ersModeColor(cold.ersMode),
                        fontSize = 14.sp,
                        fontWeight = FontWeight.Bold,
                        maxLines = 1,
                    )
                }
                InputMeter(frame.brake, Color(0xffff4d5e), Modifier.weight(1f))
            }
            ErsMeter(cold.ersPercent)
        }
    }
}

@Composable
private fun HeaderMetric(
    value: String,
    color: Color,
    modifier: Modifier,
    alignment: Alignment.Horizontal,
) {
    Column(modifier, horizontalAlignment = alignment, verticalArrangement = Arrangement.Center) {
        Text(value, color = color, fontFamily = FontFamily.Monospace, fontSize = 16.sp, fontWeight = FontWeight.Bold, maxLines = 1)
    }
}

@Composable
private fun InputMeter(value: Float, color: Color, modifier: Modifier = Modifier) {
    LinearProgressIndicator(
        progress = { value.coerceIn(0f, 1f) },
        modifier = modifier.height(12.dp).clip(MaterialTheme.shapes.extraLarge),
        color = color,
        trackColor = DashboardDivider,
        strokeCap = StrokeCap.Round,
    )
}

@Composable
private fun ErsMeter(ersPercent: Int) {
    LinearProgressIndicator(
        progress = { (ersPercent / 100f).coerceIn(0f, 1f) },
        modifier = Modifier.fillMaxWidth().height(12.dp).clip(MaterialTheme.shapes.extraLarge),
        color = if (ersPercent < 20) Color(0xfff5b942) else Color(0xff32d583),
        trackColor = DashboardDivider,
        strokeCap = StrokeCap.Round,
    )
}

@Composable
private fun ValueCell(value: String, valueColor: Color, modifier: Modifier = Modifier) {
    Box(modifier.fillMaxSize().padding(10.dp), contentAlignment = Alignment.Center) {
        Text(
            value,
            color = valueColor,
            fontFamily = FontFamily.Monospace,
            fontSize = 19.sp,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
        )
    }
}

@Composable
private fun FuelCell(cold: DashboardColdState, modifier: Modifier = Modifier) {
    Column(
        modifier.fillMaxSize().padding(10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            if (cold.statusAvailable) "%.1f KG".format(cold.fuelKg) else "—",
            color = DashboardPrimary,
            fontFamily = FontFamily.Monospace,
            fontSize = 19.sp,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
        )
        if (cold.statusAvailable) {
            Text(
                "%+.1f LAPS".format(cold.fuelLaps),
                color = fuelMarginColor(cold.fuelLaps),
                fontFamily = FontFamily.Monospace,
                fontSize = 13.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
    }
}

@Composable
private fun EmptyDashboardCell(modifier: Modifier = Modifier) {
    Box(modifier.fillMaxSize())
}

@Composable
private fun FastestLapComparisonCell(
    comparison: DashboardLapComparisonState,
    modifier: Modifier = Modifier,
) {
    Column(modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().weight(1f)) {
            ComparisonMetric(
                label = "FASTEST LAP",
                value = formatTime(comparison.fastestLapMs),
                valueColor = if (comparison.fastestLapMs > 0) {
                    DashboardFastest
                } else {
                    DashboardSecondary
                },
                modifier = Modifier.weight(1.25f),
            )
            VerticalDashboardDivider()
            ComparisonMetric(
                label = "DELTA",
                value = formatDelta(comparison.lapDeltaSeconds),
                valueColor = deltaColor(comparison.lapDeltaSeconds),
                modifier = Modifier.weight(1f),
            )
        }
        HorizontalDashboardDivider()
        Row(Modifier.fillMaxWidth().weight(1f)) {
            ComparisonMetric(
                "S1",
                formatDelta(comparison.sector1DeltaSeconds),
                deltaColor(comparison.sector1DeltaSeconds),
                Modifier.weight(1f),
            )
            VerticalDashboardDivider()
            ComparisonMetric(
                "S2",
                formatDelta(comparison.sector2DeltaSeconds),
                deltaColor(comparison.sector2DeltaSeconds),
                Modifier.weight(1f),
            )
            VerticalDashboardDivider()
            ComparisonMetric(
                "S3",
                formatDelta(comparison.sector3DeltaSeconds),
                deltaColor(comparison.sector3DeltaSeconds),
                Modifier.weight(1f),
            )
        }
    }
}

@Composable
private fun ComparisonMetric(
    label: String,
    value: String,
    valueColor: Color,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier.fillMaxSize().padding(horizontal = 4.dp, vertical = 3.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            label,
            color = DashboardSecondary,
            fontSize = 9.sp,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
        )
        Text(
            value,
            color = valueColor,
            fontFamily = FontFamily.Monospace,
            fontSize = 14.sp,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
        )
    }
}

@Composable
private fun SideTyreBlock(
    frame: DashboardFrameState,
    leftSide: Boolean,
    showDividers: Boolean = true,
    modifier: Modifier = Modifier,
) {
    val cold = frame.cold
    Column(modifier) {
        if (leftSide) {
            TyreDataRow(frame.tyreSurfaceFl, frame.tyreInnerFl, cold.tyreWearFl, cold.tyreWearAvailable, showDividers, Modifier.weight(1f))
            if (showDividers) HorizontalDashboardDivider()
            TyreDataRow(frame.tyreSurfaceRl, frame.tyreInnerRl, cold.tyreWearRl, cold.tyreWearAvailable, showDividers, Modifier.weight(1f))
        } else {
            TyreDataRow(frame.tyreSurfaceFr, frame.tyreInnerFr, cold.tyreWearFr, cold.tyreWearAvailable, showDividers, Modifier.weight(1f))
            if (showDividers) HorizontalDashboardDivider()
            TyreDataRow(frame.tyreSurfaceRr, frame.tyreInnerRr, cold.tyreWearRr, cold.tyreWearAvailable, showDividers, Modifier.weight(1f))
        }
    }
}

@Composable
private fun TyreDataRow(
    surfaceTemperature: Int,
    innerTemperature: Int,
    wear: Float,
    wearAvailable: Boolean,
    showDividers: Boolean,
    modifier: Modifier = Modifier,
) {
    Row(modifier.fillMaxWidth()) {
        TyreStatCell(formatTemperature(surfaceTemperature), tyreTemperatureColor(surfaceTemperature), Modifier.weight(1f))
        if (showDividers) VerticalDashboardDivider()
        TyreStatCell(formatTemperature(innerTemperature), tyreTemperatureColor(innerTemperature), Modifier.weight(1f))
        if (showDividers) VerticalDashboardDivider()
        TyreStatCell(
            if (wearAvailable) formatWear(wear) else "—",
            if (wearAvailable) tyreWearColor(wear) else DashboardSecondary,
            Modifier.weight(1f),
        )
    }
}

@Composable
private fun TyreStatCell(value: String, color: Color, modifier: Modifier = Modifier) {
    Box(modifier.fillMaxHeight().padding(horizontal = 3.dp, vertical = 5.dp), contentAlignment = Alignment.Center) {
        Text(
            value,
            color = color,
            fontFamily = FontFamily.Monospace,
            fontSize = 15.sp,
            fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center,
            maxLines = 1,
        )
    }
}

@Composable
private fun VerticalDashboardDivider() {
    Box(Modifier.fillMaxHeight().width(1.dp).background(DashboardDivider))
}

@Composable
private fun HorizontalDashboardDivider() {
    Box(Modifier.fillMaxWidth().height(1.dp).background(DashboardDivider))
}

private fun lapLabel(cold: DashboardColdState): String = when {
    cold.lapNumber <= 0 -> "—"
    cold.totalLaps > 0 -> "${cold.lapNumber} / ${cold.totalLaps}"
    else -> cold.lapNumber.toString()
}

private fun formatTime(milliseconds: Int): String {
    if (milliseconds <= 0) return "—"
    val minutes = milliseconds / 60_000
    val seconds = milliseconds / 1_000 % 60
    val millis = milliseconds % 1_000
    return "%d:%02d.%03d".format(minutes, seconds, millis)
}

private fun formatDelta(seconds: Double?): String {
    if (seconds == null || !seconds.isFinite()) return "—.---"
    val normalized = if (abs(seconds) < 0.0005) 0.0 else seconds
    return if (normalized > 0) "+%.3f".format(normalized) else "%.3f".format(normalized)
}

private fun deltaColor(seconds: Double?): Color = when {
    seconds == null || !seconds.isFinite() -> DashboardSecondary
    abs(seconds) < 0.0005 -> DashboardSecondary
    seconds > 0 -> Color(0xffc4162a)
    seconds < 0 -> Color(0xff37872d)
    else -> DashboardPrimary
}

private fun formatTemperature(value: Int): String = if (value > 0) "$value°" else "—"

private fun formatWear(value: Float): String = "${value.coerceIn(0f, 100f).roundToInt()}%"

private fun tyreTemperatureColor(value: Int): Color = when {
    value <= 0 -> DashboardSecondary
    value < 60 -> Color(0xff5794f2)
    value < 80 -> Color(0xffd4ad04)
    value <= 110 -> Color(0xff37872d)
    value <= 130 -> Color(0xffc47d0e)
    else -> Color(0xffc4162a)
}

private fun tyreWearColor(value: Float): Color = when {
    value < 20f -> Color(0xff73bf69)
    value < 40f -> Color(0xffa8d436)
    value < 60f -> Color(0xfffade2a)
    value < 80f -> Color(0xffff9830)
    else -> Color(0xffc4162a)
}

private fun fuelMarginColor(fuelLaps: Double): Color = when {
    fuelLaps > 1.0 -> Color(0xff37872d)
    fuelLaps >= 0.0 -> Color(0xffd4ad04)
    else -> Color(0xffc4162a)
}

private fun ersModeLabel(cold: DashboardColdState): String =
    if (!cold.statusAvailable) "—" else cold.labels["ers.mode.${cold.ersMode}"] ?: when (cold.ersMode) {
        1 -> "AUTO"
        2 -> "HOTLAP"
        3 -> if (cold.protocolYear == 2026) "BOOST" else "OVERTAKE"
        else -> "NONE"
    }

private fun ersModeColor(mode: Int): Color = when (mode) {
    1 -> Color(0xff43a5ff)
    2 -> Color(0xfff5b942)
    3 -> Color(0xff32d583)
    else -> DashboardSecondary
}

private fun gearLabel(gear: Int): String = when {
    gear < 0 -> "R"
    gear == 0 -> "N"
    else -> gear.toString()
}

private fun tyreLabel(cold: DashboardColdState): String {
    if (!cold.statusAvailable) return "—"
    val compound = when (cold.tyreCompound) {
        16 -> "SOFT"
        17 -> "MED"
        18 -> "HARD"
        7 -> "INTER"
        8 -> "WET"
        else -> return "—"
    }
    return if (cold.tyreAgeLaps > 0) "$compound ${cold.tyreAgeLaps}L" else compound
}

private fun tyreTone(compound: Int): Color = when (compound) {
    16 -> Color(0xffff4d5e)
    17 -> Color(0xfff5b942)
    7 -> Color(0xff32d583)
    8 -> Color(0xff43a5ff)
    else -> Color(0xffd8dee6)
}
