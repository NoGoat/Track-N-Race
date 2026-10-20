package com.tracknrace.android.pages

import android.content.res.Configuration
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.PrimaryTabRow
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.asComposePath
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.tracknrace.android.CarDamageState
import com.tracknrace.android.DashboardColdState
import com.tracknrace.android.DashboardLapComparisonState
import com.tracknrace.android.HotTelemetry
import com.tracknrace.android.PairedTelemetryClient
import com.tracknrace.android.PairedTelemetryPage
import com.tracknrace.android.TelemetryStore
import com.tracknrace.android.TimingCarEntry
import com.tracknrace.android.TimingTowerState
import com.tracknrace.android.ui.currentWidthClass
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

// ── Design ──────────────────────────────────────────────────────────────────
// Material Design 3, in the app's light or dark theme. Four swipeable tabs:
//
//   DRIVE    the glance screen: shift lights, status, delta / gear / speed,
//            sector splits, ERS and fuel, gaps (races), tyres, lap times
//   MAP      the live map of every car, full page
//   DAMAGE   the desktop Damage page: faults and the worst part, the car with
//            each part's damage, power-unit and gearbox wear
//   STRATEGY the desktop Strategy page (DashboardStrategy.kt): the calls, the
//            next stop of each plan, both plans' stints, tyres and rivals
//
// Landscape keeps all four tabs, with Drive, Damage and Strategy laid out wide. Every changing number is painted by Readout and
// read while drawing, so a telemetry tick repaints one small layer and never
// re-lays out text; cards around them are cached as textures.

/** Race, Race 2 and Race 3 in F1 24, F1 25 and the 2026 Season Pack. */
internal val RaceSessionTypes = setOf(15, 16, 17)

/** Interval under which a neighbour is within attack range, and DRS range. */
private const val CloseIntervalMs = 1_000

/** A gap that changed less than this over a lap is holding, not a trend. */
private const val TrendMinMs = 50

private const val AlertDurationMs = 3_000L

private const val StatusSettleMs = 2_000L

// ── Material 3 theme ────────────────────────────────────────────────────────
// The app's theme, light or dark as the system is. Painted readouts are
// coloured outside composition, so the page resolves its colours once into a
// DashPalette: text and wireframe tones are the scheme's own roles, and the
// semantic colours, which M3 has no roles for, come in a dark and a light tone.

@Immutable
internal class DashPalette(
    val gain: Color,
    val loss: Color,
    val best: Color,
    val caution: Color,
    val cold: Color,
    /** Text tones: the scheme's onSurface, onSurfaceVariant and outline. */
    val hi: Color,
    val med: Color,
    val dis: Color,
    /** Ink on a filled semantic colour, such as a sector chip. */
    val onFill: Color,
    val hard: Color,
    val damageOk: Color,
    val damageWarn: Color,
    val damageCrit: Color,
    /** The desktop Damage page's five tyre bands, 20% each. */
    val tyreBands: List<Color>,
    val wire: Color,
    val tube: Color,
    val leader: Color,
    val leaderDot: Color,
    val wearTick: Color,
)

private fun tone(dark: Long, light: Long, isDark: Boolean) = Color(if (isDark) dark else light)

@Composable
private fun rememberDashPalette(): DashPalette {
    val scheme = MaterialTheme.colorScheme
    return remember(scheme) {
        val isDark = scheme.surface.luminance() < 0.5f
        DashPalette(
            gain = tone(0xff69f0ae, 0xff0d6b2f, isDark),
            loss = tone(0xffff5252, 0xffc4162a, isDark),
            best = tone(0xffb388ff, 0xff7c3ba6, isDark),
            caution = tone(0xffffd740, 0xff765900, isDark),
            cold = tone(0xff82b1ff, 0xff0b57d0, isDark),
            hi = scheme.onSurface,
            med = scheme.onSurfaceVariant,
            dis = scheme.outline,
            onFill = scheme.surface,
            hard = if (isDark) Color(0xffeeeeee) else scheme.outline,
            damageOk = tone(0xff37872d, 0xff2e6f25, isDark),
            damageWarn = tone(0xffe0a800, 0xff8a6100, isDark),
            damageCrit = Color(0xffc4162a),
            tyreBands = listOf(
                tone(0xff37872d, 0xff2e6f25, isDark),
                tone(0xff9bbf2e, 0xff5c7a12, isDark),
                tone(0xffe0a800, 0xff8a6100, isDark),
                tone(0xfff2711c, 0xffb34700, isDark),
                Color(0xffc4162a),
            ),
            wire = scheme.outline,
            tube = scheme.outlineVariant,
            leader = scheme.outlineVariant,
            leaderDot = scheme.outline,
            wearTick = scheme.outline,
        )
    }
}

internal val LocalDashPalette = staticCompositionLocalOf<DashPalette> { error("No dashboard palette") }

private val Numerals = FontFamily.SansSerif

/** The M3 type scale roles used here, in the system face. */
internal val Headline = TextStyle(fontFamily = Numerals, fontWeight = FontWeight.Normal, fontSize = 24.sp)
internal val Title = TextStyle(fontFamily = Numerals, fontWeight = FontWeight.Medium, fontSize = 20.sp)
internal val Body = TextStyle(fontFamily = Numerals, fontWeight = FontWeight.Normal, fontSize = 14.sp)
internal val BodySmall = TextStyle(fontFamily = Numerals, fontWeight = FontWeight.Normal, fontSize = 12.sp)
internal val Label = TextStyle(fontFamily = Numerals, fontWeight = FontWeight.Medium, fontSize = 12.sp, letterSpacing = 0.4.sp)

internal val CardShape = RoundedCornerShape(12.dp)
internal val CellShape = RoundedCornerShape(8.dp)

/** Which tab, and what it asks the desktop for. */
private enum class DashboardTab(val title: String, val page: PairedTelemetryPage) {
    DRIVE("Drive", PairedTelemetryPage.DASHBOARD),
    MAP("Map", PairedTelemetryPage.DASHBOARD_MAP),
    DAMAGE("Damage", PairedTelemetryPage.DASHBOARD_DAMAGE),
    STRATEGY("Strategy", PairedTelemetryPage.DASHBOARD_STRATEGY),
}

@Composable
internal fun DashboardScreen(
    store: TelemetryStore,
    active: Boolean = true,
    /** Called with the stream the visible tab needs. */
    onPage: (PairedTelemetryPage) -> Unit = {},
) {
    val frame = rememberDashboardFrameState(store, active)
    val palette = rememberDashPalette()
    val alert = rememberDashboardAlert(frame, palette)
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val race by remember(store) {
        derivedStateOf(structuralEqualityPolicy()) { store.cold.sessionType in RaceSessionTypes }
    }
    val gaps = rememberRaceGaps(store)
    KeepScreenOn(active && store.sourceStatus.state in setOf("paired", "listening", "playback"))

    // The app shell paints the background, so there is no full-screen surface here.
    CompositionLocalProvider(LocalDashPalette provides palette) {
        var saved by rememberSaveable { mutableIntStateOf(0) }
        val pager = rememberPagerState(initialPage = saved) { DashboardTab.entries.size }
        val scope = rememberCoroutineScope()
        val current = pager.currentPage
        LaunchedEffect(current, active) {
            saved = current
            if (active) onPage(DashboardTab.entries[current].page)
        }
        Column(Modifier.fillMaxSize()) {
            // Primary tabs continuing the app bar, as on Analysis.
            PrimaryTabRow(
                selectedTabIndex = current,
                modifier = Modifier.staticLayer(),
                containerColor = MaterialTheme.colorScheme.surfaceContainer,
                divider = {},
            ) {
                DashboardTab.entries.forEachIndexed { index, tab ->
                    Tab(
                        selected = current == index,
                        onClick = { scope.launch { pager.animateScrollToPage(index) } },
                        text = { Text(tab.title, maxLines = 1) },
                        selectedContentColor = MaterialTheme.colorScheme.primary,
                        unselectedContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            HorizontalPager(pager, Modifier.fillMaxWidth().weight(1f), beyondViewportPageCount = 0) { page ->
                TabletZoom {
                    when (DashboardTab.entries[page]) {
                        DashboardTab.DRIVE ->
                            if (landscape) LandscapeDrive(frame, alert, gaps, race) else DriveTab(frame, alert, gaps, race)
                        DashboardTab.MAP -> MapTab(store, active && current == page)
                        DashboardTab.DAMAGE -> DamageTab(store, landscape)
                        DashboardTab.STRATEGY -> StrategyTab(store, landscape)
                    }
                }
            }
        }
    }
}

/** Keeps the display on while a glance screen is showing live data. */
@Composable
private fun KeepScreenOn(enabled: Boolean) {
    val view = LocalView.current
    DisposableEffect(view, enabled) {
        view.keepScreenOn = enabled
        onDispose { view.keepScreenOn = false }
    }
}

/** How much larger a tablet draws each tab: the same layout, read from further away. */
private const val TabletZoomFactor = 1.3f

/** The tabs are drawn zoomed, at tablet widths. */
internal val LocalTabletDash = staticCompositionLocalOf { false }

/**
 * At tablet widths the tabs draw at [TabletZoomFactor] times the density, so
 * text, cards and spacing grow together and each layout gets the room of a
 * large phone, which is what it was laid out for.
 */
@Composable
private fun TabletZoom(content: @Composable () -> Unit) {
    if (!currentWidthClass().atLeastMedium) {
        content()
        return
    }
    val density = LocalDensity.current
    CompositionLocalProvider(
        LocalDensity provides Density(density.density * TabletZoomFactor, density.fontScale),
        LocalTabletDash provides true,
        content = content,
    )
}

// ── Drive ───────────────────────────────────────────────────────────────────

@Composable
private fun DriveTab(
    frame: DashboardFrameState,
    alert: State<DashboardAlert?>,
    gaps: State<RaceGaps>,
    race: Boolean,
) {
    Column(
        Modifier.fillMaxSize().padding(8.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        ShiftLights(frame, Modifier.fillMaxWidth().height(8.dp).graphicsLayer())
        StatusRow(frame, alert, Modifier.fillMaxWidth().height(32.dp))
        HeroCard(frame, Modifier.fillMaxWidth().weight(1f))
        SectorStrip(frame, Modifier.fillMaxWidth().height(22.dp))
        Row(Modifier.fillMaxWidth().height(66.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            ErsCard(frame, Modifier.weight(1f).fillMaxHeight())
            FuelCard(frame, Modifier.weight(1f).fillMaxHeight())
        }
        if (race) GapsCard(gaps, frame, Modifier.fillMaxWidth().height(58.dp))
        // A tablet's tall window shares its height with tyres and lap times
        // instead of giving it all to the hero.
        val bottom = if (LocalTabletDash.current) Modifier.weight(0.55f) else Modifier.height(132.dp)
        Row(Modifier.fillMaxWidth().then(bottom), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            TyreCard(frame, "Tyres", Modifier.weight(1.3f).fillMaxHeight())
            LapTimesCard(frame, Modifier.weight(1f).fillMaxHeight())
        }
    }
}

@Composable
private fun LandscapeDrive(
    frame: DashboardFrameState,
    alert: State<DashboardAlert?>,
    gaps: State<RaceGaps>,
    race: Boolean,
) {
    val tablet = LocalTabletDash.current
    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        ShiftLights(frame, Modifier.fillMaxWidth().height(8.dp).graphicsLayer())
        Row(Modifier.fillMaxWidth().weight(1f), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (tablet) {
                // A tablet's taller window: the cards at their own height, and
                // lap times in the room that frees; tyres take the right column.
                Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    StatusRow(frame, alert, Modifier.fillMaxWidth().height(32.dp))
                    ErsCard(frame, Modifier.fillMaxWidth().height(66.dp))
                    FuelCard(frame, Modifier.fillMaxWidth().height(66.dp))
                    if (race) GapsCard(gaps, frame, Modifier.fillMaxWidth().height(58.dp))
                    LapTimesCard(frame, Modifier.fillMaxWidth().weight(1f))
                }
                Column(Modifier.weight(1.15f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    HeroCard(frame, Modifier.fillMaxWidth().weight(1f))
                    SectorStrip(frame, Modifier.fillMaxWidth().height(22.dp))
                }
                TyreCard(frame, "Tyres", Modifier.weight(1f).fillMaxHeight())
                return@Row
            }
            Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                StatusRow(frame, alert, Modifier.fillMaxWidth().height(32.dp))
                ErsCard(frame, Modifier.fillMaxWidth().weight(1f))
                if (race) GapsCard(gaps, frame, Modifier.fillMaxWidth().height(58.dp))
                FuelCard(frame, Modifier.fillMaxWidth().weight(1f))
            }
            Column(Modifier.weight(1.15f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                HeroCard(frame, Modifier.fillMaxWidth().weight(1f))
                SectorStrip(frame, Modifier.fillMaxWidth().height(22.dp))
            }
            Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                TyreCard(frame, "Tyres", Modifier.fillMaxWidth().weight(1.4f))
                LapTimesCard(frame, Modifier.fillMaxWidth().weight(1f))
            }
        }
    }
}

// ── Building blocks ─────────────────────────────────────────────────────────

/**
 * An M3 filled card: a tonal container with no shadow, so no elevation pass.
 * Its rounded background is painted rather than clipped to: nothing inside
 * spills, and a rounded clip costs the GPU on every frame the content redraws.
 */
@Composable
internal fun DashCard(modifier: Modifier, cached: Boolean = false, content: @Composable ColumnScope.() -> Unit) {
    Column(
        // A card whose values change a few times a lap is cached as a texture:
        // unchanged, it costs the render thread one image instead of replaying
        // its drawing. A card with a readout that changes several times a
        // second is drawn live, as re-rendering its texture that often costs
        // more than it saves.
        // Its own drawing list either way, so a value changing elsewhere on
        // the page does not re-record this card's unchanged drawing.
        modifier.then(if (cached) Modifier.staticLayer() else Modifier.graphicsLayer())
            .background(MaterialTheme.colorScheme.surfaceContainer, CardShape)
            .padding(horizontal = 12.dp, vertical = 8.dp),
        content = content,
    )
}

@Composable
internal fun CardLabel(text: String, modifier: Modifier = Modifier, color: Color = LocalDashPalette.current.med) {
    Text(text, modifier, style = Label, color = color, maxLines = 1, overflow = TextOverflow.Ellipsis)
}

/** A painted readout in one of the M3 type roles. */
@Composable
private fun Live(
    style: TextStyle,
    modifier: Modifier = Modifier,
    alignEnd: Boolean = false,
    center: Boolean = false,
    runs: () -> List<ReadoutRun>,
) = Readout(runs, style, modifier, alignEnd, center)

// ── Shift lights ────────────────────────────────────────────────────────────

@Composable
private fun ShiftLights(frame: DashboardFrameState, modifier: Modifier) {
    val c = LocalDashPalette.current
    val off = MaterialTheme.colorScheme.surfaceContainerHighest
    // Drawn, not composed: the bit pattern changes many times a second.
    Spacer(
        modifier.semantics { contentDescription = "Shift lights" }.drawBehind {
            val bitValue = frame.revLightsBitValue
            val gap = 2.dp.toPx()
            val width = (size.width - gap * 14) / 15f
            val corner = CornerRadius(2.dp.toPx())
            repeat(15) { index ->
                val lit = bitValue != null && bitValue and (1 shl index) != 0
                val tone = when {
                    index < 5 -> c.gain
                    index < 10 -> c.loss
                    else -> c.cold
                }
                drawRoundRect(if (lit) tone else off, Offset(index * (width + gap), 0f), Size(width, size.height), corner)
            }
        },
    )
}

// ── Status row ──────────────────────────────────────────────────────────────

/** Position and places gained, lap, fitted tyre, penalty and DRS chips; an alert takes the row. */
@Composable
private fun StatusRow(frame: DashboardFrameState, alert: State<DashboardAlert?>, modifier: Modifier) {
    val c = LocalDashPalette.current
    val shown = alert.value
    if (shown != null) {
        Box(
            modifier
                .background(shown.color.copy(alpha = 0.18f), CellShape)
                .semantics { liveRegion = LiveRegionMode.Polite },
            contentAlignment = Alignment.CenterStart,
        ) {
            Text(shown.text, Modifier.padding(horizontal = 12.dp), style = Title.copy(fontSize = 16.sp), color = shown.color, maxLines = 1)
        }
        return
    }
    // Changes with a lap, a pass or a tyre stop: cached as a texture between.
    Row(modifier.staticLayer(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        val position = frame.position
        val gained = frame.placesGained
        Text(if (position > 0) "P$position" else "P–", style = Headline.copy(fontWeight = FontWeight.Bold, fontSize = 22.sp), color = c.hi, maxLines = 1)
        if (gained != 0) {
            Text(
                if (gained > 0) "▲$gained" else "▼${-gained}",
                style = BodySmall.copy(fontWeight = FontWeight.Medium),
                color = if (gained > 0) c.gain else c.loss,
                modifier = Modifier.padding(start = 0.dp),
            )
        }
        Text(frame.lapText, style = Body, color = c.med, maxLines = 1, modifier = Modifier.weight(1f))
        frame.penaltyText?.let { Chip(it, c.loss.copy(alpha = 0.18f), c.loss) }
        val compound = compoundColor(c, frame.tyreCompound)
        if (frame.statusAvailable && compound != null) {
            Chip(frame.tyreChipText, Color.Transparent, c.hi, dot = compound, outline = MaterialTheme.colorScheme.outline)
        }
        if (frame.aeroOpen) Chip(if (frame.slmCar) "SLM" else "DRS", c.gain.copy(alpha = 0.18f), c.gain)
    }
}

/** An M3 chip: 8 dp corners, outlined or tonal. */
@Composable
private fun Chip(text: String, background: Color, content: Color, dot: Color? = null, outline: Color? = null) {
    Row(
        Modifier
            .height(28.dp)
            .background(background, CellShape)
            .then(if (outline != null) Modifier.border(1.dp, outline, CellShape) else Modifier)
            .padding(horizontal = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (dot != null) {
            Box(Modifier.size(9.dp).background(dot, CircleShape))
            Spacer(Modifier.width(5.dp))
        }
        Text(text, style = Label, color = content, maxLines = 1)
    }
}

// ── Hero ────────────────────────────────────────────────────────────────────

/** Delta, gear, speed and RPM on the centre line; throttle and brake at the edges. */
@Composable
private fun HeroCard(frame: DashboardFrameState, modifier: Modifier) {
    val c = LocalDashPalette.current
    Box(modifier.graphicsLayer().background(MaterialTheme.colorScheme.surfaceContainer, CardShape)) {
        Box(Modifier.fillMaxSize()) {
            PedalBar({ frame.throttle }, c.gain, Modifier.align(Alignment.CenterStart).padding(start = 10.dp, top = 12.dp, bottom = 12.dp))
            PedalBar({ frame.brake }, c.loss, Modifier.align(Alignment.CenterEnd).padding(end = 10.dp, top = 12.dp, bottom = 12.dp))
            Column(
                Modifier.fillMaxSize().padding(horizontal = 24.dp, vertical = 6.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Live(Headline.copy(fontSize = 22.sp, fontWeight = FontWeight.Medium), Modifier.fillMaxWidth(), center = true) {
                    val delta = frame.lapDeltaSeconds
                    listOf(ReadoutRun(formatDelta(delta), if (delta == null) c.dis else deltaColor(c, delta)))
                }
                Gear(frame, Modifier.fillMaxWidth().weight(1f))
                Live(Headline, Modifier.fillMaxWidth(), center = true) {
                    listOf(ReadoutRun(frame.speedKph.toString(), c.hi), ReadoutRun(" km/h", c.med, bold = false, scale = 0.5f))
                }
                Live(BodySmall, Modifier.fillMaxWidth(), center = true) {
                    val rpm = frame.rpm
                    if (rpm <= 0) emptyList() else listOf(ReadoutRun("%,d".format(rpm), c.med, bold = false), ReadoutRun(" rpm", c.dis, bold = false))
                }
            }
        }
    }
}

/** A pedal's travel, filled from the bottom. */
@Composable
private fun PedalBar(value: () -> Float, color: Color, modifier: Modifier) {
    val track = MaterialTheme.colorScheme.surfaceContainerHighest
    Spacer(
        modifier.width(8.dp).fillMaxHeight().graphicsLayer().drawBehind {
            val corner = CornerRadius(size.width / 2f)
            drawRoundRect(track, cornerRadius = corner)
            val fill = value().coerceIn(0f, 1f) * size.height
            if (fill > 0f) drawRoundRect(color, Offset(0f, size.height - fill), Size(size.width, fill), corner)
        },
    )
}

/** The largest glyph, sized once to the space it has and cached as a texture. */
@Composable
private fun Gear(frame: DashboardFrameState, modifier: Modifier) {
    val c = LocalDashPalette.current
    BoxWithConstraints(modifier, contentAlignment = Alignment.Center) {
        val density = LocalDensity.current
        // A tablet's hero has room for a larger digit.
        val cap = if (LocalTabletDash.current) 220.dp else 144.dp
        val fontSize = with(density) { (maxHeight.toPx() * 0.92f).coerceAtMost(cap.toPx()).toSp() }
        Text(
            gearLabel(frame.gear),
            modifier = Modifier.graphicsLayer(compositingStrategy = CompositingStrategy.Offscreen),
            style = TextStyle(fontFamily = Numerals, fontWeight = FontWeight.Black, fontSize = fontSize, lineHeight = fontSize),
            color = c.hi,
            textAlign = TextAlign.Center,
            maxLines = 1,
        )
    }
}

// ── Sectors ─────────────────────────────────────────────────────────────────

/** This lap's sector deltas to the best lap: green faster, amber slower, grey not reached. */
@Composable
private fun SectorStrip(frame: DashboardFrameState, modifier: Modifier) {
    val c = LocalDashPalette.current
    val density = LocalDensity.current
    val paint = remember(density) { textPaint(with(density) { 11.sp.toPx() }, FontWeight.Medium.weight) }
    val neutral = MaterialTheme.colorScheme.surfaceContainerHigh
    Spacer(
        modifier.graphicsLayer().semantics { contentDescription = "Sector deltas" }.drawBehind {
            val deltas = frame.sectorDeltas
            val gap = 4.dp.toPx()
            val width = (size.width - gap * 2) / 3f
            val pad = 6.dp.toPx()
            val canvas = drawContext.canvas.nativeCanvas
            val metrics = paint.fontMetrics
            val baseline = size.height / 2f - (metrics.ascent + metrics.descent) / 2f
            for (sector in 0..2) {
                val value = deltas.getOrNull(sector)
                val x = sector * (width + gap)
                val (fill, text) = when {
                    value == null -> neutral to c.dis
                    // A split of exactly zero is the line just crossed, not a gain.
                    abs(value) < 0.0005 -> neutral to c.hi
                    value < 0 -> c.gain to c.onFill
                    else -> c.caution to c.onFill
                }
                drawRoundRect(fill, Offset(x, 0f), Size(width, size.height), CornerRadius(8.dp.toPx()))
                paint.color = text.toArgb()
                canvas.drawText("S${sector + 1}", x + pad, baseline, paint)
                val delta = if (value == null) "—" else formatDelta(value)
                canvas.drawText(delta, x + width - pad - paint.measureText(delta), baseline, paint)
            }
        },
    )
}

// ── ERS and fuel ────────────────────────────────────────────────────────────

@Composable
private fun ErsCard(frame: DashboardFrameState, modifier: Modifier) {
    val c = LocalDashPalette.current
    DashCard(modifier) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            CardLabel("ERS", Modifier.weight(1f))
            Text(
                ersModeLabel(frame.ersMode),
                Modifier
                    .background(MaterialTheme.colorScheme.tertiaryContainer, RoundedCornerShape(6.dp))
                    .padding(horizontal = 8.dp, vertical = 1.dp),
                style = Label.copy(fontSize = 11.sp),
                color = MaterialTheme.colorScheme.onTertiaryContainer,
                maxLines = 1,
            )
        }
        Live(Title, Modifier.fillMaxWidth()) {
            if (!frame.statusAvailable) listOf(ReadoutRun("–", c.dis)) else listOf(ReadoutRun("${frame.ersPercent}%", ersColor(c, frame.ersPercent)))
        }
        val track = MaterialTheme.colorScheme.surfaceContainerHighest
        Spacer(
            Modifier.fillMaxWidth().height(4.dp).graphicsLayer().drawBehind {
                val corner = CornerRadius(size.height / 2f)
                drawRoundRect(track, cornerRadius = corner)
                if (frame.statusAvailable) {
                    drawRoundRect(
                        ersColor(c, frame.ersPercent),
                        size = Size(size.width * (frame.ersPercent / 100f).coerceIn(0f, 1f), size.height),
                        cornerRadius = corner,
                    )
                }
            },
        )
    }
}

@Composable
private fun FuelCard(frame: DashboardFrameState, modifier: Modifier) {
    val c = LocalDashPalette.current
    DashCard(modifier, cached = true) {
        CardLabel("Fuel")
        Live(Title, Modifier.fillMaxWidth()) {
            if (!frame.statusAvailable) {
                listOf(ReadoutRun("–", c.dis))
            } else {
                listOf(ReadoutRun("%+.1f".format(frame.fuelLaps), fuelMarginColor(c, frame.fuelLaps)), ReadoutRun(" laps", c.med, bold = false, scale = 0.55f))
            }
        }
        Live(BodySmall, Modifier.fillMaxWidth()) {
            val target = frame.fuelTargetText
            if (target.isEmpty()) emptyList() else listOf(ReadoutRun(target, c.med, bold = false))
        }
    }
}

// ── Gaps ────────────────────────────────────────────────────────────────────

@Composable
private fun GapsCard(gaps: State<RaceGaps>, frame: DashboardFrameState, modifier: Modifier) {
    val c = LocalDashPalette.current
    DashCard(modifier, cached = true) {
        val current = gaps.value
        Row(Modifier.fillMaxSize(), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                CardLabel(if (current.leading) "Leading" else "Ahead · ${current.aheadName ?: "–"}")
                Live(Title, Modifier.fillMaxWidth()) {
                    val now = gaps.value
                    if (now.leading) emptyList() else gapRuns(c, now.aheadMs, "−", c.gain, frame.aheadTrendMs, closingIsGood = true)
                }
            }
            Box(Modifier.width(1.dp).fillMaxHeight(0.8f).background(MaterialTheme.colorScheme.outlineVariant))
            Column(Modifier.weight(1f).padding(start = 10.dp), horizontalAlignment = Alignment.End) {
                CardLabel(if (current.last) "Last" else "Behind · ${current.behindName ?: "–"}")
                Live(Title, Modifier.fillMaxWidth(), alignEnd = true) {
                    val now = gaps.value
                    if (now.last) emptyList() else gapRuns(c, now.behindMs, "+", c.loss, frame.behindTrendMs, closingIsGood = false)
                }
            }
        }
    }
}

/** An interval with its trend per lap: ▼ closing, ▲ opening. */
private fun gapRuns(c: DashPalette, interval: Int?, sign: String, closeColor: Color, trendMs: Int?, closingIsGood: Boolean): List<ReadoutRun> {
    val close = interval != null && interval in 1 until CloseIntervalMs
    val runs = mutableListOf(
        ReadoutRun(formatInterval(interval, sign), when {
            interval == null -> c.dis
            close -> closeColor
            else -> c.hi
        }),
    )
    if (trendMs != null && abs(trendMs) >= TrendMinMs && interval != null) {
        val closing = trendMs < 0
        val good = closing == closingIsGood
        runs += ReadoutRun(
            "  ${if (closing) "▼" else "▲"} %.1f/lap".format(abs(trendMs) / 1000.0),
            if (good) c.gain else c.med,
            bold = false,
            scale = 0.55f,
        )
    }
    return runs
}

// ── Tyres ───────────────────────────────────────────────────────────────────

/**
 * Wear leads each corner, then surface temperature, then inner temperature;
 * the legend is said once in the title.
 */
@Composable
private fun TyreCard(frame: DashboardFrameState, title: String, modifier: Modifier) {
    val c = LocalDashPalette.current
    DashCard(modifier) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            CardLabel(title, Modifier.weight(1f))
            Text("wear · surface · inner", style = BodySmall.copy(fontSize = 10.sp), color = c.dis, maxLines = 1)
        }
        Spacer(Modifier.height(4.dp))
        Column(Modifier.fillMaxWidth().weight(1f), verticalArrangement = Arrangement.spacedBy(5.dp)) {
            Row(Modifier.fillMaxWidth().weight(1f), horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                TyreCell(frame, 0, "FL", Modifier.weight(1f).fillMaxHeight())
                TyreCell(frame, 1, "FR", Modifier.weight(1f).fillMaxHeight())
            }
            Row(Modifier.fillMaxWidth().weight(1f), horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                TyreCell(frame, 2, "RL", Modifier.weight(1f).fillMaxHeight())
                TyreCell(frame, 3, "RR", Modifier.weight(1f).fillMaxHeight())
            }
        }
    }
}

@Composable
private fun TyreCell(frame: DashboardFrameState, corner: Int, label: String, modifier: Modifier) {
    val c = LocalDashPalette.current
    Box(modifier.background(MaterialTheme.colorScheme.surfaceContainerHigh, CellShape).padding(horizontal = 8.dp, vertical = 4.dp)) {
        Text(label, Modifier.align(Alignment.TopStart), style = Label.copy(fontSize = 10.sp), color = c.med, maxLines = 1)
        Row(Modifier.align(Alignment.BottomStart).fillMaxWidth(), verticalAlignment = Alignment.Bottom) {
            Live(Headline.copy(fontSize = 22.sp, fontWeight = FontWeight.Medium), Modifier.weight(1f)) {
                val wear = frame.wear(corner)
                if (wear == null) listOf(ReadoutRun("–", c.dis)) else listOf(ReadoutRun(wear.toString(), tyreBandColor(c, wear)), ReadoutRun("%", c.med, bold = false, scale = 0.5f))
            }
            Column(horizontalAlignment = Alignment.End) {
                Live(Body.copy(fontSize = 13.sp, fontWeight = FontWeight.Medium), Modifier.width(40.dp), alignEnd = true) {
                    val surface = frame.surface(corner)
                    if (surface <= 0) listOf(ReadoutRun("–", c.dis)) else listOf(ReadoutRun("$surface°", tyreTemperatureColor(c, surface)))
                }
                Live(BodySmall.copy(fontSize = 10.sp), Modifier.width(40.dp), alignEnd = true) {
                    val inner = frame.inner(corner)
                    if (inner <= 0) emptyList() else listOf(ReadoutRun("$inner°", c.dis, bold = false))
                }
            }
        }
    }
}

// ── Lap times ───────────────────────────────────────────────────────────────

@Composable
private fun LapTimesCard(frame: DashboardFrameState, modifier: Modifier) {
    val c = LocalDashPalette.current
    DashCard(modifier) {
        CardLabel("Lap times")
        Column(Modifier.fillMaxWidth().weight(1f), verticalArrangement = Arrangement.SpaceEvenly) {
            TimeRow("Lap") { listOf(ReadoutRun(formatRunning(frame.currentLapMs), c.hi)) }
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            TimeRow("Last") {
                val last = frame.lastLapMs
                val best = frame.fastestLapMs
                listOf(ReadoutRun(formatTime(last), if (last > 0 && best > 0 && last == best) c.best else if (last > 0) c.hi else c.dis))
            }
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            TimeRow("Best") { listOf(ReadoutRun(formatTime(frame.fastestLapMs), if (frame.fastestLapMs > 0) c.best else c.dis)) }
        }
    }
}

@Composable
private fun TimeRow(label: String, value: () -> List<ReadoutRun>) {
    val c = LocalDashPalette.current
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = BodySmall, color = c.med, modifier = Modifier.width(36.dp))
        Live(Body.copy(fontWeight = FontWeight.Medium), Modifier.weight(1f), alignEnd = true, runs = value)
    }
}

// ── Map tab ─────────────────────────────────────────────────────────────────

@Composable
private fun MapTab(store: TelemetryStore, active: Boolean) {
    LiveMap(store, active, Modifier.fillMaxSize().padding(8.dp))
}

/**
 * Every car on the circuit, as the desktop map draws it: a dot in the car's
 * livery colour with its three-letter code above. Your car is drawn last.
 */
@Composable
private fun LiveMap(store: TelemetryStore, active: Boolean, modifier: Modifier) {
    // Every display frame while shown: the map eases each dot toward its car.
    val tick = remember { mutableLongStateOf(0L) }
    LaunchedEffect(active) {
        if (!active) return@LaunchedEffect
        while (true) withFrameNanos { tick.longValue = it }
    }
    val trackId by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.cold.trackId } }
    val aeroMode by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.cold.aeroMode } }
    val roster by remember(store) {
        derivedStateOf(structuralEqualityPolicy()) {
            store.timing.drivers.values.sortedBy { it.index }.map { driver ->
                Triple(driver.index, liveryColor(driver.teamColor), mapLabel(driver.name, driver.raceNumber))
            }
        }
    }
    val you = store.selectedDriverIndex
    val cars = remember(roster, you) {
        val (mine, others) = roster.partition { it.first == you }
        (others + mine).map { (index, color, label) -> LiveMapCar(index, color, label) }
    }
    LiveTrackMap(
        trackId = trackId,
        aeroMode = aeroMode,
        positions = store::latestMapPositions,
        frameTick = tick,
        cars = cars,
        trackColor = MaterialTheme.colorScheme.onSurface,
        modifier = modifier,
    )
}

/** The desktop map label: the surname's first three letters, else the race number. */
private fun mapLabel(name: String, raceNumber: Int): String {
    val surname = name.trim().split(Regex("\\s+")).lastOrNull().orEmpty()
    return if (surname.isNotEmpty()) surname.take(3).uppercase() else raceNumber.toString()
}

private fun liveryColor(value: String?): Color = try {
    Color(android.graphics.Color.parseColor(value ?: "#8e8e8e"))
} catch (_: IllegalArgumentException) {
    Color(0xff8e8e8e)
}

// ── Damage tab ──────────────────────────────────────────────────────────────
// The desktop Damage page on a phone: one line for faults and the worst part,
// the car with each part's damage beside it and a leader to the part, and
// power-unit and gearbox wear as bars. Colours and thresholds are Electron's:
// body damage and wear on a three-step scale, tyre, brake and blister damage
// on its five 20% bands. Tyre values keep their green band; body damage and
// wear stay uncoloured until the first warning step, so damage stands out.

private val WheelCalloutWidth = 84.dp
private val PartValue = Body.copy(fontSize = 17.sp, lineHeight = 19.sp, fontWeight = FontWeight.Bold)

/** Body damage: any is worth seeing; above 20% it costs real performance. */
private fun damageColor(c: DashPalette, value: Int?): Color? = when {
    value == null -> null
    value == 0 -> c.damageOk
    value <= 20 -> c.damageWarn
    else -> c.damageCrit
}

/** A body part's value: dimmed while undamaged. */
private fun partColor(c: DashPalette, value: Int?): Color = if (value == null || value == 0) c.dis else damageColor(c, value) ?: c.dis

private fun tyreColor(c: DashPalette, value: Int?): Color = value?.let { tyreBandColor(c, it) } ?: c.dis

/** Power-unit and gearbox wear accumulate normally, so only high wear warns. */
private fun wearColor(c: DashPalette, value: Int?): Color = when {
    value == null || value == 0 -> c.dis
    value < 50 -> c.hi
    value < 75 -> c.damageWarn
    else -> c.damageCrit
}

private fun percentText(value: Int?) = value?.let { "$it%" } ?: "—"

/** A percentage with a smaller % sign. */
private fun percent(value: Int?): AnnotatedString = buildAnnotatedString {
    if (value == null) {
        append("—")
    } else {
        append(value.toString())
        withStyle(SpanStyle(fontSize = 0.65.em, fontWeight = FontWeight.Medium)) { append("%") }
    }
}

private class WheelDamage(val name: String, val tyre: Int?, val brake: Int?, val blisters: Int?)

private fun wheels(damage: CarDamageState) = listOf(
    WheelDamage("FL", damage.tyreFl, damage.brakeFl, damage.blistersFl),
    WheelDamage("FR", damage.tyreFr, damage.brakeFr, damage.blistersFr),
    WheelDamage("RL", damage.tyreRl, damage.brakeRl, damage.blistersRl),
    WheelDamage("RR", damage.tyreRr, damage.brakeRr, damage.blistersRr),
)

/** Where a leader leaves the car for its callout. */
private enum class LeaderSide { LEFT, RIGHT, TOP, BOTTOM }

/** A leader from the car's edge to a point on a part, in the nose-up drawing's units. */
private class Leader(val zone: String, val x: Float, val y: Float, val side: LeaderSide)

/** Portrait, nose up: each side's callouts top to bottom. */
private val PortraitLeaders = listOf(
    Leader("z-fw-l", 34f, 62f, LeaderSide.LEFT),
    Leader("t-fl", 20f, 190f, LeaderSide.LEFT),
    Leader("z-sp-l", 55f, 520f, LeaderSide.LEFT),
    Leader("t-rl", 2f, 757f, LeaderSide.LEFT),
    Leader("z-fw-r", 316f, 62f, LeaderSide.RIGHT),
    Leader("t-fr", 330f, 190f, LeaderSide.RIGHT),
    Leader("z-floor", 323f, 335f, LeaderSide.RIGHT),
    Leader("t-rr", 348f, 757f, LeaderSide.RIGHT),
)

/** Landscape, nose right with the driver's left on top: each band's callouts left to right. */
private val LandscapeLeaders = listOf(
    Leader("t-rl", 2f, 757f, LeaderSide.TOP),
    Leader("z-floor", 38f, 380f, LeaderSide.TOP),
    Leader("t-fl", 20f, 190f, LeaderSide.TOP),
    Leader("z-fw-l", 34f, 62f, LeaderSide.TOP),
    Leader("t-rr", 348f, 757f, LeaderSide.BOTTOM),
    Leader("z-sp-r", 296f, 520f, LeaderSide.BOTTOM),
    Leader("t-fr", 330f, 190f, LeaderSide.BOTTOM),
    Leader("z-fw-r", 316f, 62f, LeaderSide.BOTTOM),
)

@Composable
private fun DamageTab(store: TelemetryStore, landscape: Boolean) {
    val c = LocalDashPalette.current
    val damage by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.damage } }
    // DRS, or Rear Wing for F1 26, from the desktop's label catalogue.
    val wingFault by remember(store) {
        derivedStateOf(structuralEqualityPolicy()) { store.cold.labels["ui.damage.wing_fault"] ?: "DRS" }
    }
    val context = LocalContext.current.applicationContext
    val wireframe by produceState<CarWireframe?>(null, context) {
        value = withContext(Dispatchers.IO) { loadCarWireframe(context) }
    }
    val drawn = { store.damage }
    val faults = faultSummary(damage, wingFault)
    val worst = worstPart(c, damage)
    // Never scrolls: the car card takes the room the others leave, and the
    // car and its callouts are fitted into it.
    if (landscape) {
        // The car nose-right as on the desktop, faults and wear in a column beside it.
        Row(
            Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            WideCarDamageCard(damage, drawn, wireframe, Modifier.weight(1f).fillMaxHeight())
            Column(Modifier.width(220.dp).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                DamageStatusCard(faults, worst, Modifier.fillMaxWidth())
                WearCard(damage, columns = 1, Modifier.fillMaxWidth().weight(1f))
            }
        }
    } else {
        Column(Modifier.fillMaxSize().padding(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(Modifier.fillMaxWidth().height(32.dp).padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                FaultChip(faults)
                Spacer(Modifier.weight(1f).width(8.dp))
                if (worst != null) WorstReadout(worst)
            }
            CarDamageCard(damage, drawn, wireframe, Modifier.fillMaxWidth().weight(1f))
            WearCard(damage, columns = 2, Modifier.fillMaxWidth())
        }
    }
}

/**
 * The game sets blown or seized for some failures only (not an MGU-H failure,
 * say), so overall engine wear at 100% without either reads as failed.
 */
private fun engineStatus(damage: CarDamageState): String? = when {
    damage.engineSeized == 1 -> "Seized"
    damage.engineBlown == 1 -> "Blown"
    damage.engine != null && damage.engine >= 100 -> "Failed"
    damage.engineSeized == null && damage.engineBlown == null -> null
    else -> "OK"
}

/** The engine, wing (DRS) and ERS checks: what has failed, and what is fine. */
private class FaultSummary(val failed: List<String>, val ok: List<String>) {
    val known get() = failed.isNotEmpty() || ok.isNotEmpty()
}

private fun faultSummary(damage: CarDamageState, wingFault: String): FaultSummary {
    val failed = mutableListOf<String>()
    val ok = mutableListOf<String>()
    when (val engine = engineStatus(damage)) {
        null -> Unit
        "OK" -> ok += "Engine"
        else -> failed += "Engine ${engine.lowercase()}"
    }
    for ((name, flag) in listOf(wingFault to damage.drsFault, "ERS" to damage.ersFault)) {
        when (flag) {
            null -> Unit
            0 -> ok += name
            else -> failed += "$name fault"
        }
    }
    return FaultSummary(failed, ok)
}

private class WorstPart(val name: String, val value: Int, val color: Color)

/** The most damaged part: the highest severity step, then the highest value. */
private fun worstPart(c: DashPalette, damage: CarDamageState): WorstPart? {
    var worst: WorstPart? = null
    var worstRank = -1
    fun offer(name: String, value: Int?, rank: Int, color: Color) {
        if (value == null) return
        if (rank > worstRank || rank == worstRank && value > (worst?.value ?: -1)) {
            worst = WorstPart(name, value, color)
            worstRank = rank
        }
    }
    for (wheel in wheels(damage)) {
        for ((part, value) in listOf("tyre" to wheel.tyre, "brake" to wheel.brake, "blisters" to wheel.blisters)) {
            // The five 20% bands.
            offer("${wheel.name} $part", value, ((value ?: 0) / 20).coerceAtMost(4), tyreColor(c, value))
        }
    }
    for ((name, value) in listOf(
        "Wing L" to damage.wingFl, "Wing R" to damage.wingFr, "Rear wing" to damage.wingRear,
        "Floor" to damage.floor, "Sidepods" to damage.sidepods, "Diffuser" to damage.diffuser,
    )) {
        // On the same steps as the tyre bands' amber and red.
        val rank = when {
            value == null || value == 0 -> 0
            value <= 20 -> 2
            else -> 4
        }
        offer(name, value, rank, partColor(c, value))
    }
    return worst
}

/** "No faults", or a filled chip naming what has failed. */
@Composable
private fun FaultChip(faults: FaultSummary) {
    val c = LocalDashPalette.current
    val failed = faults.failed.isNotEmpty()
    val fill = when {
        failed -> MaterialTheme.colorScheme.error
        faults.known -> c.damageOk.copy(alpha = 0.22f)
        else -> MaterialTheme.colorScheme.surfaceContainerHigh
    }
    val ink = when {
        failed -> MaterialTheme.colorScheme.onError
        faults.known -> c.damageOk
        else -> c.dis
    }
    val text = when {
        failed -> faults.failed.joinToString(" · ")
        faults.known -> "No faults"
        else -> "No data"
    }
    Row(
        Modifier.height(26.dp).background(fill, CircleShape).padding(start = 8.dp, end = 11.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(5.dp),
    ) {
        if (faults.known) {
            Spacer(
                Modifier.size(14.dp).drawBehind {
                    val w = size.width
                    val h = size.height
                    if (failed) {
                        val triangle = Path().apply {
                            moveTo(w * 0.5f, h * 0.06f)
                            lineTo(w * 0.97f, h * 0.9f)
                            lineTo(w * 0.03f, h * 0.9f)
                            close()
                        }
                        drawPath(triangle, ink)
                        drawLine(fill, Offset(w * 0.5f, h * 0.38f), Offset(w * 0.5f, h * 0.6f), 1.6.dp.toPx(), StrokeCap.Round)
                        drawCircle(fill, 0.9.dp.toPx(), Offset(w * 0.5f, h * 0.75f))
                    } else {
                        val tick = Path().apply {
                            moveTo(w * 0.18f, h * 0.52f)
                            lineTo(w * 0.4f, h * 0.74f)
                            lineTo(w * 0.82f, h * 0.3f)
                        }
                        drawPath(tick, ink, style = Stroke(2.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
                    }
                },
            )
        }
        Text(text, style = Label.copy(fontWeight = FontWeight.Bold), color = ink, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun WorstReadout(worst: WorstPart) {
    val c = LocalDashPalette.current
    Row {
        Text("WORST", Modifier.alignByBaseline(), style = Label.copy(fontSize = 10.sp, letterSpacing = 1.sp), color = c.dis, maxLines = 1)
        Spacer(Modifier.width(6.dp))
        Text(worst.name, Modifier.alignByBaseline(), style = BodySmall, color = c.med, maxLines = 1)
        Spacer(Modifier.width(5.dp))
        Text(
            percentText(worst.value),
            Modifier.alignByBaseline(),
            style = Body.copy(fontWeight = FontWeight.Bold),
            color = worst.color,
            maxLines = 1,
        )
    }
}

/** Landscape: the fault chip, what was checked, and the worst part. */
@Composable
private fun DamageStatusCard(faults: FaultSummary, worst: WorstPart?, modifier: Modifier) {
    val c = LocalDashPalette.current
    DashCard(modifier, cached = true) {
        FaultChip(faults)
        val checked = when {
            faults.failed.isEmpty() -> faults.ok.joinToString(" · ")
            faults.ok.isEmpty() -> ""
            else -> faults.ok.joinToString(" · ") + " OK"
        }
        if (checked.isNotEmpty()) {
            Text(checked, Modifier.padding(top = 4.dp), style = BodySmall, color = c.dis, maxLines = 2)
        }
        if (worst != null) {
            HorizontalDivider(Modifier.padding(vertical = 6.dp), color = MaterialTheme.colorScheme.outlineVariant)
            WorstReadout(worst)
        }
    }
}

/**
 * Portrait: the car nose-up with the front wings, wheels, sidepods and floor
 * either side of it, each level with its part, and the rear wing and diffuser
 * under it. Fills the height it is given.
 */
@Composable
private fun CarDamageCard(
    damage: CarDamageState,
    drawn: () -> CarDamageState,
    wireframe: CarWireframe?,
    modifier: Modifier,
) {
    val (fl, fr, rl, rr) = wheels(damage)
    DashCard(modifier, cached = true) {
        BoxWithConstraints(Modifier.fillMaxWidth().weight(1f)) {
            val bounds = wireframe?.bounds
            // Height over width of the drawing; the car's nose-up proportions.
            val aspect = if (bounds == null || bounds.width() <= 0f) 2.66f else bounds.height() / bounds.width()
            val gap = 8.dp
            val carWidth = minOf(maxWidth - (WheelCalloutWidth + gap) * 2, maxHeight / aspect).coerceAtLeast(0.dp)
            val carHeight = carWidth * aspect
            val carTop = (maxHeight - carHeight) / 2
            // Where along the car each part sits, in the drawing's units.
            fun at(y: Float): Float = if (bounds == null) 0f else ((y - bounds.top) / bounds.height()).coerceIn(0f, 1f)
            Row(Modifier.fillMaxSize()) {
                CalloutStack(
                    PortraitLeaders.filter { it.side == LeaderSide.LEFT }.map { at(it.y) },
                    carTop,
                    carHeight,
                    vertical = true,
                    alignEnd = true,
                    modifier = Modifier.weight(1f).fillMaxHeight(),
                ) {
                    PartCallout("Wing L", damage.wingFl, Alignment.End)
                    WheelCallout(fl, valueFirst = true)
                    PartCallout("Sidepods", damage.sidepods, Alignment.End)
                    WheelCallout(rl, valueFirst = true)
                }
                CarDrawing(
                    wireframe,
                    drawn,
                    PortraitLeaders,
                    Modifier.align(Alignment.CenterVertically).padding(horizontal = gap).width(carWidth).height(carHeight),
                )
                CalloutStack(
                    PortraitLeaders.filter { it.side == LeaderSide.RIGHT }.map { at(it.y) },
                    carTop,
                    carHeight,
                    vertical = true,
                    alignEnd = false,
                    modifier = Modifier.weight(1f).fillMaxHeight(),
                ) {
                    PartCallout("Wing R", damage.wingFr, Alignment.Start)
                    WheelCallout(fr, valueFirst = false)
                    PartCallout("Floor", damage.floor, Alignment.Start)
                    WheelCallout(rr, valueFirst = false)
                }
            }
        }
        Row(
            Modifier.fillMaxWidth().padding(top = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(20.dp, Alignment.CenterHorizontally),
        ) {
            InlinePart("Rear wing", damage.wingRear)
            InlinePart("Diffuser", damage.diffuser)
        }
    }
}

/**
 * Landscape: the car nose-right with the driver's left side on top, as on the
 * desktop. The left wheels, floor and wing sit above it, the right ones and
 * the sidepods below, each over its part; the rear wing and diffuser behind it.
 */
@Composable
private fun WideCarDamageCard(
    damage: CarDamageState,
    drawn: () -> CarDamageState,
    wireframe: CarWireframe?,
    modifier: Modifier,
) {
    val (fl, fr, rl, rr) = wheels(damage)
    DashCard(modifier, cached = true) {
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val bounds = wireframe?.bounds
            // Turned nose-right, the drawing's length becomes its width.
            val aspect = if (bounds == null || bounds.width() <= 0f) 2.66f else bounds.height() / bounds.width()
            val tail = 64.dp
            val gap = 8.dp
            // Room above and below for a wheel callout.
            val band = 56.dp
            val carWidth = minOf(maxWidth - tail - gap, (maxHeight - (band + gap) * 2) * aspect).coerceAtLeast(0.dp)
            val carHeight = carWidth / aspect
            // Across the page, rear to nose, in the drawing's units.
            fun along(y: Float): Float = if (bounds == null) 0f else (1f - (y - bounds.top) / bounds.height()).coerceIn(0f, 1f)
            // The wings' callouts sit a little toward the nose, clear of the front wheels'.
            val wingShift = if (carWidth > 0.dp) 16.dp / carWidth else 0f
            fun fractions(side: LeaderSide) = LandscapeLeaders.filter { it.side == side }
                .map { along(it.y) + if (it.zone.startsWith("z-fw")) wingShift else 0f }
            Row(Modifier.fillMaxSize(), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.width(tail).height(carHeight), verticalArrangement = Arrangement.SpaceAround) {
                    PartCallout("Rear wing", damage.wingRear, Alignment.Start)
                    PartCallout("Diffuser", damage.diffuser, Alignment.Start)
                }
                Column(Modifier.padding(start = gap).width(carWidth).fillMaxHeight()) {
                    CalloutStack(
                        fractions(LeaderSide.TOP),
                        0.dp,
                        carWidth,
                        vertical = false,
                        alignEnd = true,
                        modifier = Modifier.fillMaxWidth().weight(1f),
                    ) {
                        WheelCallout(rl, valueFirst = false)
                        PartCallout("Floor", damage.floor, Alignment.CenterHorizontally)
                        WheelCallout(fl, valueFirst = false)
                        PartCallout("Wing L", damage.wingFl, Alignment.CenterHorizontally)
                    }
                    CarDrawing(
                        wireframe,
                        drawn,
                        LandscapeLeaders,
                        Modifier.padding(vertical = gap).width(carWidth).height(carHeight),
                        noseRight = true,
                    )
                    CalloutStack(
                        fractions(LeaderSide.BOTTOM),
                        0.dp,
                        carWidth,
                        vertical = false,
                        alignEnd = false,
                        modifier = Modifier.fillMaxWidth().weight(1f),
                    ) {
                        WheelCallout(rr, valueFirst = false)
                        PartCallout("Sidepods", damage.sidepods, Alignment.CenterHorizontally)
                        WheelCallout(fr, valueFirst = false)
                        PartCallout("Wing R", damage.wingFr, Alignment.CenterHorizontally)
                    }
                }
            }
        }
    }
}

/**
 * Callouts level with their parts, always inside the stack: each is centred
 * on its share of the car along the stack's axis, pushed past the one before
 * and kept inside the far end. When they cannot all fit at full size they
 * shrink together. [alignEnd] puts them against the right edge of a vertical
 * stack, or the bottom of a horizontal one.
 */
@Composable
private fun CalloutStack(
    fractions: List<Float>,
    start: Dp,
    span: Dp,
    vertical: Boolean,
    alignEnd: Boolean,
    modifier: Modifier,
    content: @Composable () -> Unit,
) {
    Layout(content, modifier) { measurables, constraints ->
        val placeables = measurables.map { it.measure(constraints.copy(minWidth = 0, minHeight = 0)) }
        val length = if (vertical) constraints.maxHeight else constraints.maxWidth
        val lengths = placeables.map { if (vertical) it.height else it.width }
        val natural = lengths.sum() + 6.dp.roundToPx() * (placeables.size - 1)
        val scale = if (natural > length && natural > 0) length.toFloat() / natural else 1f
        val gap = 6.dp.toPx() * scale
        val sizes = lengths.map { it * scale }
        val first = start.toPx()
        val extent = span.toPx()
        val leads = FloatArray(placeables.size)
        var end = -gap
        for (index in placeables.indices) {
            val wanted = first + fractions.getOrElse(index) { 1f } * extent - sizes[index] / 2f
            leads[index] = max(max(wanted, 0f), end + gap)
            end = leads[index] + sizes[index]
        }
        var limit = length.toFloat()
        for (index in placeables.indices.reversed()) {
            leads[index] = min(leads[index], limit - sizes[index])
            limit = leads[index] - gap
        }
        layout(constraints.maxWidth, constraints.maxHeight) {
            placeables.forEachIndexed { index, placeable ->
                val lead = leads[index].roundToInt()
                val cross = when {
                    !alignEnd -> 0
                    vertical -> constraints.maxWidth - placeable.width
                    else -> constraints.maxHeight - placeable.height
                }
                val x = if (vertical) cross else lead
                val y = if (vertical) lead else cross
                if (scale < 1f) {
                    placeable.placeWithLayer(x, y) {
                        scaleX = scale
                        scaleY = scale
                        val far = if (alignEnd) 1f else 0f
                        transformOrigin = if (vertical) TransformOrigin(far, 0f) else TransformOrigin(0f, far)
                    }
                } else {
                    placeable.place(x, y)
                }
            }
        }
    }
}

/** A body part: its name over its damage. */
@Composable
private fun PartCallout(name: String, value: Int?, align: Alignment.Horizontal) {
    val c = LocalDashPalette.current
    Column(horizontalAlignment = align) {
        Text(name, style = BodySmall, color = c.med, maxLines = 1)
        Text(percent(value), style = PartValue, color = partColor(c, value), maxLines = 1)
    }
}

/** A body part on one line, under the car in portrait. */
@Composable
private fun InlinePart(name: String, value: Int?) {
    val c = LocalDashPalette.current
    Row {
        Text(name, Modifier.alignByBaseline(), style = BodySmall, color = c.med, maxLines = 1)
        Spacer(Modifier.width(6.dp))
        Text(percent(value), Modifier.alignByBaseline(), style = PartValue, color = partColor(c, value), maxLines = 1)
    }
}

/** A wheel: tyre damage large beside its name, brake and blisters under it. */
@Composable
private fun WheelCallout(wheel: WheelDamage, valueFirst: Boolean) {
    Column(Modifier.width(WheelCalloutWidth)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            if (valueFirst) {
                WheelTyre(wheel.tyre)
                WheelName(wheel.name)
            } else {
                WheelName(wheel.name)
                WheelTyre(wheel.tyre)
            }
        }
        WheelLine("Brake", wheel.brake)
        WheelLine("Blisters", wheel.blisters)
    }
}

@Composable
private fun RowScope.WheelName(name: String) {
    val c = LocalDashPalette.current
    Text(
        name,
        Modifier.alignByBaseline(),
        style = Label.copy(fontWeight = FontWeight.Bold, letterSpacing = 1.sp),
        color = c.med,
        maxLines = 1,
    )
}

@Composable
private fun RowScope.WheelTyre(value: Int?) {
    val c = LocalDashPalette.current
    Text(
        percent(value),
        Modifier.alignByBaseline(),
        style = Title.copy(fontSize = 21.sp, lineHeight = 23.sp, fontWeight = FontWeight.Bold),
        color = tyreColor(c, value),
        maxLines = 1,
    )
}

@Composable
private fun WheelLine(label: String, value: Int?) {
    val c = LocalDashPalette.current
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        val style = BodySmall.copy(fontSize = 11.sp, lineHeight = 14.sp)
        Text(label, style = style, color = c.dis, maxLines = 1)
        Text(value?.toString() ?: "—", style = style.copy(fontWeight = FontWeight.Bold), color = tyreColor(c, value), maxLines = 1)
    }
}

/**
 * The wireframe with damaged parts outlined and tinted as on the desktop: a
 * body part once it has any damage, a tyre always, in the band of its worst
 * tyre, brake or blister value. Each leader runs from just outside the car to
 * its part, in that part's colour once marked. [damage] is read while drawing.
 */
@Composable
private fun CarDrawing(
    wireframe: CarWireframe?,
    damage: () -> CarDamageState,
    leaders: List<Leader>,
    modifier: Modifier,
    noseRight: Boolean = false,
) {
    val c = LocalDashPalette.current
    val card = MaterialTheme.colorScheme.surfaceContainer
    Spacer(
        modifier.semantics { contentDescription = "Car damage by part" }.drawWithCache {
            val car = wireframe ?: return@drawWithCache onDrawBehind {}
            // Nose-right is a quarter turn clockwise: the driver's left goes on top.
            val matrix = android.graphics.Matrix()
            if (noseRight) matrix.postRotate(90f)
            val turned = android.graphics.RectF(car.bounds).also { matrix.mapRect(it) }
            val scale = min(size.width / turned.width(), size.height / turned.height())
            matrix.postTranslate(-turned.left, -turned.top)
            matrix.postScale(scale, scale)
            matrix.postTranslate((size.width - turned.width() * scale) / 2f, (size.height - turned.height() * scale) / 2f)
            val paths = car.parts.map { part -> android.graphics.Path(part.path).apply { transform(matrix) }.asComposePath() }
            val anchors = leaders.map { leader ->
                val point = floatArrayOf(leader.x, leader.y).also { matrix.mapPoints(it) }
                Offset(point[0], point[1])
            }
            val reach = 6.dp.toPx()
            val starts = leaders.mapIndexed { index, leader ->
                val anchor = anchors[index]
                when (leader.side) {
                    LeaderSide.LEFT -> Offset(-reach, anchor.y)
                    LeaderSide.RIGHT -> Offset(size.width + reach, anchor.y)
                    LeaderSide.TOP -> Offset(anchor.x, -reach)
                    LeaderSide.BOTTOM -> Offset(anchor.x, size.height + reach)
                }
            }
            val wire = Stroke(1.2.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round)
            val marked = Stroke(2.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round)
            val detail = Stroke(0.7.dp.toPx(), cap = StrokeCap.Round)
            val tube = Stroke(0.9.dp.toPx(), cap = StrokeCap.Round)
            val line = 1.dp.toPx()
            val dot = 2.4.dp.toPx()
            onDrawBehind {
                val zones = zoneColors(c, damage())
                car.parts.forEachIndexed { index, part ->
                    val tone = part.zone?.let(zones::get)
                    val path = paths[index]
                    when (part.kind) {
                        CarWireframe.Kind.SOLID -> {
                            drawPath(path, if (tone != null) lerp(card, tone, 0.24f) else card)
                            drawPath(path, tone ?: c.wire, style = if (tone != null) marked else wire)
                        }
                        CarWireframe.Kind.DETAIL -> drawPath(path, (tone ?: c.wire).copy(alpha = 0.6f), style = detail)
                        CarWireframe.Kind.TUBE -> drawPath(path, c.tube, style = tube)
                    }
                }
                leaders.forEachIndexed { index, leader ->
                    val tone = zones[leader.zone]
                    drawLine(tone ?: c.leader, starts[index], anchors[index], line)
                    drawCircle(tone ?: c.leaderDot, dot, anchors[index])
                }
            }
        },
    )
}

/** Each damage zone's colour, or none for a part drawn plain. */
private fun zoneColors(c: DashPalette, damage: CarDamageState): Map<String, Color> = buildMap {
    // Body parts are marked from the first percent of damage, as on the desktop.
    fun body(zone: String, value: Int?) {
        if (value != null && value > 0) damageColor(c, value)?.let { put(zone, it) }
    }
    body("z-fw-l", damage.wingFl)
    body("z-fw-r", damage.wingFr)
    body("z-rw", damage.wingRear)
    body("z-floor", damage.floor)
    body("z-sp-l", damage.sidepods)
    body("z-sp-r", damage.sidepods)
    body("z-diff", damage.diffuser)
    // Tyres always take the band of their worst value, green included.
    fun tyre(zone: String, vararg values: Int?) {
        values.filterNotNull().maxOrNull()?.let { put(zone, tyreBandColor(c, it)) }
    }
    tyre("t-fl", damage.tyreFl, damage.brakeFl, damage.blistersFl)
    tyre("t-fr", damage.tyreFr, damage.brakeFr, damage.blistersFr)
    tyre("t-rl", damage.tyreRl, damage.brakeRl, damage.blistersRl)
    tyre("t-rr", damage.tyreRr, damage.brakeRr, damage.blistersRr)
}

/** Power-unit and gearbox wear as bars, with ticks where their colour changes. */
@Composable
private fun WearCard(damage: CarDamageState, columns: Int, modifier: Modifier) {
    val c = LocalDashPalette.current
    val tiles = wearTiles(damage)
    DashCard(modifier, cached = true) {
        Row {
            Text("Power unit", Modifier.alignByBaseline(), style = Label, color = c.med, maxLines = 1)
            Text(" wear", Modifier.alignByBaseline(), style = Label.copy(fontWeight = FontWeight.Normal), color = c.dis, maxLines = 1)
        }
        Spacer(Modifier.height(4.dp))
        if (columns == 1) {
            // Spread over the card's height.
            Column(Modifier.fillMaxWidth().weight(1f)) {
                for ((name, value) in tiles) WearRow(name, value, Modifier.fillMaxWidth().weight(1f))
            }
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                for (row in tiles.chunked(columns)) {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                        for ((name, value) in row) WearRow(name, value, Modifier.weight(1f).height(20.dp))
                    }
                }
            }
        }
    }
}

private fun wearTiles(damage: CarDamageState): List<Pair<String, Int?>> = listOf(
    "Engine" to damage.engine,
    "Gearbox" to damage.gearbox,
    "ICE" to damage.iceWear,
    "MGU-H" to damage.mguhWear,
    "MGU-K" to damage.mgukWear,
    "ES" to damage.esWear,
    "CE" to damage.ceWear,
    "TC" to damage.tcWear,
)

@Composable
private fun WearRow(name: String, value: Int?, modifier: Modifier) {
    val c = LocalDashPalette.current
    val tone = wearColor(c, value)
    // Below the first threshold the bar is grey, like the text is plain.
    val bar = if (value != null && value >= 50) tone else c.dis
    val track = MaterialTheme.colorScheme.surfaceContainerHighest
    Row(modifier, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(name, Modifier.width(54.dp), style = BodySmall, color = c.med, maxLines = 1)
        Spacer(
            Modifier.weight(1f).height(6.dp).drawBehind {
                val corner = CornerRadius(size.height / 2f)
                drawRoundRect(track, cornerRadius = corner)
                val tick = 1.dp.toPx()
                for (mark in floatArrayOf(0.5f, 0.75f)) {
                    drawRect(c.wearTick, Offset(size.width * mark - tick / 2f, 0f), Size(tick, size.height))
                }
                if (value != null && value > 0) {
                    drawRoundRect(bar, size = Size(size.width * (value / 100f).coerceIn(0f, 1f), size.height), cornerRadius = corner)
                }
            },
        )
        Text(
            percentText(value),
            Modifier.width(38.dp),
            style = BodySmall.copy(fontWeight = FontWeight.Bold),
            color = tone,
            maxLines = 1,
            textAlign = TextAlign.End,
        )
    }
}

// ── Frame sampling ──────────────────────────────────────────────────────────

/**
 * Stable holder whose fields are independent Compose states. The dashboard
 * passes this holder without reading it, so a telemetry tick invalidates only
 * what reads a changed field, and the painted readouts only redraw.
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
    var aeroOpen by mutableStateOf(false)
        private set
    var slmCar by mutableStateOf(false)
        private set
    var revLightsBitValue by mutableStateOf(initialHot.revLightsBitValue)
        private set
    private val surfaces = Array(4) { mutableIntStateOf(0) }
    private val inners = Array(4) { mutableIntStateOf(0) }
    private val wears = Array(4) { mutableStateOf<Int?>(null) }

    fun surface(corner: Int): Int = surfaces[corner].intValue
    fun inner(corner: Int): Int = inners[corner].intValue
    fun wear(corner: Int): Int? = wears[corner].value

    var cold by mutableStateOf(DashboardColdState())
        private set
    var lapComparison by mutableStateOf(DashboardLapComparisonState())
        private set

    // Cold rows arrive several times a second, mostly carrying the running lap
    // time. Each readout reads only its own fields.
    var position by mutableIntStateOf(0)
        private set
    var lapText by mutableStateOf("Lap –")
        private set
    var tyreChipText by mutableStateOf("–")
        private set
    var tyreCompound by mutableIntStateOf(0)
        private set
    var statusAvailable by mutableStateOf(false)
        private set
    var ersPercent by mutableIntStateOf(0)
        private set
    var ersMode by mutableIntStateOf(0)
        private set
    var fuelLaps by mutableDoubleStateOf(0.0)
        private set
    var lastLapMs by mutableIntStateOf(0)
        private set
    var currentLapMs by mutableIntStateOf(0)
        private set
    var fastestLapMs by mutableIntStateOf(0)
        private set
    var sectorDeltas by mutableStateOf<List<Double?>>(listOf(null, null, null))
        private set

    /** Delta to the fastest lap, rounded to the displayed millisecond. */
    var lapDeltaSeconds by mutableStateOf<Double?>(null)
        private set

    // Derived on the phone.
    var startPosition by mutableIntStateOf(0)
        private set
    var burnPerLapKg by mutableStateOf<Double?>(null)
        private set
    var fuelTargetText by mutableStateOf("")
        private set
    var aheadTrendMs by mutableStateOf<Int?>(null)
        private set
    var behindTrendMs by mutableStateOf<Int?>(null)
        private set
    var penaltyText by mutableStateOf<String?>(null)
        private set
    var pitText by mutableStateOf("")
        private set

    val placesGained: Int get() = if (startPosition > 0 && position > 0) startPosition - position else 0


    private var lastHot = initialHot
    private var lastCold = cold
    private var lastLapComparison = lapComparison
    private var lastTiming: TimingTowerState? = null
    private var lapNumber = 0
    private var fuelAtLapStart = Double.NaN
    private var aheadAtLapStart: Int? = null
    private var behindAtLapStart: Int? = null

    fun update(
        hot: HotTelemetry,
        latestCold: DashboardColdState,
        latestLapComparison: DashboardLapComparisonState,
        timing: TimingTowerState,
        driverIndex: Int,
    ) {
        if (hot !== lastHot) {
            speedKph = hot.speedKph
            rpm = hot.rpm
            gear = hot.gear
            throttle = hot.throttle
            brake = hot.brake
            aeroOpen = hot.drs != 0 || hot.slm != 0
            revLightsBitValue = hot.revLightsBitValue
            surfaces[0].intValue = hot.tyreSurfaceFl
            surfaces[1].intValue = hot.tyreSurfaceFr
            surfaces[2].intValue = hot.tyreSurfaceRl
            surfaces[3].intValue = hot.tyreSurfaceRr
            inners[0].intValue = hot.tyreInnerFl
            inners[1].intValue = hot.tyreInnerFr
            inners[2].intValue = hot.tyreInnerRl
            inners[3].intValue = hot.tyreInnerRr
            lastHot = hot
        }
        if (latestCold !== lastCold) {
            cold = latestCold
            lastCold = latestCold
            updateCold(latestCold, timing, driverIndex)
        }
        if (latestLapComparison !== lastLapComparison) {
            lapComparison = latestLapComparison
            lastLapComparison = latestLapComparison
            fastestLapMs = latestLapComparison.fastestLapMs
            sectorDeltas = listOf(
                latestLapComparison.sector1DeltaSeconds,
                latestLapComparison.sector2DeltaSeconds,
                latestLapComparison.sector3DeltaSeconds,
            ).map { it?.takeIf { value -> value.isFinite() } }
            val delta = latestLapComparison.lapDeltaSeconds?.takeIf { it.isFinite() }?.let { kotlin.math.round(it * 1000) / 1000 }
            lapDeltaSeconds = delta
        }
        if (timing !== lastTiming) {
            lastTiming = timing
            updatePenalties(timing, driverIndex)
        }
    }

    private fun updateCold(latest: DashboardColdState, timing: TimingTowerState, driverIndex: Int) {
        position = latest.position
        lapText = lapLabel(latest)
        tyreChipText = tyreChipLabel(latest)
        tyreCompound = latest.tyreCompound
        statusAvailable = latest.statusAvailable
        ersPercent = latest.ersPercent
        ersMode = latest.ersMode
        fuelLaps = latest.fuelLaps
        lastLapMs = latest.lastLapMs
        currentLapMs = latest.currentLapMs
        slmCar = latest.aeroMode == "slm"
        val wearKnown = latest.tyreWearAvailable
        wears[0].value = if (wearKnown) latest.tyreWearFl.toInt() else null
        wears[1].value = if (wearKnown) latest.tyreWearFr.toInt() else null
        wears[2].value = if (wearKnown) latest.tyreWearRl.toInt() else null
        wears[3].value = if (wearKnown) latest.tyreWearRr.toInt() else null

        // Places gained: the position the race started from.
        val raceSession = latest.sessionType in RaceSessionTypes
        if (!raceSession || latest.lapNumber < lapNumber) startPosition = 0
        if (raceSession && startPosition == 0 && latest.lapNumber == 1 && latest.position > 0) startPosition = latest.position

        if (latest.lapNumber != lapNumber) {
            val completed = latest.lapNumber == lapNumber + 1 && lapNumber > 0
            // Fuel burnt over the lap just completed.
            if (completed && fuelAtLapStart.isFinite() && latest.fuelKg > 0) {
                burnPerLapKg = (fuelAtLapStart - latest.fuelKg).takeIf { it > 0.05 }
            }
            fuelAtLapStart = latest.fuelKg
            // Gap trends: the change in each interval over the lap just completed.
            val gaps = raceGaps(timing, driverIndex)
            if (completed) {
                aheadTrendMs = trend(aheadAtLapStart, gaps.aheadMs)
                behindTrendMs = trend(behindAtLapStart, gaps.behindMs)
            } else {
                aheadTrendMs = null
                behindTrendMs = null
            }
            aheadAtLapStart = gaps.aheadMs
            behindAtLapStart = gaps.behindMs
            lapNumber = latest.lapNumber
        }
        fuelTargetText = fuelTarget(latest, burnPerLapKg)
    }

    private fun updatePenalties(timing: TimingTowerState, driverIndex: Int) {
        val me: TimingCarEntry? = timing.cars.firstOrNull { it.index == driverIndex }
        penaltyText = me?.let {
            buildList {
                if (it.penaltiesSeconds > 0) add("+${it.penaltiesSeconds} s")
                if (it.driveThroughPenalties > 0) add("Drive-through")
                if (it.stopGoPenalties > 0) add("Stop-go")
            }.joinToString(" · ").ifEmpty { null }
        }
        pitText = when (me?.pitStatus) {
            1 -> "Pitting"
            2 -> "In the pit lane"
            else -> ""
        }
    }

    private fun trend(start: Int?, now: Int?): Int? =
        if (start == null || now == null) null else now - start
}

private fun fuelTarget(cold: DashboardColdState, burnPerLapKg: Double?): String {
    if (!cold.statusAvailable) return ""
    val margin = cold.fuelLaps
    return when {
        margin > 1.0 -> "Can push"
        margin >= 0.0 -> "On target"
        burnPerLapKg == null || cold.totalLaps <= 0 -> "Fuel short"
        else -> {
            val lapsLeft = (cold.totalLaps - cold.lapNumber + 1).coerceAtLeast(1)
            "Save %.2f kg/lap".format(-margin * burnPerLapKg / lapsLeft)
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
                store.timing,
                store.selectedDriverIndex,
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
private fun rememberDashboardAlert(frame: DashboardFrameState, colors: DashPalette): State<DashboardAlert?> {
    val alert = remember(frame) { mutableStateOf<DashboardAlert?>(null) }
    // The collector outlives a theme change; it colours each alert as raised.
    val palette = rememberUpdatedState(colors)
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
                alertFor(palette.value, it, cold, previousBest, best, statusSettled)
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
    c: DashPalette,
    previous: DashboardColdState,
    cold: DashboardColdState,
    previousBestMs: Int,
    bestMs: Int,
    statusSettled: Boolean,
): Pair<String, Color>? {
    val statusSteady = statusSettled && previous.statusAvailable
    return when {
        cold.lapNumber > 0 && previous.lapNumber == cold.lapNumber &&
            !previous.lapInvalid && cold.lapInvalid -> "Lap invalid" to c.loss
        previousBestMs > 0 && bestMs in 1 until previousBestMs ->
            "Best lap · ${formatTime(bestMs)}" to c.best
        cold.lapNumber > previous.lapNumber && previous.lapNumber > 0 && cold.lastLapMs > 0 ->
            "Lap · ${formatTime(cold.lastLapMs)}" to c.hi
        statusSteady && previous.brakeBias != cold.brakeBias ->
            "Brake bias · ${cold.brakeBias}%" to c.cold
        statusSteady && previous.fuelLaps >= 0.0 && cold.fuelLaps < 0.0 ->
            "Fuel short" to c.loss
        else -> null
    }
}

// ── Race gaps ───────────────────────────────────────────────────────────────

@Immutable
private data class RaceGaps(
    val aheadIndex: Int?,
    val aheadName: String?,
    val aheadCode: String?,
    val aheadMs: Int?,
    val behindIndex: Int?,
    val behindName: String?,
    val behindCode: String?,
    val behindMs: Int?,
    val leading: Boolean,
    val last: Boolean,
)

@Composable
private fun rememberRaceGaps(store: TelemetryStore): State<RaceGaps> = remember(store) {
    derivedStateOf(structuralEqualityPolicy()) { raceGaps(store.timing, store.selectedDriverIndex) }
}

private fun raceGaps(timing: TimingTowerState, driverIndex: Int): RaceGaps {
    val cars = timing.cars
    val me = cars.firstOrNull { it.index == driverIndex && it.position > 0 }
        ?: return RaceGaps(null, null, null, null, null, null, null, null, leading = false, last = false)
    val ahead = cars.firstOrNull { it.position == me.position - 1 }
    val behind = cars.firstOrNull { it.position == me.position + 1 }
    fun name(index: Int?) = index?.let { timing.drivers[it]?.name }
    fun code(index: Int?) = name(index)?.let { driverCode(it, index ?: 0) }
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
        aheadIndex = ahead?.index,
        aheadName = name(ahead?.index),
        aheadCode = code(ahead?.index),
        aheadMs = aheadMs,
        behindIndex = behind?.index,
        behindName = name(behind?.index),
        behindCode = code(behind?.index),
        behindMs = behindMs,
        leading = me.position == 1,
        last = behind == null,
    )
}

/** A driver's three-letter code from their name. */
private fun driverCode(name: String, index: Int): String {
    val surname = name.trim().split(' ').lastOrNull().orEmpty()
    return surname.take(3).uppercase().ifEmpty { "#$index" }
}

// ── Colour ──────────────────────────────────────────────────────────────────

private fun deltaColor(c: DashPalette, seconds: Double): Color = when {
    !seconds.isFinite() || abs(seconds) < 0.0005 -> c.hi
    seconds > 0 -> c.loss
    else -> c.gain
}

private fun ersColor(c: DashPalette, percent: Int): Color = if (percent < 20) c.caution else c.cold

/** The desktop Damage page's five tyre bands, 20% each. */
internal fun tyreBandColor(c: DashPalette, percent: Int): Color = c.tyreBands[(percent / 20).coerceIn(0, 4)]

private fun tyreTemperatureColor(c: DashPalette, value: Int): Color = when {
    value <= 0 -> c.med
    value < 80 -> c.cold
    value <= 105 -> c.gain
    value <= 120 -> c.caution
    else -> c.loss
}

private fun fuelMarginColor(c: DashPalette, fuelLaps: Double): Color = when {
    fuelLaps > 1.0 -> c.hi
    fuelLaps >= 0.0 -> c.caution
    else -> c.loss
}

private fun compoundColor(c: DashPalette, compound: Int): Color? = when (compound) {
    16 -> c.loss
    17 -> c.caution
    18 -> c.hard
    7 -> c.gain
    8 -> c.cold
    else -> null
}

/** The compounds the status row shows a chip for. */
private val ShownCompounds = setOf(7, 8, 16, 17, 18)

/** The game's ERS deploy modes. */
private fun ersModeLabel(mode: Int): String = when (mode) {
    0 -> "None"
    1 -> "Medium"
    2 -> "Hotlap"
    3 -> "Overtake"
    else -> "–"
}

// ── Formatting ──────────────────────────────────────────────────────────────

private fun lapLabel(cold: DashboardColdState): String = when {
    cold.lapNumber <= 0 -> "Lap –"
    cold.totalLaps > 0 -> "Lap ${cold.lapNumber} / ${cold.totalLaps}"
    else -> "Lap ${cold.lapNumber}"
}

private fun compoundName(compound: Int): String = when (compound) {
    7 -> "Inter"
    8 -> "Wet"
    16 -> "C5"
    17 -> "C4"
    18 -> "C3"
    19 -> "C2"
    20 -> "C1"
    21 -> "C0"
    22 -> "C6"
    else -> "Tyre"
}

private fun tyreChipLabel(cold: DashboardColdState): String = when {
    !cold.statusAvailable || cold.tyreCompound !in ShownCompounds -> "–"
    else -> "${compoundName(cold.tyreActualCompound)} · ${cold.tyreAgeLaps} lap${if (cold.tyreAgeLaps == 1) "" else "s"}"
}

private fun formatTime(milliseconds: Int): String {
    if (milliseconds <= 0) return "–"
    val minutes = milliseconds / 60_000
    val seconds = milliseconds / 1_000 % 60
    val millis = milliseconds % 1_000
    return "%d:%02d.%03d".format(minutes, seconds, millis)
}

/** The running lap to the tenth. */
private fun formatRunning(milliseconds: Int): String {
    if (milliseconds <= 0) return "–"
    val minutes = milliseconds / 60_000
    val seconds = milliseconds / 1_000 % 60
    val tenths = milliseconds % 1_000 / 100
    return "%d:%02d.%d".format(minutes, seconds, tenths)
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
