package com.nogoat.tracknrace.pages

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.nogoat.tracknrace.AndroidSettings
import com.nogoat.tracknrace.PairedTelemetryClient
import com.nogoat.tracknrace.R
import com.nogoat.tracknrace.ui.theme.ThemeMode
import com.nogoat.tracknrace.ui.wheelDisplay

// ── Design ──────────────────────────────────────────────────────────────────
// Material 3 settings: a small label over each group, and every setting one
// full-width row in the group's container. A row is a switch, a link to a
// page or picker, an action, or a segmented choice; nothing else.

@Composable
internal fun SettingsScreen(
    settings: AndroidSettings,
    onSource: (String) -> Unit,
    onPairing: () -> Unit,
    onRecording: (Boolean) -> Unit,
    onThemeMode: (ThemeMode) -> Unit,
    onWheelDisplay: (Boolean) -> Unit,
    onChooseDirectory: () -> Unit,
    onDefaultDirectory: () -> Unit,
    onForgetDesktop: () -> Unit,
    onLicenses: () -> Unit,
) {
    val context = LocalContext.current
    val version = remember(context) {
        runCatching { context.packageManager.getPackageInfo(context.packageName, 0).versionName }.getOrNull()
    }
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
    ) {
        item {
            SettingsGroup("Appearance") {
                ChoiceRow(
                    title = "Theme",
                    options = listOf("System", "Light", "Dark"),
                    selected = settings.themeMode.ordinal,
                    onSelect = { onThemeMode(ThemeMode.entries[it]) },
                )
                GroupDivider()
                val wheel = wheelDisplay(settings.wheelDisplay, LocalConfiguration.current)
                SwitchRow(
                    title = "Wheel display",
                    supporting = if (wheel) "Landscape hides the bars until you tap" else "Landscape keeps the bars on screen",
                    checked = wheel,
                    onCheckedChange = onWheelDisplay,
                )
            }
        }
        item {
            SettingsGroup("Telemetry") {
                val paired = settings.source == PairedTelemetryClient.SOURCE_PAIRED
                ChoiceRow(
                    title = "Source",
                    supporting = if (paired) {
                        "Live or playback telemetry from Track N Race desktop"
                    } else {
                        "UDP from the game on this network, port 20777"
                    },
                    options = listOf("Direct UDP", "Paired desktop"),
                    selected = if (paired) 1 else 0,
                    onSelect = { index ->
                        when {
                            index == 0 -> onSource(PairedTelemetryClient.SOURCE_DIRECT)
                            settings.hasSavedDesktop -> onSource(PairedTelemetryClient.SOURCE_PAIRED)
                            // Paired mode needs a desktop first.
                            else -> onPairing()
                        }
                    },
                )
                GroupDivider()
                LinkRow(
                    title = if (settings.hasSavedDesktop) settings.desktopName else "Pair a desktop",
                    supporting = if (settings.hasSavedDesktop) {
                        "Manage or replace the saved desktop"
                    } else {
                        "Scan the desktop's QR code or enter its code"
                    },
                    onClick = onPairing,
                )
                if (settings.hasSavedDesktop) {
                    GroupDivider()
                    ActionRow("Forget paired desktop", onForgetDesktop, destructive = true)
                }
            }
        }
        item {
            SettingsGroup("Recording") {
                SwitchRow(
                    title = "Record sessions",
                    supporting = if (settings.recordingEnabled) {
                        "Saving complete sessions as TNRD V6 files"
                    } else {
                        "Off · saves complete sessions as TNRD V6 files"
                    },
                    checked = settings.recordingEnabled,
                    onCheckedChange = onRecording,
                )
                GroupDivider()
                LinkRow(
                    title = "Recording folder",
                    supporting = settings.recordingDirectory.ifEmpty { "App Documents folder" },
                    onClick = onChooseDirectory,
                )
                if (settings.usingCustomDirectory) {
                    GroupDivider()
                    ActionRow("Use the app Documents folder", onDefaultDirectory)
                }
            }
        }
        item {
            SettingsGroup("About") {
                LinkRow(
                    title = "Licenses",
                    supporting = "Track N Race and the open-source software it uses",
                    onClick = onLicenses,
                )
            }
            Text(
                listOfNotNull("Track N Race for Android", version?.let { "Version $it" }).joinToString(" · "),
                Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 16.dp),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/** A group's label, then its rows in one container. */
@Composable
private fun SettingsGroup(title: String, content: @Composable ColumnScope.() -> Unit) {
    Text(
        title,
        Modifier.padding(start = 16.dp, top = 16.dp, bottom = 8.dp),
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
    )
    Surface(
        Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainerLow,
    ) {
        Column(content = content)
    }
}

@Composable
private fun GroupDivider() {
    HorizontalDivider(Modifier.padding(horizontal = 16.dp), color = MaterialTheme.colorScheme.outlineVariant)
}

private val RowColors @Composable get() = ListItemDefaults.colors(containerColor = Color.Transparent)

@Composable
private fun SwitchRow(title: String, supporting: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(supporting) },
        // The whole row toggles, so the switch itself takes no clicks.
        trailingContent = { Switch(checked = checked, onCheckedChange = null) },
        colors = RowColors,
        modifier = Modifier.fillMaxWidth().clickable(role = Role.Switch) { onCheckedChange(!checked) },
    )
}

@Composable
private fun LinkRow(title: String, supporting: String, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = { Text(supporting, maxLines = 2, overflow = TextOverflow.Ellipsis) },
        trailingContent = { Icon(painterResource(R.drawable.ic_chevron_right), null) },
        colors = RowColors,
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick),
    )
}

/** An action that happens at once, named in the primary or error colour. */
@Composable
private fun ActionRow(title: String, onClick: () -> Unit, destructive: Boolean = false) {
    ListItem(
        headlineContent = {
            Text(
                title,
                color = if (destructive) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.primary,
            )
        },
        colors = RowColors,
        modifier = Modifier.fillMaxWidth().clickable(role = Role.Button, onClick = onClick),
    )
}

/** A setting with a few exclusive values, as an M3 segmented button under its name. */
@Composable
private fun ChoiceRow(
    title: String,
    options: List<String>,
    selected: Int,
    onSelect: (Int) -> Unit,
    supporting: String? = null,
) {
    Column(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            if (supporting != null) {
                Text(
                    supporting,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
            options.forEachIndexed { index, label ->
                SegmentedButton(
                    selected = index == selected,
                    onClick = { onSelect(index) },
                    shape = SegmentedButtonDefaults.itemShape(index, options.size),
                ) {
                    Text(label, maxLines = 1)
                }
            }
        }
    }
}
