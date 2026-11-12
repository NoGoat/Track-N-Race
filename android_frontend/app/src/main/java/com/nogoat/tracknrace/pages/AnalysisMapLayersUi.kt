package com.nogoat.tracknrace.pages

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.painter.Painter
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nogoat.tracknrace.MapLayer
import com.nogoat.tracknrace.MapLayerPaint
import com.nogoat.tracknrace.MapLayerPalette
import com.nogoat.tracknrace.R
import kotlin.math.roundToInt

/** The layer previews' ground: the map as drawn in the dark theme, in both themes. */
private val PreviewGround = Color(0xFF0D131E)

/**
 * The map's tools, floating over it: the layer sheet, the braking points
 * toggle where the map shows braking zones ([brakingOn] null hides it), and
 * a reset to the whole circuit, live while the map is [zoomed] in.
 */
@Composable
internal fun MapToolRail(
    onLayers: () -> Unit,
    layersOpen: Boolean,
    brakingOn: Boolean?,
    onBraking: () -> Unit,
    zoomed: Boolean,
    onResetZoom: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(26.dp),
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        shadowElevation = 3.dp,
    ) {
        Column(Modifier.padding(4.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            RailButton(
                painter = painterResource(R.drawable.ic_map_layers),
                description = "Map layer",
                on = layersOpen,
                onClick = onLayers,
            )
            if (brakingOn != null) {
                RailButton(
                    painter = painterResource(R.drawable.ic_braking_points),
                    description = "Braking points",
                    on = brakingOn,
                    onClick = onBraking,
                    toggle = true,
                )
            }
            RailButton(
                painter = painterResource(R.drawable.ic_fit_circuit),
                description = "Show the whole circuit",
                on = false,
                onClick = onResetZoom,
                enabled = zoomed,
            )
        }
    }
}

@Composable
private fun RailButton(
    painter: Painter,
    description: String,
    on: Boolean,
    onClick: () -> Unit,
    toggle: Boolean = false,
    enabled: Boolean = true,
) {
    val colors = MaterialTheme.colorScheme
    IconButton(
        onClick = onClick,
        enabled = enabled,
        modifier = Modifier
            .size(44.dp)
            .semantics {
                contentDescription = description
                if (toggle) stateDescription = if (on) "On" else "Off"
            },
        colors = IconButtonDefaults.iconButtonColors(
            containerColor = if (on) colors.secondaryContainer else Color.Transparent,
            contentColor = if (on) colors.onSecondaryContainer else colors.onSurfaceVariant,
        ),
    ) {
        Icon(painter, contentDescription = null, modifier = Modifier.size(22.dp))
    }
}

/**
 * Layer 1: every layer as a tile with a preview of São Paulo painted in it.
 * Choosing one applies it at once and leaves the sheet open, so the layers can
 * be flicked through; the sheet closes from its button, a swipe or the scrim.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun MapLayerSheet(selected: MapLayer, onSelect: (MapLayer) -> Unit, onDismiss: () -> Unit) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(Modifier.navigationBarsPadding().padding(bottom = 16.dp)) {
            Row(Modifier.fillMaxWidth().padding(start = 24.dp, end = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("Map layer", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
                IconButton(onClick = onDismiss) {
                    Icon(painterResource(R.drawable.ic_close), contentDescription = "Close")
                }
            }
            Column(
                Modifier.padding(PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp)).selectableGroup(),
                verticalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                for (row in MapLayer.entries.chunked(4)) {
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        for (layer in row) {
                            LayerTile(layer, layer == selected, { onSelect(layer) }, Modifier.weight(1f))
                        }
                        // Keeps the last row's tiles the width of the others.
                        repeat(4 - row.size) { Spacer(Modifier.weight(1f)) }
                    }
                }
            }
        }
    }
}

@Composable
private fun LayerTile(layer: MapLayer, selected: Boolean, onClick: () -> Unit, modifier: Modifier) {
    val colors = MaterialTheme.colorScheme
    Column(
        modifier
            .clip(RoundedCornerShape(12.dp))
            .selectable(selected = selected, role = Role.RadioButton, onClick = onClick),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Box {
            Image(
                painter = painterResource(layer.preview),
                contentDescription = null,
                contentScale = ContentScale.Fit,
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(10f / 7f)
                    .clip(RoundedCornerShape(12.dp))
                    .background(PreviewGround)
                    .border(
                        if (selected) 2.dp else 1.dp,
                        if (selected) colors.primary else colors.outlineVariant,
                        RoundedCornerShape(12.dp),
                    )
                    .padding(4.dp),
            )
            if (selected) {
                Box(
                    Modifier
                        .align(Alignment.TopEnd)
                        .padding(5.dp)
                        .size(18.dp)
                        .background(colors.primary, CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(painterResource(R.drawable.ic_check), contentDescription = null, tint = colors.onPrimary, modifier = Modifier.size(12.dp))
                }
            }
        }
        Column {
            Text(
                layer.label,
                style = MaterialTheme.typography.labelMedium,
                fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
                color = if (selected) colors.primary else colors.onSurface,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            if (layer.perLap) {
                Text("Per lap", style = MaterialTheme.typography.labelSmall, color = colors.onSurfaceVariant)
            }
        }
    }
}

/** One lap as the lap-view menu and the stacked maps name it. */
internal class MapLapName(val text: String, val color: Color)

/** A lap's name over its own map when both laps are shown stacked. */
@Composable
internal fun MapLapTag(name: MapLapName, modifier: Modifier = Modifier) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Box(Modifier.size(3.dp, 14.dp).background(name.color, RoundedCornerShape(2.dp)))
        Text(name.text, style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.Bold)
    }
}

/**
 * The layer's key: the four pedal states, or a colour scale with its ends,
 * in a small panel on the map; [compact] draws it smaller and bare, for a
 * tight space. Track dominance and Racing line draw in the laps' own
 * colours, so they have no key.
 */
@Composable
internal fun MapLayerLegend(
    layer: MapLayer,
    paint: MapLayerPaint?,
    dark: Boolean,
    compact: Boolean,
    modifier: Modifier = Modifier,
) {
    val palette = MapLayerPalette.of(dark)
    val small = MaterialTheme.typography.labelSmall.let { if (compact) it.copy(fontSize = 10.sp) else it }
    val textColor = MaterialTheme.colorScheme.onSurfaceVariant
    @Composable
    fun swatch(color: Color, label: String) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            Box(Modifier.size(if (compact) 8.dp else 14.dp, if (compact) 8.dp else 4.dp).background(color, RoundedCornerShape(2.dp)))
            Text(label, style = small, color = textColor, maxLines = 1)
        }
    }
    @Composable
    fun scale(ramp: List<Int>, low: String, high: String) {
        Column(Modifier.width(if (compact) 76.dp else 150.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(5.dp)
                    .clip(RoundedCornerShape(3.dp))
                    .background(if (ramp.size > 1) Brush.horizontalGradient(ramp.map { Color(it) }) else Brush.horizontalGradient(listOf(textColor, textColor))),
            )
            Row {
                Text(low, style = small, color = textColor, modifier = Modifier.weight(1f), maxLines = 1)
                Text(high, style = small, color = textColor, maxLines = 1)
            }
        }
    }
    val content: @Composable () -> Unit = {
        when (layer) {
            MapLayer.NONE, MapLayer.DOMINANCE, MapLayer.RACING_LINE -> Unit
            MapLayer.INPUTS -> {
                // Coasting, partial, full throttle, braking, as libtnrp orders them.
                val inputs = palette?.inputs.orEmpty()
                val names = listOf("Coast", "Partial", "Full", "Brake")
                val entries = names.indices.mapNotNull { index -> inputs.getOrNull(index)?.let { Color(it) to names[index] } }
                    .reversed()
                if (compact) {
                    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        for (pair in entries.chunked(2)) {
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                for ((color, name) in pair) Box(Modifier.width(44.dp)) { swatch(color, name) }
                            }
                        }
                    }
                } else {
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        for ((color, name) in entries) swatch(color, name)
                    }
                }
            }
            MapLayer.SPEED -> {
                val low = paint?.speedMinKph?.takeIf { it > 0 }?.roundToInt()?.toString() ?: "Slow"
                val high = paint?.speedMaxKph?.takeIf { it > 0 }?.let { "${it.roundToInt()} km/h" } ?: "Fast"
                scale(palette?.speed.orEmpty(), low, high)
            }
            MapLayer.ERS -> {
                val high = paint?.ersPeakKw?.takeIf { it > 0 }?.let { "${it.roundToInt()} kW" } ?: "Peak"
                scale(palette?.ers.orEmpty(), "0", high)
            }
            MapLayer.HARVEST -> {
                val high = paint?.harvestPeakKw?.takeIf { it > 0 }?.let { "${it.roundToInt()} kW" } ?: "Peak"
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    scale(palette?.harvest.orEmpty(), "0", high)
                    if (!compact) palette?.let { swatch(Color(it.superclip), "Superclip") }
                }
            }
        }
    }
    if (layer == MapLayer.NONE || layer == MapLayer.DOMINANCE || layer == MapLayer.RACING_LINE) return
    if (compact) {
        Box(modifier) { content() }
    } else {
        Surface(
            modifier = modifier,
            shape = RoundedCornerShape(8.dp),
            color = MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.92f),
        ) {
            Box(Modifier.padding(horizontal = 10.dp, vertical = 6.dp)) { content() }
        }
    }
}

/** Why a layer shows nothing yet: loading, an older desktop, or a car's Restricted telemetry. */
@Composable
internal fun MapLayerStatus(loading: Boolean, message: String?, modifier: Modifier = Modifier) {
    if (!loading && message == null) return
    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(8.dp),
        color = MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.92f),
    ) {
        Row(
            Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (loading) CircularProgressIndicator(Modifier.size(14.dp), strokeWidth = 2.dp)
            Text(
                message ?: "Painting the layer",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}
