package com.tracknrace.android.pages

import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableDoubleState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.tracknrace.android.PlaybackLapInfo
import com.tracknrace.android.R
import com.tracknrace.android.TelemetryStore
import kotlin.math.abs

/** The desktop's playback speeds. */
private val PlayerSpeeds = listOf(0.25f, 0.5f, 1f, 2f, 4f)

private const val SkipSeconds = 5.0

/**
 * The player for a recording open on the phone, sitting on the navigation bar
 * in the same colour, laid out like the Analysis playback bar. The top line
 * names the recording with the lap (tap to jump to another), the speed and
 * close; the bottom line has ±5 s, play, and the time on a scrubber ticked at
 * every lap. The clock and scrubber are drawn, so playing recomposes nothing.
 */
@Composable
internal fun PlayerBar(
    store: TelemetryStore,
    onPlay: () -> Unit,
    onPause: () -> Unit,
    onSeek: (Float) -> Unit,
    onSpeed: (Float) -> Unit,
    onClose: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val total = store.transportTotal
    val playing = store.transportPlaying
    val speed = store.transportSpeed
    val title = store.localRecording.orEmpty()
    val catalog = store.playbackCatalog
    val laps = remember(catalog) { catalog.laps.filter { it.lapNumber > 0 } }
    val currentLap = store.playbackSelectedLap

    // Seconds from the recording's start, sampled on the frame clock and read
    // only while drawing.
    val position = remember { mutableDoubleStateOf(0.0) }
    var dragging by remember { mutableStateOf(false) }
    LaunchedEffect(store) {
        while (true) {
            withFrameNanos { }
            val now = store.latestTransport().currentTime
            if (!dragging && now != position.doubleValue) position.doubleValue = now
        }
    }
    var lapsOpen by remember { mutableStateOf(false) }
    var speedOpen by remember { mutableStateOf(false) }
    val timeStyle = MaterialTheme.typography.labelLarge.copy(fontFeatureSettings = "tnum")
    // Wide enough for "0:00:00", so the scrubber never shifts.
    val timeWidth = with(LocalDensity.current) { (timeStyle.fontSize.toPx() * 3.9f).toDp() }

    fun seekTo(seconds: Double) {
        if (total <= 0.0) return
        val clamped = seconds.coerceIn(0.0, total)
        position.doubleValue = clamped
        onSeek((clamped / total).toFloat())
    }

    Surface(modifier.fillMaxWidth(), color = colors.surfaceContainer) {
        Column(Modifier.padding(horizontal = 8.dp)) {
            Row(Modifier.fillMaxWidth().height(48.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(
                    Modifier
                        .weight(1f)
                        .clip(MaterialTheme.shapes.small)
                        .clickable(enabled = laps.isNotEmpty(), onClickLabel = "Go to lap") { lapsOpen = true }
                        .padding(horizontal = 8.dp, vertical = 2.dp),
                ) {
                    Text(
                        title,
                        style = MaterialTheme.typography.titleSmall,
                        color = colors.onSurface,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            when {
                                laps.isEmpty() -> "Loading laps"
                                currentLap > 0 -> "Lap $currentLap of ${laps.last().lapNumber}"
                                else -> "${laps.size} laps"
                            },
                            style = MaterialTheme.typography.bodySmall,
                            color = colors.onSurfaceVariant,
                            maxLines = 1,
                        )
                        if (laps.isNotEmpty()) {
                            Icon(
                                painterResource(R.drawable.ic_expand_more),
                                contentDescription = null,
                                tint = colors.onSurfaceVariant,
                                modifier = Modifier.size(16.dp),
                            )
                        }
                    }
                }
                // A 48 dp target around a compact outlined label, as on Analysis.
                Box(
                    Modifier
                        .height(48.dp)
                        .widthIn(min = 48.dp)
                        .clip(MaterialTheme.shapes.small)
                        .clickable(onClickLabel = "Change speed") { speedOpen = true },
                    contentAlignment = Alignment.Center,
                ) {
                    Row(
                        Modifier
                            .border(1.dp, colors.outlineVariant, MaterialTheme.shapes.small)
                            .padding(start = 8.dp, end = 4.dp, top = 4.dp, bottom = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(speedLabel(speed), style = timeStyle, color = colors.primary, maxLines = 1)
                        Icon(
                            painterResource(R.drawable.ic_expand_more),
                            contentDescription = null,
                            modifier = Modifier.size(16.dp),
                            tint = colors.primary,
                        )
                    }
                }
                IconButton(onClick = onClose) {
                    Icon(painterResource(R.drawable.ic_close), contentDescription = "Close recording")
                }
            }
            Row(Modifier.fillMaxWidth().height(56.dp), verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = { seekTo(store.latestTransport().currentTime - SkipSeconds) }) {
                    Icon(painterResource(R.drawable.ic_replay), contentDescription = "Back 5 seconds")
                }
                FilledIconButton(onClick = if (playing) onPause else onPlay) {
                    Icon(
                        painterResource(if (playing) R.drawable.ic_pause else R.drawable.ic_play),
                        contentDescription = if (playing) "Pause" else "Play",
                    )
                }
                IconButton(onClick = { seekTo(store.latestTransport().currentTime + SkipSeconds) }) {
                    Icon(painterResource(R.drawable.ic_forward), contentDescription = "Forward 5 seconds")
                }
                // Drawn, not composed: the clock moves every frame while playing.
                Readout(
                    runs = { listOf(ReadoutRun(clock(position.doubleValue), colors.onSurface, bold = false)) },
                    style = timeStyle,
                    modifier = Modifier.padding(start = 4.dp).width(timeWidth),
                )
                LapScrubber(
                    position = position,
                    total = total,
                    marks = remember(laps, catalog) {
                        val start = store.latestTransport().startTime
                        laps.drop(1).map { it.startSessionTime - start }
                    },
                    onDrag = { dragging = it },
                    onSeek = ::seekTo,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    clock(total),
                    style = timeStyle,
                    color = colors.onSurfaceVariant,
                    maxLines = 1,
                    modifier = Modifier.padding(end = 4.dp),
                )
            }
        }
    }

    if (speedOpen) {
        SpeedDialog(
            speed = speed,
            onPick = {
                speedOpen = false
                onSpeed(it)
            },
            onDismiss = { speedOpen = false },
            speeds = PlayerSpeeds,
        )
    }
    if (lapsOpen) {
        LapDialog(
            laps = laps,
            current = currentLap,
            fastest = catalog.fastestLap,
            onPick = { lap ->
                lapsOpen = false
                seekTo(lap.startSessionTime - store.latestTransport().startTime)
            },
            onDismiss = { lapsOpen = false },
        )
    }
}

/**
 * The Analysis scrubber's look: a thin track filled to the clock, a tick at
 * every lap, and a thumb that grows while dragged. Dragging moves the thumb;
 * letting go seeks, so a drag costs one seek rather than one per frame.
 */
@Composable
private fun LapScrubber(
    position: MutableDoubleState,
    total: Double,
    marks: List<Double>,
    onDrag: (Boolean) -> Unit,
    onSeek: (Double) -> Unit,
    modifier: Modifier,
) {
    val colors = MaterialTheme.colorScheme
    var dragging by remember { mutableStateOf(false) }
    val inset = 10.dp
    fun fraction(): Float = if (total > 0) (position.doubleValue / total).toFloat().coerceIn(0f, 1f) else 0f
    Spacer(
        modifier
            .height(48.dp)
            .graphicsLayer()
            .semantics {
                contentDescription = "Playback position"
                progressBarRangeInfo = ProgressBarRangeInfo(fraction(), 0f..1f)
                setProgress { value ->
                    onSeek(value.toDouble() * total)
                    true
                }
            }
            .pointerInput(total) {
                val edge = inset.toPx()
                fun secondsAt(x: Float): Double {
                    val span = (size.width - 2 * edge).coerceAtLeast(1f)
                    return ((x - edge) / span).coerceIn(0f, 1f).toDouble() * total
                }
                awaitEachGesture {
                    val down = awaitFirstDown()
                    if (total <= 0) return@awaitEachGesture
                    dragging = true
                    onDrag(true)
                    position.doubleValue = secondsAt(down.position.x)
                    down.consume()
                    drag(down.id) { change ->
                        position.doubleValue = secondsAt(change.position.x)
                        change.consume()
                    }
                    dragging = false
                    onDrag(false)
                    onSeek(position.doubleValue)
                }
            }
            .drawBehind {
                val edge = inset.toPx()
                val span = size.width - 2 * edge
                val y = size.height / 2
                val track = 4.dp.toPx()
                val corner = CornerRadius(track / 2)
                val at = edge + span * fraction()
                drawRoundRect(colors.surfaceContainerHighest, Offset(edge, y - track / 2), Size(span, track), corner)
                drawRoundRect(colors.primary, Offset(edge, y - track / 2), Size(at - edge, track), corner)
                // Lap starts, so a lap can be found by eye.
                if (total > 0) for (mark in marks) {
                    if (mark <= 0 || mark >= total) continue
                    val x = edge + span * (mark / total).toFloat()
                    drawLine(colors.onSurfaceVariant, Offset(x, y - 5.dp.toPx()), Offset(x, y + 5.dp.toPx()), 1.dp.toPx())
                }
                drawCircle(colors.primary, (if (dragging) 10.dp else 7.dp).toPx(), Offset(at, y))
            },
    )
}

/** The laps as a radio list, each with its time; the fastest is marked. */
@Composable
private fun LapDialog(
    laps: List<PlaybackLapInfo>,
    current: Int,
    fastest: Int,
    onPick: (PlaybackLapInfo) -> Unit,
    onDismiss: () -> Unit,
) {
    val listState = rememberLazyListState(
        initialFirstVisibleItemIndex = laps.indexOfFirst { it.lapNumber == current }.coerceAtLeast(0),
    )
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Go to lap") },
        text = {
            LazyColumn(Modifier.selectableGroup(), state = listState) {
                items(laps, key = { it.lapNumber }) { lap ->
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .height(52.dp)
                            .clip(MaterialTheme.shapes.small)
                            .selectable(selected = lap.lapNumber == current, role = Role.RadioButton) { onPick(lap) },
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(selected = lap.lapNumber == current, onClick = null)
                        Spacer(Modifier.width(12.dp))
                        Text("Lap ${lap.lapNumber}", style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
                        Text(
                            if (lap.lapTimeMs > 0) lapTime(lap.lapTimeMs) else "—",
                            style = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace),
                            color = if (lap.lapNumber == fastest) MaterialTheme.colorScheme.tertiary
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1,
                        )
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

private fun clock(seconds: Double): String {
    val whole = seconds.coerceAtLeast(0.0).toLong()
    val hours = whole / 3600
    val minutes = whole / 60 % 60
    val secs = whole % 60
    return if (hours > 0) "%d:%02d:%02d".format(hours, minutes, secs) else "%d:%02d".format(minutes, secs)
}

private fun lapTime(milliseconds: Int): String =
    "%d:%02d.%03d".format(milliseconds / 60_000, milliseconds / 1_000 % 60, milliseconds % 1_000)
