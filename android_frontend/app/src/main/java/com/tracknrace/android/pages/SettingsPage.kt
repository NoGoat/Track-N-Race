package com.tracknrace.android.pages

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.tracknrace.android.AndroidSettings
import com.tracknrace.android.PairedTelemetryClient
import com.tracknrace.android.R

@Composable
internal fun SettingsScreen(
    settings: AndroidSettings,
    onSource: (String) -> Unit,
    onPairing: () -> Unit,
    onRecording: (Boolean) -> Unit,
    onTimingOneLine: (Boolean) -> Unit,
    onChooseDirectory: () -> Unit,
    onDefaultDirectory: () -> Unit,
    onForgetDesktop: () -> Unit,
    onLicenses: () -> Unit,
) {
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            SettingsSection(
                title = "Telemetry source",
                supporting = "Choose where this phone receives its live timing data.",
            ) {
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    FilterChip(
                        selected = settings.source == PairedTelemetryClient.SOURCE_DIRECT,
                        onClick = { onSource(PairedTelemetryClient.SOURCE_DIRECT) },
                        label = { Text("Direct UDP") },
                    )
                    FilterChip(
                        selected = settings.source == PairedTelemetryClient.SOURCE_PAIRED,
                        onClick = {
                            if (settings.hasSavedDesktop) onSource(PairedTelemetryClient.SOURCE_PAIRED)
                            else onPairing()
                        },
                        label = { Text("Paired desktop") },
                    )
                }
                HorizontalDivider()
                SettingsRow(
                    title = if (settings.hasSavedDesktop) settings.desktopName else "Pair a desktop",
                    supporting = if (settings.hasSavedDesktop) {
                        "Manage or replace the saved desktop"
                    } else {
                        "Receive live or playback telemetry from Track N Race desktop"
                    },
                    onClick = onPairing,
                )
                if (settings.hasSavedDesktop) {
                    TextButton(onClick = onForgetDesktop) {
                        Text("Forget paired desktop", color = MaterialTheme.colorScheme.error)
                    }
                }
            }
        }
        item {
            SettingsSection(
                title = "Timing tower",
                supporting = "Choose how much vertical space each driver uses.",
            ) {
                ListItem(
                    headlineContent = { Text("One-line rows") },
                    supportingContent = {
                        Text(if (settings.timingOneLine) "Compact single-line layout" else "Detailed two-line layout")
                    },
                    trailingContent = {
                        Switch(checked = settings.timingOneLine, onCheckedChange = onTimingOneLine)
                    },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                )
            }
        }
        item {
            SettingsSection(
                title = "Recording",
                supporting = "Capture complete sessions as portable TNRD V5 files.",
            ) {
                ListItem(
                    headlineContent = { Text("Record telemetry sessions") },
                    supportingContent = {
                        Text(if (settings.recordingEnabled) "Recording is enabled" else "Off by default")
                    },
                    trailingContent = {
                        Switch(checked = settings.recordingEnabled, onCheckedChange = onRecording)
                    },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                )
                HorizontalDivider()
                SettingsRow(
                    title = "Recording folder",
                    supporting = settings.recordingDirectory.ifEmpty { "App Documents folder" },
                    onClick = onChooseDirectory,
                )
                if (settings.usingCustomDirectory) {
                    TextButton(onClick = onDefaultDirectory) {
                        Text("Use the app Documents folder")
                    }
                }
            }
        }
        item {
            SettingsSection(
                title = "About",
                supporting = "Track N Race Android · Native telemetry dashboard",
            ) {
                SettingsRow(
                    "Open-source licences",
                    "Libraries, notices, and software licences",
                    onLicenses,
                )
            }
        }
    }
}

@Composable
private fun SettingsSection(
    title: String,
    supporting: String,
    content: @Composable () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
        shape = MaterialTheme.shapes.extraLarge,
    ) {
        Column(
            Modifier.fillMaxWidth().padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                Text(title, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
                Text(
                    supporting,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            content()
        }
    }
}

@Composable
private fun SettingsRow(title: String, supporting: String, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(supporting, maxLines = 2, overflow = TextOverflow.Ellipsis) },
        trailingContent = { Icon(painterResource(R.drawable.ic_chevron_right), null) },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick),
    )
}
