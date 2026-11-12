package com.nogoat.tracknrace.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.nogoat.tracknrace.MapLapView
import com.nogoat.tracknrace.R
import kotlin.math.roundToInt

/** The expanded playback bar's height. */
private val PlaybackBarHeight = 48.dp

/** The collapsed playback pill, and the touch targets in it and in the bar. */
private val PillHeight = 44.dp

/** The lap-view button names its view in at most this many characters, then an ellipsis. */
internal const val LAP_VIEW_LABEL_MAX = 5

/** The lap view of a per-lap layer with two laps, for the playback control's menu. */
internal class MapLapChoice(
    val view: MapLapView,
    val onView: (MapLapView) -> Unit,
    val current: MapLapName,
    val comparison: MapLapName,
) {
    /** A view's name in full: the laps by the names they are shown with. */
    fun name(option: MapLapView): String = when (option) {
        MapLapView.CURRENT -> current.text
        MapLapView.COMPARISON -> comparison.text
        MapLapView.BOTH -> "Both"
        MapLapView.COMBINED -> "Combined"
    }
}

/** [text] cut to [LAP_VIEW_LABEL_MAX] characters, with an ellipsis when cut. */
internal fun lapViewLabel(text: String): String {
    val characters = text.codePointCount(0, text.length)
    if (characters <= LAP_VIEW_LABEL_MAX) return text
    return text.substring(0, text.offsetByCodePoints(0, LAP_VIEW_LABEL_MAX)) + "…"
}

/**
 * The map's playback control. For a fixed comparison, with its own clock: a
 * small pill (play, the time, and a button that opens the bar), or the bar
 * itself (play, time, scrubber, speed, the lap-view menu, and a button that
 * closes it). Following the desktop there is no clock to drive, so only the
 * lap-view menu shows, and nothing at all without a choice of view.
 */
@Composable
internal fun MapPlaybackControl(
    clock: AnalysisMapClock?,
    elapsed: State<Double>,
    total: Double,
    /** Seconds into the lap where each sector ends, ticked on the scrubber. */
    sectorMarks: List<Double>,
    expanded: Boolean,
    onExpand: (Boolean) -> Unit,
    lapChoice: MapLapChoice?,
    modifier: Modifier = Modifier,
) {
    if (clock == null && lapChoice == null) return
    val colors = MaterialTheme.colorScheme
    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(percent = 50),
        color = colors.surfaceContainerHigh,
        shadowElevation = 3.dp,
    ) {
        when {
            clock == null -> Box(Modifier.padding(4.dp)) { LapViewButton(lapChoice!!) }
            expanded -> Row(
                Modifier.fillMaxWidth().height(PlaybackBarHeight).padding(horizontal = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                PlayButton(clock)
                ElapsedTime(elapsed, Modifier.padding(start = 2.dp))
                Scrubber(clock, elapsed, total, sectorMarks, Modifier.weight(1f))
                SpeedButton(clock)
                if (lapChoice != null) LapViewButton(lapChoice, Modifier.padding(horizontal = 2.dp))
                ControlIcon(R.drawable.ic_close_fullscreen, "Close the progress bar") { onExpand(false) }
            }
            else -> Row(Modifier.height(PillHeight), verticalAlignment = Alignment.CenterVertically) {
                PlayButton(clock)
                Row(
                    Modifier
                        .height(PillHeight)
                        .clip(RoundedCornerShape(percent = 50))
                        .clickable(onClickLabel = "Open the progress bar") { onExpand(true) }
                        .padding(start = 2.dp, end = 14.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    ElapsedTime(elapsed)
                    Box(Modifier.width(1.dp).height(20.dp).background(colors.outlineVariant))
                    Icon(
                        painterResource(R.drawable.ic_open_in_full),
                        contentDescription = null,
                        modifier = Modifier.size(18.dp),
                        tint = colors.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

@Composable
private fun PlayButton(clock: AnalysisMapClock) {
    val playing = clock.playing
    ControlIcon(
        if (playing) R.drawable.ic_pause else R.drawable.ic_play,
        if (playing) "Pause" else "Play",
        tint = MaterialTheme.colorScheme.primary,
        iconSize = 20.dp,
        onClick = clock::toggle,
    )
}

@Composable
private fun ControlIcon(
    icon: Int,
    description: String,
    tint: Color = MaterialTheme.colorScheme.onSurfaceVariant,
    iconSize: Dp = 18.dp,
    onClick: () -> Unit,
) {
    Box(
        Modifier.size(PillHeight).clip(CircleShape).clickable(onClickLabel = description, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(painterResource(icon), contentDescription = description, modifier = Modifier.size(iconSize), tint = tint)
    }
}

/** The clock in tenths, drawn rather than composed: it changes ten times a second while playing. */
@Composable
private fun ElapsedTime(elapsed: State<Double>, modifier: Modifier = Modifier) {
    val colors = MaterialTheme.colorScheme
    val style = MaterialTheme.typography.labelLarge.copy(fontFeatureSettings = "tnum")
    val tenths by remember { derivedStateOf(structuralEqualityPolicy()) { (elapsed.value * 10).roundToInt() } }
    // Wide enough for "0:00.0" at this size, so nothing beside it shifts.
    val width = with(LocalDensity.current) { (style.fontSize.toPx() * 3.4f).toDp() }
    Readout(
        runs = { listOf(ReadoutRun(formatElapsed(tenths / 10.0, 0.1), colors.onSurface, bold = false)) },
        style = style,
        modifier = modifier.width(width),
    )
}

@Composable
private fun SpeedButton(clock: AnalysisMapClock) {
    val colors = MaterialTheme.colorScheme
    var open by remember { mutableStateOf(false) }
    Box(
        Modifier
            .height(PillHeight)
            .widthIn(min = 36.dp)
            .clip(MaterialTheme.shapes.small)
            .clickable(onClickLabel = "Change speed") { open = true },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            speedLabel(clock.speed),
            style = MaterialTheme.typography.labelLarge.copy(fontFeatureSettings = "tnum"),
            color = colors.primary,
            maxLines = 1,
        )
    }
    if (open) {
        SpeedDialog(
            speed = clock.speed,
            onPick = { next ->
                clock.changeSpeed(next)
                open = false
            },
            onDismiss = { open = false },
        )
    }
}

/**
 * The lap view as a button naming it in at most [LAP_VIEW_LABEL_MAX]
 * characters, opening a menu of the four views in full: each lap by its
 * name, Both on two maps, Combined as bands.
 */
@Composable
private fun LapViewButton(choice: MapLapChoice, modifier: Modifier = Modifier) {
    val colors = MaterialTheme.colorScheme
    var open by remember { mutableStateOf(false) }
    val full = choice.name(choice.view)
    Box(modifier) {
        Row(
            Modifier
                .height(36.dp)
                .clip(RoundedCornerShape(percent = 50))
                .background(if (open) colors.secondaryContainer else colors.surfaceContainerHighest)
                .clickable(onClickLabel = "Change lap view") { open = true }
                .semantics { contentDescription = "Lap view: $full" }
                .padding(start = 12.dp, end = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(
                lapViewLabel(full),
                style = MaterialTheme.typography.labelLarge,
                color = if (open) colors.onSecondaryContainer else colors.onSurface,
                maxLines = 1,
            )
            Icon(
                painterResource(R.drawable.ic_expand_more),
                contentDescription = null,
                modifier = Modifier.size(18.dp).rotate(if (open) 180f else 0f),
                tint = colors.onSurfaceVariant,
            )
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (option in MapLapView.entries) {
                val selected = option == choice.view
                DropdownMenuItem(
                    text = {
                        Column {
                            Text(choice.name(option), maxLines = 1, fontWeight = if (selected) FontWeight.SemiBold else null)
                            when (option) {
                                MapLapView.BOTH -> Text("Two maps", style = MaterialTheme.typography.labelSmall, color = colors.onSurfaceVariant)
                                MapLapView.COMBINED -> Text("Bands", style = MaterialTheme.typography.labelSmall, color = colors.onSurfaceVariant)
                                else -> Unit
                            }
                        }
                    },
                    leadingIcon = { LapViewGlyph(option, choice.current.color, choice.comparison.color) },
                    trailingIcon = if (selected) {
                        { Icon(painterResource(R.drawable.ic_check), contentDescription = "Selected", tint = colors.primary, modifier = Modifier.size(20.dp)) }
                    } else {
                        null
                    },
                    onClick = {
                        choice.onView(option)
                        open = false
                    },
                )
            }
        }
    }
}

/** A view's mark in the lap colours: a dot for one lap, two bars stacked for Both, side by side for Combined. */
@Composable
private fun LapViewGlyph(view: MapLapView, current: Color, comparison: Color) {
    Box(Modifier.size(18.dp), contentAlignment = Alignment.Center) {
        when (view) {
            MapLapView.CURRENT -> Box(Modifier.size(8.dp).background(current, CircleShape))
            MapLapView.COMPARISON -> Box(Modifier.size(8.dp).background(comparison, CircleShape))
            MapLapView.BOTH -> Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Box(Modifier.size(10.dp, 3.dp).background(current, RoundedCornerShape(1.dp)))
                Box(Modifier.size(10.dp, 3.dp).background(comparison, RoundedCornerShape(1.dp)))
            }
            MapLapView.COMBINED -> Row(horizontalArrangement = Arrangement.spacedBy(1.5.dp)) {
                Box(Modifier.size(3.dp, 11.dp).background(current, RoundedCornerShape(1.dp)))
                Box(Modifier.size(3.dp, 11.dp).background(comparison, RoundedCornerShape(1.dp)))
            }
        }
    }
}
