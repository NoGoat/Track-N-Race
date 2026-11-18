package com.nogoat.tracknrace.pages

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.nogoat.tracknrace.AppUpdater
import com.nogoat.tracknrace.AvailableUpdate
import com.nogoat.tracknrace.R
import com.nogoat.tracknrace.UpdateProgress
import java.util.Locale

/** The desktop's Update Available dialog (UpdateAvailableDialog.tsx), as an M3 dialog. */
@Composable
internal fun UpdateDialog(update: AvailableUpdate, progress: UpdateProgress) {
    val context = LocalContext.current
    val working = progress is UpdateProgress.Downloading ||
        progress is UpdateProgress.Verifying ||
        progress is UpdateProgress.Installing
    val failed = progress as? UpdateProgress.Failed
    // Back from "Install unknown apps": go on only if it was allowed.
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        if (AppUpdater.canInstall(context)) {
            AppUpdater.install(context)
        } else {
            AppUpdater.fail("Track N Race isn't allowed to install apps. Allow it, or use Download.")
        }
    }
    val installable = update.file != null && failed == null

    AlertDialog(
        onDismissRequest = AppUpdater::dismiss,
        properties = DialogProperties(dismissOnBackPress = !working, dismissOnClickOutside = false),
        icon = { Icon(painterResource(R.drawable.ic_download), null) },
        title = { Text("Update available") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
                Text(
                    "Track N Race ${update.latestVersion} is available.",
                    style = MaterialTheme.typography.bodyLarge,
                )
                if (update.file == null) {
                    Text(
                        "Download opens the GitHub release page.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Surface(
                    Modifier.fillMaxWidth(),
                    shape = MaterialTheme.shapes.medium,
                    color = MaterialTheme.colorScheme.surfaceContainerHighest,
                ) {
                    Row(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
                        VersionCell("Installed", update.installedVersion, Modifier.weight(1f))
                        VersionCell("Latest", update.latestVersion, Modifier.weight(1f))
                    }
                }
                if (working) {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        if (progress is UpdateProgress.Downloading && progress.total > 0) {
                            LinearProgressIndicator(
                                progress = { (progress.received.toFloat() / progress.total).coerceIn(0f, 1f) },
                                modifier = Modifier.fillMaxWidth(),
                            )
                        } else {
                            LinearProgressIndicator(Modifier.fillMaxWidth())
                        }
                        Text(
                            progressLabel(progress),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                if (failed != null) {
                    Text(
                        "Update failed: ${failed.message}",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        },
        confirmButton = {
            if (installable) {
                TextButton(
                    enabled = !working,
                    onClick = {
                        if (AppUpdater.canInstall(context)) {
                            AppUpdater.install(context)
                        } else {
                            permission.launch(AppUpdater.installPermissionIntent(context))
                        }
                    },
                ) { Text("Update now") }
            } else {
                TextButton(enabled = !working, onClick = { AppUpdater.openReleasePage(context) }) { Text("Download") }
            }
        },
        dismissButton = {
            Row {
                TextButton(enabled = !working, onClick = { AppUpdater.skip(context) }) { Text("Skip this version") }
                TextButton(enabled = !working, onClick = AppUpdater::dismiss) { Text("Remind me later") }
            }
        },
    )
}

@Composable
private fun VersionCell(label: String, version: String, modifier: Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(
            version,
            style = MaterialTheme.typography.titleMedium.copy(fontFamily = FontFamily.Monospace),
            fontWeight = FontWeight.SemiBold,
        )
    }
}

private fun progressLabel(progress: UpdateProgress): String = when (progress) {
    is UpdateProgress.Downloading ->
        "Downloading ${megabytes(progress.received)} / ${megabytes(progress.total)} MB"
    UpdateProgress.Verifying -> "Verifying…"
    UpdateProgress.Installing -> "Installing…"
    else -> ""
}

private fun megabytes(bytes: Long) = String.format(Locale.US, "%.1f", bytes / (1024.0 * 1024.0))
