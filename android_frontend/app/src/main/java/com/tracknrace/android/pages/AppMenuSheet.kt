package com.tracknrace.android.pages

import androidx.annotation.DrawableRes
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.tracknrace.android.R
import kotlinx.coroutines.launch

/** Where the app's data comes from now, as the menu sheet's card shows it. */
internal sealed interface MenuSource {
    /** A recording on this phone is playing. */
    data class Recording(val title: String) : MenuSource

    /** A paired desktop, and the link's state as the paired client reports it. */
    data class Desktop(val name: String, val state: String, val detail: String?) : MenuSource

    /** The game's UDP straight to this phone. */
    data object Direct : MenuSource
}

/** The driver the app shows, as the menu sheet's driver row reads it. */
internal class MenuDriver(
    val name: String,
    /** Whether the driver's telemetry is public only; null when the game has not said. */
    val restricted: Boolean?,
    /** Opens the driver picker; null where the desktop chooses the driver. */
    val onPick: (() -> Unit)?,
)

/**
 * The app bar's menu, as a bottom sheet: what feeds the app now, with the
 * one thing to do about it, then the app's other actions. Each action slides
 * the sheet away before it runs.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun AppMenuSheet(
    source: MenuSource,
    driver: MenuDriver?,
    onOpenRecordings: () -> Unit,
    onCloseRecording: () -> Unit,
    onReconnect: () -> Unit,
    onSettings: () -> Unit,
    onDismiss: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val scope = rememberCoroutineScope()
    fun close(then: () -> Unit) {
        scope.launch { sheetState.hide() }.invokeOnCompletion {
            onDismiss()
            then()
        }
    }

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(Modifier.fillMaxWidth().padding(bottom = 16.dp)) {
            SourceCard(
                source,
                onCloseRecording = { close(onCloseRecording) },
                onReconnect = { close(onReconnect) },
                modifier = Modifier.padding(horizontal = 16.dp),
            )
            Spacer(Modifier.height(8.dp))
            driver?.let { DriverRow(it, it.onPick?.let { pick -> { close(pick) } }) }
            MenuRow(R.drawable.ic_folder_open, "Open recording", "Play a session saved on this phone") { close(onOpenRecordings) }
            MenuRow(R.drawable.ic_settings, "Settings", "Source, pairing and recording") { close(onSettings) }
        }
    }
}

@Composable
private fun SourceCard(source: MenuSource, onCloseRecording: () -> Unit, onReconnect: () -> Unit, modifier: Modifier) {
    val colors = MaterialTheme.colorScheme
    val icon: Int
    val heading: String
    val title: String
    var status: String? = null
    var statusColor: Color = colors.onSurfaceVariant
    var dot = false
    var action: Pair<String, () -> Unit>? = null
    when (source) {
        is MenuSource.Recording -> {
            icon = R.drawable.ic_recording
            heading = "Playing recording"
            title = source.title
            action = "Close" to onCloseRecording
        }
        is MenuSource.Desktop -> {
            icon = R.drawable.ic_desktop
            heading = "Paired desktop"
            title = source.name.ifEmpty { "Desktop" }
            dot = true
            when (source.state) {
                "connected" -> {
                    status = "Connected"
                    statusColor = colors.primary
                }
                "connecting", "reconnecting", "starting" -> {
                    status = if (source.state == "reconnecting") "Reconnecting…" else "Connecting…"
                    statusColor = colors.tertiary
                }
                else -> {
                    status = source.detail ?: "Disconnected"
                    statusColor = colors.error
                    action = "Reconnect" to onReconnect
                }
            }
        }
        MenuSource.Direct -> {
            icon = R.drawable.ic_sensors
            heading = "Live"
            title = "Direct UDP"
            status = "From the game on this network"
        }
    }

    Surface(modifier.fillMaxWidth(), shape = MaterialTheme.shapes.large, color = colors.surfaceContainerHigh) {
        Row(
            Modifier.padding(start = 16.dp, end = 12.dp, top = 14.dp, bottom = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Box(
                Modifier.size(44.dp).background(colors.primaryContainer, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Icon(painterResource(icon), contentDescription = null, tint = colors.onPrimaryContainer)
            }
            Column(Modifier.weight(1f)) {
                Text(
                    heading.uppercase(),
                    style = MaterialTheme.typography.labelSmall,
                    color = colors.onSurfaceVariant,
                )
                Text(
                    title,
                    style = MaterialTheme.typography.titleMedium,
                    color = colors.onSurface,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                status?.let {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (dot) {
                            Box(Modifier.size(8.dp).background(statusColor, CircleShape))
                            Spacer(Modifier.size(6.dp))
                        }
                        Text(
                            it,
                            style = MaterialTheme.typography.bodySmall,
                            color = if (dot) statusColor else colors.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
            }
            action?.let { (label, onClick) ->
                FilledTonalButton(onClick = onClick) { Text(label) }
            }
        }
    }
}

@Composable
private fun MenuRow(@DrawableRes icon: Int, title: String, supporting: String, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(supporting) },
        leadingContent = { Icon(painterResource(icon), contentDescription = null) },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.clickable(onClick = onClick).padding(horizontal = 8.dp),
    )
}

/**
 * The driver the app shows, with the desktop driver selector's wording for
 * their telemetry access. Picks another driver in a recording; paired, the
 * desktop chooses.
 */
@Composable
private fun DriverRow(driver: MenuDriver, onPick: (() -> Unit)?) {
    val access = when (driver.restricted) {
        true -> "Public data only"
        null -> "Telemetry access unknown"
        false -> null
    }
    val supporting = listOfNotNull(access, if (onPick == null) "Chosen on the desktop" else null).joinToString(" · ")
    ListItem(
        overlineContent = { Text("Driver") },
        headlineContent = { Text(driver.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = if (supporting.isEmpty()) null else {
            { Text(supporting) }
        },
        leadingContent = { Icon(painterResource(R.drawable.ic_person), contentDescription = null) },
        trailingContent = if (onPick == null) null else {
            { Icon(painterResource(R.drawable.ic_chevron_right), contentDescription = "Change driver") }
        },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = (if (onPick != null) Modifier.clickable(onClick = onPick) else Modifier).padding(horizontal = 8.dp),
    )
}
