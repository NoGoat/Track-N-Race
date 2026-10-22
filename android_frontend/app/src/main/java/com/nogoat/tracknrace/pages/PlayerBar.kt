package com.nogoat.tracknrace.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.nogoat.tracknrace.PlaybackLapInfo
import com.nogoat.tracknrace.R
import com.nogoat.tracknrace.TelemetryStore

/** The desktop's playback speeds. */
private val PlayerSpeeds = listOf(0.25f, 0.5f, 1f, 2f, 4f)

private const val SkipSeconds = 5.0

/** The hidden player's strip, on the navigation bar in portrait and along the screen's edge in landscape. */
internal val PlayerStripHeight = 32.dp

/** The open player's width in landscape, centred. */
internal val PlayerSheetWidthLandscape = 480.dp

/**
 * Seconds from the recording's start, sampled on the frame clock. Readers
 * read it only while drawing, so playing recomposes nothing.
 */
@Composable
private fun rememberPlaybackPosition(store: TelemetryStore, held: () -> Boolean = { false }): MutableDoubleState {
    val position = remember { mutableDoubleStateOf(0.0) }
    LaunchedEffect(store) {
        while (true) {
            withFrameNanos { }
            val now = store.latestTransport().currentTime
            if (!held() && now != position.doubleValue) position.doubleValue = now
        }
    }
    return position
}

/**
 * The player while hidden: a strip in the navigation bar's colour, so it reads
 * as part of the bottom bar, with how far the recording has played as a line
 * along its top. Play and pause work from it; tapping anywhere else opens the
 * player.
 */
@Composable
internal fun PlayerStrip(
    store: TelemetryStore,
    onPlay: () -> Unit,
    onPause: () -> Unit,
    onOpen: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val position = rememberPlaybackPosition(store)
    val total = store.transportTotal
    val playing = store.transportPlaying
    val laps = remember(store.playbackCatalog) { store.playbackCatalog.laps.filter { it.lapNumber > 0 } }
    val currentLap = store.playbackSelectedLap
    val label = MaterialTheme.typography.labelMedium
    Row(
        modifier
            .fillMaxWidth()
            .height(PlayerStripHeight)
            .background(colors.surfaceContainer)
            .clickable(onClickLabel = "Show player", onClick = onOpen)
            .drawBehind {
                val line = 2.dp.toPx()
                drawRect(colors.surfaceContainerHighest, size = Size(size.width, line))
                val fraction = if (total > 0) (position.doubleValue / total).toFloat().coerceIn(0f, 1f) else 0f
                if (fraction > 0f) drawRect(colors.primary, size = Size(size.width * fraction, line))
            }
            .padding(start = 4.dp, end = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(PlayerStripHeight)
                .clip(CircleShape)
                .clickable(onClickLabel = if (playing) "Pause" else "Play", onClick = if (playing) onPause else onPlay),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                painterResource(if (playing) R.drawable.ic_pause else R.drawable.ic_play),
                contentDescription = if (playing) "Pause" else "Play",
                tint = colors.primary,
                modifier = Modifier.size(18.dp),
            )
        }
        Spacer(Modifier.width(4.dp))
        when {
            laps.isEmpty() -> Text("Loading laps", style = label, color = colors.onSurfaceVariant, maxLines = 1)
            currentLap > 0 -> {
                Text("Lap $currentLap", style = label, color = colors.onSurface, maxLines = 1)
                Text(" of ${laps.last().lapNumber}", style = label, color = colors.outline, maxLines = 1)
            }
            else -> Text("${laps.size} laps", style = label, color = colors.onSurface, maxLines = 1)
        }
        Spacer(Modifier.weight(1f))
        // Drawn, not composed: the clock moves every frame while playing.
        Readout(
            runs = {
                listOf(
                    ReadoutRun(clock(position.doubleValue), colors.onSurfaceVariant, bold = false),
                    ReadoutRun(" / ${clock(total)}", colors.outline, bold = false),
                )
            },
            style = label.copy(fontFeatureSettings = "tnum"),
            modifier = Modifier.width(120.dp),
            alignEnd = true,
        )
        Icon(
            painterResource(R.drawable.ic_expand_more),
            contentDescription = null,
            tint = colors.onSurfaceVariant,
            modifier = Modifier.padding(start = 8.dp).size(20.dp).graphicsLayer { rotationZ = 180f },
        )
    }
}

/**
 * The open player, over the page: the lap (tap to jump to another) and the
 * clock, a scrubber marked at the laps, then speed, back 5 s, play, forward
 * 5 s and close. It takes the strip's place; swiping it down, or tapping
 * outside it, hides it again.
 */
@Composable
internal fun PlayerSheet(
    store: TelemetryStore,
    onPlay: () -> Unit,
    onPause: () -> Unit,
    onSeek: (Float) -> Unit,
    onSpeed: (Float) -> Unit,
    onClose: () -> Unit,
    onHide: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val total = store.transportTotal
    val playing = store.transportPlaying
    val speed = store.transportSpeed
    val catalog = store.playbackCatalog
    val laps = remember(catalog) { catalog.laps.filter { it.lapNumber > 0 } }
    val currentLap = store.playbackSelectedLap

    var dragging by remember { mutableStateOf(false) }
    val position = rememberPlaybackPosition(store) { dragging }
    var lapsOpen by remember { mutableStateOf(false) }
    var speedOpen by remember { mutableStateOf(false) }
    val timeStyle = MaterialTheme.typography.labelMedium.copy(fontFeatureSettings = "tnum")

    fun seekTo(seconds: Double) {
        if (total <= 0.0) return
        val clamped = seconds.coerceIn(0.0, total)
        position.doubleValue = clamped
        onSeek((clamped / total).toFloat())
    }

    Surface(
        modifier.pointerInput(Unit) {
            // A swipe down hides it.
            var travel = 0f
            detectVerticalDragGestures(
                onDragStart = { travel = 0f },
                onDragEnd = { if (travel > 32.dp.toPx()) onHide() },
            ) { change, amount ->
                travel += amount
                change.consume()
            }
        },
        shape = RoundedCornerShape(topStart = 16.dp, topEnd = 16.dp),
        color = colors.surfaceContainer,
        shadowElevation = 6.dp,
    ) {
        Column(Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp, bottom = 10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Row(
                    Modifier
                        .weight(1f)
                        .clip(MaterialTheme.shapes.small)
                        .clickable(enabled = laps.isNotEmpty(), onClickLabel = "Go to lap") { lapsOpen = true },
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    when {
                        laps.isEmpty() -> Text("Loading laps", style = MaterialTheme.typography.labelLarge, color = colors.onSurfaceVariant, maxLines = 1)
                        currentLap > 0 -> {
                            Text("Lap $currentLap", style = MaterialTheme.typography.labelLarge, color = colors.onSurface, maxLines = 1)
                            Text(" of ${laps.last().lapNumber}", style = MaterialTheme.typography.labelLarge, color = colors.outline, maxLines = 1)
                        }
                        else -> Text("${laps.size} laps", style = MaterialTheme.typography.labelLarge, color = colors.onSurface, maxLines = 1)
                    }
                    if (laps.isNotEmpty()) {
                        Icon(painterResource(R.drawable.ic_expand_more), contentDescription = null, tint = colors.outline, modifier = Modifier.size(18.dp))
                    }
                }
                // Drawn, not composed: the clock moves every frame while playing.
                Readout(
                    runs = {
                        listOf(
                            ReadoutRun(clock(position.doubleValue), colors.onSurface, bold = false),
                            ReadoutRun(" / ${clock(total)}", colors.outline, bold = false),
                        )
                    },
                    style = timeStyle,
                    modifier = Modifier.width(120.dp),
                    alignEnd = true,
                )
            }
            LapScrubber(
                position = position,
                total = total,
                marks = remember(laps, catalog) {
                    val start = store.latestTransport().startTime
                    laps.drop(1).map { it.lapNumber to it.startSessionTime - start }
                },
                onDrag = { dragging = it },
                onSeek = ::seekTo,
                modifier = Modifier.fillMaxWidth(),
            )
            Box(Modifier.fillMaxWidth().padding(top = 2.dp)) {
                // A small outlined button; its touch target is the full 48 dp.
                Box(
                    Modifier
                        .align(Alignment.CenterStart)
                        .size(width = 56.dp, height = 48.dp)
                        .clickable(onClickLabel = "Playback speed") { speedOpen = true },
                    contentAlignment = Alignment.CenterStart,
                ) {
                    Text(
                        speedLabel(speed),
                        Modifier
                            .border(1.dp, colors.outlineVariant, RoundedCornerShape(8.dp))
                            .padding(horizontal = 12.dp, vertical = 6.dp),
                        style = MaterialTheme.typography.labelLarge.copy(fontFeatureSettings = "tnum"),
                        color = colors.onSurfaceVariant,
                        maxLines = 1,
                    )
                }
                Row(
                    Modifier.align(Alignment.Center),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(16.dp),
                ) {
                    IconButton(onClick = { seekTo(store.latestTransport().currentTime - SkipSeconds) }) {
                        Icon(painterResource(R.drawable.ic_replay_5), contentDescription = "Back 5 seconds", tint = colors.onSurfaceVariant)
                    }
                    FilledIconButton(onClick = if (playing) onPause else onPlay, modifier = Modifier.size(52.dp)) {
                        Icon(
                            painterResource(if (playing) R.drawable.ic_pause else R.drawable.ic_play),
                            contentDescription = if (playing) "Pause" else "Play",
                        )
                    }
                    IconButton(onClick = { seekTo(store.latestTransport().currentTime + SkipSeconds) }) {
                        Icon(painterResource(R.drawable.ic_forward_5), contentDescription = "Forward 5 seconds", tint = colors.onSurfaceVariant)
                    }
                }
                IconButton(onClick = onClose, modifier = Modifier.align(Alignment.CenterEnd)) {
                    Icon(painterResource(R.drawable.ic_close), contentDescription = "Close recording", tint = colors.onSurfaceVariant)
                }
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

/** Lap strides for the scrubber's ticks; a race of 50+ laps cannot mark each one. */
private val MarkStrides = intArrayOf(1, 2, 5, 10, 20, 50)

/** The narrowest gap between two lap ticks before they thin out. */
private val MinMarkSpacing = 24.dp

/**
 * A thin line filled to the clock, with a short tick at each lap start (every
 * 2nd, 5th or 10th lap when they would crowd) and a thumb that grows while
 * dragged. Dragging moves the thumb; letting go seeks, so a drag costs one
 * seek rather than one per frame.
 */
@Composable
private fun LapScrubber(
    position: MutableDoubleState,
    total: Double,
    /** Each lap after the first: its number and when it starts. */
    marks: List<Pair<Int, Double>>,
    onDrag: (Boolean) -> Unit,
    onSeek: (Double) -> Unit,
    modifier: Modifier,
) {
    val colors = MaterialTheme.colorScheme
    var dragging by remember { mutableStateOf(false) }
    val inset = 6.dp
    fun fraction(): Float = if (total > 0) (position.doubleValue / total).toFloat().coerceIn(0f, 1f) else 0f
    Spacer(
        modifier
            .height(28.dp)
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
                val track = 2.dp.toPx()
                val at = edge + span * fraction()
                drawRect(colors.surfaceContainerHighest, Offset(edge, y - track / 2), Size(span, track))
                drawRect(colors.primary, Offset(edge, y - track / 2), Size(at - edge, track))
                // Lap starts as short ticks across the line, thinned so they never crowd.
                if (total > 0 && marks.isNotEmpty()) {
                    val spacing = span / (marks.size + 1)
                    val stride = MarkStrides.firstOrNull { it * spacing >= MinMarkSpacing.toPx() } ?: MarkStrides.last()
                    val tick = 6.dp.toPx()
                    val width = 1.dp.toPx()
                    for ((lap, start) in marks) {
                        if (lap % stride != 0 || start <= 0 || start >= total) continue
                        val x = edge + span * (start / total).toFloat()
                        drawRect(if (x <= at) colors.primary else colors.outlineVariant, Offset(x - width / 2, y - tick / 2), Size(width, tick))
                    }
                }
                drawCircle(colors.primary, (if (dragging) 8.dp else 6.dp).toPx(), Offset(at, y))
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
