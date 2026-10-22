package com.nogoat.tracknrace.pages

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import java.util.Locale

private const val THUMB_RADIUS_DP = 10f
private const val HUE_TRACK_DP = 12f
private val HUE_STOPS = List(7) { Color.hsv(it * 60f % 360f, 1f, 1f) }

/**
 * A colour picker in a dialog, as the desktop's: saturation and brightness in
 * a square, hue on a slider, and the hex value to type. Nothing changes until Apply; Default fills in [default] to apply.
 */
@Composable
internal fun ColorPickerDialog(
    title: String,
    initial: Int,
    default: Int,
    onPick: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    val start = remember(initial) { hsv(initial) }
    var hue by remember(initial) { mutableFloatStateOf(start[0]) }
    var saturation by remember(initial) { mutableFloatStateOf(start[1]) }
    var value by remember(initial) { mutableFloatStateOf(start[2]) }
    var hex by remember(initial) { mutableStateOf(hexOf(initial)) }
    val picked = android.graphics.Color.HSVToColor(floatArrayOf(hue, saturation, value))

    // Moves the picker to [color], keeping the hue where the colour has none.
    fun show(color: Int, updateHex: Boolean = true) {
        val next = hsv(color)
        if (next[1] > 0f && next[2] > 0f) hue = next[0]
        saturation = next[1]
        value = next[2]
        if (updateHex) hex = hexOf(color)
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                // Saturation across, brightness down.
                Box(
                    Modifier
                        .fillMaxWidth()
                        .aspectRatio(1.5f)
                        .drawBehind {
                            val corner = CornerRadius(10.dp.toPx())
                            drawRoundRect(Brush.horizontalGradient(listOf(Color.White, Color.hsv(hue, 1f, 1f))), cornerRadius = corner)
                            drawRoundRect(Brush.verticalGradient(listOf(Color.Transparent, Color.Black)), cornerRadius = corner)
                            drawThumb(Offset(saturation * size.width, (1f - value) * size.height), Color(picked))
                        }
                        .dragPosition { position, size ->
                            saturation = (position.x / size.width).coerceIn(0f, 1f)
                            value = 1f - (position.y / size.height).coerceIn(0f, 1f)
                            hex = hexOf(android.graphics.Color.HSVToColor(floatArrayOf(hue, saturation, value)))
                        },
                )
                // Hue, its thumb kept inside the track's ends.
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height((THUMB_RADIUS_DP * 2 + 4).dp)
                        .drawBehind {
                            val radius = THUMB_RADIUS_DP.dp.toPx()
                            val track = HUE_TRACK_DP.dp.toPx()
                            drawRoundRect(
                                Brush.horizontalGradient(HUE_STOPS, startX = radius, endX = size.width - radius),
                                topLeft = Offset(0f, (size.height - track) / 2f),
                                size = Size(size.width, track),
                                cornerRadius = CornerRadius(track / 2f),
                            )
                            drawThumb(Offset(radius + hue / 360f * (size.width - radius * 2f), size.height / 2f), Color.hsv(hue, 1f, 1f))
                        }
                        .dragPosition { position, size ->
                            val radius = THUMB_RADIUS_DP.dp.toPx()
                            val span = (size.width - radius * 2f).coerceAtLeast(1f)
                            hue = ((position.x - radius) / span).coerceIn(0f, 1f) * 360f
                            hex = hexOf(android.graphics.Color.HSVToColor(floatArrayOf(hue, saturation, value)))
                        },
                )
                // The colour as it was beside the colour as picked, and its hex.
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    // Sized and shaped as the hex field.
                    Row(
                        Modifier
                            .size(56.dp)
                            .clip(OutlinedTextFieldDefaults.shape),
                    ) {
                        Box(Modifier.weight(1f).fillMaxHeight().background(Color(initial)))
                        Box(Modifier.weight(1f).fillMaxHeight().background(Color(picked)))
                    }
                    OutlinedTextField(
                        value = hex,
                        onValueChange = { text ->
                            val digits = text.uppercase(Locale.US).filter { it in "0123456789ABCDEF" }.take(6)
                            hex = digits
                            if (digits.length == 6) show((0xff shl 24) or digits.toInt(16), updateHex = false)
                        },
                        prefix = { Text("#") },
                        singleLine = true,
                        isError = hex.length != 6,
                        keyboardOptions = KeyboardOptions(
                            capitalization = KeyboardCapitalization.Characters,
                            autoCorrectEnabled = false,
                            keyboardType = KeyboardType.Ascii,
                        ),
                        modifier = Modifier.weight(1f),
                    )
                }
            }
        },
        confirmButton = { TextButton(onClick = { onPick(picked) }) { Text("Apply") } },
        dismissButton = {
            Row {
                TextButton(onClick = { show(default) }) { Text("Default") }
                TextButton(onClick = onDismiss) { Text("Cancel") }
            }
        },
    )
}

private fun hsv(color: Int): FloatArray = FloatArray(3).also { android.graphics.Color.colorToHSV(color, it) }

private fun hexOf(color: Int): String = String.format(Locale.US, "%06X", color and 0xffffff)

/** A ring with [fill] inside, legible on light and dark. */
private fun DrawScope.drawThumb(centre: Offset, fill: Color) {
    val radius = THUMB_RADIUS_DP.dp.toPx()
    drawCircle(fill, radius, centre)
    drawCircle(Color.White, radius, centre, style = Stroke(2.5.dp.toPx()))
    drawCircle(Color.Black.copy(alpha = 0.35f), radius + 1.dp.toPx(), centre, style = Stroke(1.dp.toPx()))
}

/** Reports where a finger is, from first touch through every drag, over a dialog that would otherwise scroll. */
private fun Modifier.dragPosition(onPosition: Density.(Offset, IntSize) -> Unit): Modifier = pointerInput(Unit) {
    awaitEachGesture {
        val down = awaitFirstDown()
        down.consume()
        onPosition(down.position, size)
        while (true) {
            val event = awaitPointerEvent()
            val change = event.changes.firstOrNull { it.id == down.id } ?: break
            if (!change.pressed) break
            change.consume()
            onPosition(change.position, size)
        }
    }
}
