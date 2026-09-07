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
import androidx.compose.material3.Card
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
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
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.tracknrace.android.DashboardColdState
import com.tracknrace.android.HotTelemetry
import com.tracknrace.android.TelemetryStore
import kotlin.math.roundToInt

private val DashboardBackground = Color(0xff07090c)
private val DashboardCard = Color(0xff10151b)
private val DashboardDivider = Color(0xff26313b)
private val DashboardPrimary = Color.White
private val DashboardSecondary = Color(0xff8f9aa6)

@Composable
internal fun DashboardScreen(
    store: TelemetryStore,
    active: Boolean = true,
    cold: DashboardColdState = store.cold,
) {
    val hot = rememberFrameSample(store, active)
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
                    RpmLights(hot.revLightsBitValue, Modifier.fillMaxSize())
                }
                HorizontalDashboardDivider()
                LandscapeDashboard(hot, cold, wideLandscape, Modifier.weight(1f))
            } else {
                Card(modifier = Modifier.fillMaxWidth().weight(1f)) {
                    Box(
                        Modifier.fillMaxWidth().height(26.dp)
                            .padding(horizontal = 10.dp, vertical = 4.dp),
                    ) {
                        RpmLights(hot.revLightsBitValue, Modifier.fillMaxSize())
                    }
                    HorizontalDashboardDivider()
                    PortraitDashboard(hot, cold, Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
private fun rememberFrameSample(store: TelemetryStore, active: Boolean): HotTelemetry {
    var displayed by remember(store) { mutableStateOf(store.latestHot()) }
    LaunchedEffect(store, active) {
        if (!active) return@LaunchedEffect
        while (true) {
            withFrameNanos { }
            val latest = store.latestHot()
            if (latest != displayed) displayed = latest
        }
    }
    return displayed
}

@Composable
private fun RpmLights(bitValue: Int?, modifier: Modifier = Modifier) {
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
    hot: HotTelemetry,
    cold: DashboardColdState,
    unframed: Boolean,
    modifier: Modifier,
) {
    Row(modifier.fillMaxWidth()) {
        Column(
            Modifier.weight(1f).fillMaxHeight()
                .background(if (unframed) Color.Transparent else DashboardCard),
        ) {
            ValueCell(lapLabel(cold), DashboardPrimary, Modifier.weight(0.54f))
            HorizontalDashboardDivider()
            EmptyDashboardCell(Modifier.weight(0.96f))
            HorizontalDashboardDivider()
            SideTyreBlock(
                hot = hot,
                cold = cold,
                leftSide = true,
                showDividers = true,
                modifier = Modifier.weight(0.86f),
            )
        }
        VerticalDashboardDivider()
        CenterReadout(
            hot,
            cold,
            Modifier.weight(1.95f).fillMaxHeight()
                .background(if (unframed) Color.Transparent else DashboardCard),
        )
        VerticalDashboardDivider()
        Column(
            Modifier.weight(1f).fillMaxHeight()
                .background(if (unframed) Color.Transparent else DashboardCard),
        ) {
            FuelCell(cold, Modifier.weight(0.54f))
            HorizontalDashboardDivider()
            EmptyDashboardCell(Modifier.weight(0.96f))
            HorizontalDashboardDivider()
            SideTyreBlock(
                hot = hot,
                cold = cold,
                leftSide = false,
                showDividers = true,
                modifier = Modifier.weight(0.86f),
            )
        }
    }
}

@Composable
private fun PortraitDashboard(hot: HotTelemetry, cold: DashboardColdState, modifier: Modifier) {
    Column(modifier.fillMaxWidth()) {
        CenterReadout(hot, cold, Modifier.fillMaxWidth().weight(1.34f))
        HorizontalDashboardDivider()
        Row(Modifier.fillMaxWidth().weight(0.38f)) {
            ValueCell(lapLabel(cold), DashboardPrimary, Modifier.weight(1f).fillMaxHeight())
            VerticalDashboardDivider()
            FuelCell(cold, Modifier.weight(1f).fillMaxHeight())
        }
        HorizontalDashboardDivider()
        Row(Modifier.fillMaxWidth().weight(0.54f)) {
            EmptyDashboardCell(Modifier.weight(1f).fillMaxHeight())
            VerticalDashboardDivider()
            EmptyDashboardCell(Modifier.weight(1f).fillMaxHeight())
        }
        HorizontalDashboardDivider()
        Row(Modifier.fillMaxWidth().weight(0.76f)) {
            SideTyreBlock(hot, cold, leftSide = true, modifier = Modifier.weight(1f).fillMaxHeight())
            VerticalDashboardDivider()
            SideTyreBlock(hot, cold, leftSide = false, modifier = Modifier.weight(1f).fillMaxHeight())
        }
    }
}

@Composable
private fun CenterReadout(hot: HotTelemetry, cold: DashboardColdState, modifier: Modifier = Modifier) {
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
                "${hot.speedKph} KM/H",
                color = DashboardPrimary,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold,
                fontSize = speedSize,
                lineHeight = speedSize,
            )
            Text(
                gearLabel(hot.gear),
                color = DashboardPrimary,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Black,
                fontSize = gearSize,
                lineHeight = gearSize,
            )
            Text(
                "${hot.rpm} RPM",
                color = DashboardSecondary,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.SemiBold,
                fontSize = rpmSize,
            )
        }
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                InputMeter(hot.throttle, Color(0xff32d583), Modifier.weight(1f))
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
                InputMeter(hot.brake, Color(0xffff4d5e), Modifier.weight(1f))
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
private fun SideTyreBlock(
    hot: HotTelemetry,
    cold: DashboardColdState,
    leftSide: Boolean,
    showDividers: Boolean = true,
    modifier: Modifier = Modifier,
) {
    Column(modifier) {
        if (leftSide) {
            TyreDataRow(hot.tyreSurfaceFl, hot.tyreInnerFl, cold.tyreWearFl, cold.tyreWearAvailable, showDividers, Modifier.weight(1f))
            if (showDividers) HorizontalDashboardDivider()
            TyreDataRow(hot.tyreSurfaceRl, hot.tyreInnerRl, cold.tyreWearRl, cold.tyreWearAvailable, showDividers, Modifier.weight(1f))
        } else {
            TyreDataRow(hot.tyreSurfaceFr, hot.tyreInnerFr, cold.tyreWearFr, cold.tyreWearAvailable, showDividers, Modifier.weight(1f))
            if (showDividers) HorizontalDashboardDivider()
            TyreDataRow(hot.tyreSurfaceRr, hot.tyreInnerRr, cold.tyreWearRr, cold.tyreWearAvailable, showDividers, Modifier.weight(1f))
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
