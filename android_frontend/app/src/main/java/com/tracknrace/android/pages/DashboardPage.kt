package com.tracknrace.android.pages

import android.content.res.Configuration
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
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
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.tracknrace.android.DashboardColdState
import com.tracknrace.android.HotTelemetry
import com.tracknrace.android.TelemetryStore

@Composable
internal fun DashboardScreen(
    store: TelemetryStore,
    active: Boolean = true,
    cold: DashboardColdState = store.cold,
) {
    val hot = rememberFrameSample(store, active)
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    Surface(color = Color(0xff07090c), modifier = Modifier.fillMaxSize()) {
        Column(
            Modifier.fillMaxSize().padding(if (landscape) 10.dp else 8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            RpmLights(hot.revLightsBitValue, Modifier.fillMaxWidth().height(18.dp))
            if (landscape) {
                LandscapeDashboard(hot, cold, Modifier.weight(1f))
            } else {
                PortraitDashboard(hot, cold, Modifier.weight(1f))
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
    val off = Color(0xff26313b)
    val green = Color(0xff32d583)
    val red = Color(0xffff4d5e)
    val purple = Color(0xffc875ff)
    Canvas(modifier.semantics { contentDescription = "RPM shift lights" }) {
        val gap = 5.dp.toPx()
        val lightWidth = (size.width - gap * 14) / 15
        val radius = CornerRadius(4.dp.toPx())
        repeat(15) { index ->
            val lit = bitValue != null && bitValue and (1 shl index) != 0
            val active = when {
                index < 5 -> green
                index < 10 -> red
                else -> purple
            }
            drawRoundRect(
                color = if (lit) active else off,
                topLeft = Offset(index * (lightWidth + gap), 0f),
                size = Size(lightWidth, size.height),
                cornerRadius = radius,
            )
        }
    }
}

@Composable
private fun LandscapeDashboard(hot: HotTelemetry, cold: DashboardColdState, modifier: Modifier) {
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Column(Modifier.weight(0.72f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            MetricCard("POSITION", if (cold.position > 0) "P${cold.position}" else "—", Color(0xff43a5ff), Modifier.weight(1f))
            MetricCard("LAP", lapLabel(cold), Color.White, Modifier.weight(1f))
            MetricCard(if (cold.lapInvalid) "CURRENT • INVALID" else "CURRENT LAP", formatTime(cold.currentLapMs), if (cold.lapInvalid) Color(0xffff4d5e) else Color.White, Modifier.weight(1f))
        }
        CenterReadout(hot, cold, Modifier.weight(1.65f).fillMaxHeight())
        Column(Modifier.weight(0.72f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            MetricCard("ERS", "${cold.ersPercent}%", if (cold.ersPercent < 20) Color(0xfff5b942) else Color(0xff32d583), Modifier.weight(1f))
            MetricCard("FUEL", if (cold.fuelLaps > 0) "%.1f LAPS".format(cold.fuelLaps) else "—", Color.White, Modifier.weight(1f))
            MetricCard("LAST LAP", formatTime(cold.lastLapMs), Color.White, Modifier.weight(1f))
        }
    }
}

@Composable
private fun PortraitDashboard(hot: HotTelemetry, cold: DashboardColdState, modifier: Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        CenterReadout(hot, cold, Modifier.fillMaxWidth().weight(1.8f))
        Row(
            Modifier.fillMaxWidth().weight(1f),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            MetricCard("POSITION", if (cold.position > 0) "P${cold.position}" else "—", Color(0xff43a5ff), Modifier.weight(1f).fillMaxHeight())
            MetricCard("LAP", lapLabel(cold), Color.White, Modifier.weight(1f).fillMaxHeight())
        }
        Row(
            Modifier.fillMaxWidth().weight(1f),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            MetricCard("ERS", "${cold.ersPercent}%", Color(0xff32d583), Modifier.weight(1f).fillMaxHeight())
            MetricCard("FUEL", if (cold.fuelLaps > 0) "%.1f LAPS".format(cold.fuelLaps) else "—", Color.White, Modifier.weight(1f).fillMaxHeight())
        }
        Row(
            Modifier.fillMaxWidth().weight(1f),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            MetricCard("CURRENT LAP", formatTime(cold.currentLapMs), if (cold.lapInvalid) Color(0xffff4d5e) else Color.White, Modifier.weight(1f).fillMaxHeight())
            MetricCard("LAST LAP", formatTime(cold.lastLapMs), Color.White, Modifier.weight(1f).fillMaxHeight())
        }
    }
}

@Composable
private fun CenterReadout(hot: HotTelemetry, cold: DashboardColdState, modifier: Modifier = Modifier) {
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val gearSize = if (landscape) 82.sp else 70.sp
    val speedSize = if (landscape) 52.sp else 42.sp
    Card(
        modifier,
        colors = CardDefaults.cardColors(containerColor = Color(0xff10151b)),
        border = CardDefaults.outlinedCardBorder().copy(width = 1.dp),
    ) {
        Column(
            Modifier.fillMaxSize().padding(if (landscape) 16.dp else 12.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.SpaceBetween,
        ) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                DashboardPill(
                    label = cold.labels["ui.overview.drs"] ?: cold.aeroMode.uppercase(),
                    active = if (cold.aeroMode == "slm") hot.slm > 0 else hot.drs > 0,
                )
                DashboardPill(tyreLabel(cold), active = false, tone = tyreTone(cold.tyreCompound))
            }
            Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.Center) {
                Text(
                    gearLabel(hot.gear),
                    color = Color.White,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Black,
                    fontSize = gearSize,
                    lineHeight = gearSize,
                )
                Spacer(Modifier.width(if (landscape) 24.dp else 16.dp))
                Column(horizontalAlignment = Alignment.End) {
                    Text(
                        hot.speedKph.toString(),
                        color = Color.White,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold,
                        fontSize = speedSize,
                        lineHeight = speedSize,
                    )
                    Text(
                        "KM/H  •  ${hot.rpm} RPM",
                        color = Color(0xff8f9aa6),
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 12.sp,
                    )
                }
            }
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                PedalBar("BRK", hot.brake, Color(0xffff4d5e))
                PedalBar("THR", hot.throttle, Color(0xff32d583))
            }
        }
    }
}

@Composable
private fun DashboardPill(label: String, active: Boolean, tone: Color = Color(0xff8f9aa6)) {
    val color = if (active) Color(0xff32d583) else tone
    Surface(color = color.copy(alpha = 0.17f), shape = MaterialTheme.shapes.extraLarge) {
        Text(
            label,
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
            color = color,
            fontWeight = FontWeight.Bold,
            fontSize = 12.sp,
        )
    }
}

@Composable
private fun PedalBar(label: String, value: Float, color: Color) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = Color(0xff8f9aa6), fontWeight = FontWeight.Bold, fontSize = 11.sp, modifier = Modifier.width(34.dp))
        LinearProgressIndicator(
            progress = { value.coerceIn(0f, 1f) },
            modifier = Modifier.fillMaxWidth().height(8.dp).clip(MaterialTheme.shapes.extraLarge),
            color = color,
            trackColor = Color(0xff26313b),
            strokeCap = StrokeCap.Round,
        )
    }
}

@Composable
private fun MetricCard(label: String, value: String, valueColor: Color, modifier: Modifier = Modifier) {
    Card(
        modifier,
        colors = CardDefaults.cardColors(containerColor = Color(0xff10151b)),
    ) {
        Column(
            Modifier.fillMaxSize().padding(10.dp),
            verticalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(label, color = Color(0xff8f9aa6), fontSize = 10.sp, fontWeight = FontWeight.Bold)
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

private fun gearLabel(gear: Int): String = when {
    gear < 0 -> "R"
    gear == 0 -> "N"
    else -> gear.toString()
}

private fun tyreLabel(cold: DashboardColdState): String {
    val compound = when (cold.tyreCompound) {
        16 -> "SOFT"
        17 -> "MED"
        18 -> "HARD"
        7 -> "INTER"
        8 -> "WET"
        else -> "TYRE"
    }
    return if (cold.tyreAgeLaps > 0) "$compound ${cold.tyreAgeLaps}L" else "$compound —"
}

private fun tyreTone(compound: Int): Color = when (compound) {
    16 -> Color(0xffff4d5e)
    17 -> Color(0xfff5b942)
    7 -> Color(0xff32d583)
    8 -> Color(0xff43a5ff)
    else -> Color(0xffd8dee6)
}
