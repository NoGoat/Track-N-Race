package com.tracknrace.android.pages

import android.net.Uri
import android.text.format.Formatter
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.InputChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.tracknrace.android.R
import com.tracknrace.android.RecordingFile
import com.tracknrace.android.RecordingLibrary
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * The recordings in the app's folder and the added folders, newest first and
 * grouped by day, the way a gallery lists photos. Picking one opens it.
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
internal fun RecordingsSheet(
    /** The recording open now, marked in the list. */
    openTitle: String?,
    /** Changes when a folder is added or removed, to search again. */
    foldersVersion: Int,
    onOpen: (RecordingFile) -> Unit,
    onAddFolder: () -> Unit,
    onRemoveFolder: (Uri) -> Unit,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current.applicationContext
    var scans by remember { mutableIntStateOf(0) }
    // Any return to the app: files may have arrived meanwhile.
    LifecycleResumeEffect(Unit) {
        scans++
        onPauseOrDispose { }
    }
    var folders by remember { mutableStateOf(RecordingLibrary.folders(context)) }
    var recordings by remember { mutableStateOf<List<RecordingFile>?>(null) }
    var scanning by remember { mutableStateOf(true) }
    LaunchedEffect(scans, foldersVersion) {
        // The resume effect counts the sheet's own opening too: wait for it,
        // so one opening searches once rather than twice at the same time.
        if (scans == 0) return@LaunchedEffect
        scanning = true
        folders = RecordingLibrary.folders(context)
        recordings = withContext(Dispatchers.IO) { RecordingLibrary.scan(context) }
        scanning = false
    }
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        // Always the full height: a sheet whose content is about as tall as the
        // screen never settles, as its status bar padding moves where it rests.
        Column(Modifier.fillMaxWidth().fillMaxHeight()) {
            Row(
                Modifier.fillMaxWidth().padding(start = 24.dp, end = 12.dp, bottom = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text("Recordings", style = MaterialTheme.typography.titleLarge)
                    recordings?.let {
                        Text(
                            if (it.size == 1) "1 recording" else "${it.size} recordings",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                IconButton(onClick = onAddFolder) {
                    Icon(painterResource(R.drawable.ic_create_new_folder), contentDescription = "Add a folder")
                }
            }
            if (folders.isNotEmpty()) {
                FlowRow(
                    Modifier.fillMaxWidth().padding(horizontal = 24.dp, vertical = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    for (folder in folders) {
                        InputChip(
                            selected = false,
                            onClick = { onRemoveFolder(folder) },
                            label = { Text(RecordingLibrary.folderLabel(folder), maxLines = 1, overflow = TextOverflow.Ellipsis) },
                            leadingIcon = { Icon(painterResource(R.drawable.ic_folder), contentDescription = null, Modifier.size(18.dp)) },
                            trailingIcon = { Icon(painterResource(R.drawable.ic_close), contentDescription = "Stop searching this folder", Modifier.size(18.dp)) },
                        )
                    }
                }
            }
            if (scanning) {
                LinearProgressIndicator(Modifier.fillMaxWidth().padding(horizontal = 24.dp, vertical = 8.dp))
            } else {
                Spacer(Modifier.height(4.dp))
            }
            val list = recordings
            if (list != null && list.isEmpty() && !scanning) {
                EmptyRecordings(onAddFolder)
            } else if (list != null) {
                RecordingList(list, openTitle, onOpen)
            }
        }
    }
}

@Composable
private fun EmptyRecordings(onAddFolder: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().padding(vertical = 48.dp, horizontal = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Icon(
            painterResource(R.drawable.ic_recording),
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(40.dp),
        )
        Spacer(Modifier.height(12.dp))
        Text("No recordings found", style = MaterialTheme.typography.titleMedium)
        Text(
            "Add the folder your .tnrd files are in.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(16.dp))
        FilledTonalButton(onClick = onAddFolder) { Text("Add folder") }
    }
}

/** Recordings under a heading for each day, newest first. */
@Composable
private fun RecordingList(recordings: List<RecordingFile>, openTitle: String?, onOpen: (RecordingFile) -> Unit) {
    val zone = remember { ZoneId.systemDefault() }
    val today = remember { LocalDate.now(zone) }
    val days = remember(recordings) {
        recordings.groupBy { Instant.ofEpochMilli(it.recordedMs).atZone(zone).toLocalDate() }.toList()
    }
    val dayFormat = remember { DateTimeFormatter.ofPattern("EEEE, d MMMM") }
    val yearFormat = remember { DateTimeFormatter.ofLocalizedDate(FormatStyle.LONG) }
    val timeFormat = remember { DateTimeFormatter.ofLocalizedTime(FormatStyle.SHORT) }
    val context = LocalContext.current
    LazyColumn(Modifier.fillMaxWidth()) {
        for ((day, files) in days) {
            item(key = "day-$day") {
                Text(
                    when {
                        day == today -> "Today"
                        day == today.minusDays(1) -> "Yesterday"
                        day.year == today.year -> day.format(dayFormat)
                        else -> day.format(yearFormat)
                    },
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 16.dp, bottom = 4.dp),
                )
            }
            items(files, key = { it.key }) { file ->
                val time = Instant.ofEpochMilli(file.recordedMs).atZone(zone).format(timeFormat)
                val size = Formatter.formatShortFileSize(context, file.sizeBytes)
                val open = file.title == openTitle
                val supporting = listOfNotNull(file.detail.ifEmpty { null }, file.durationSeconds?.let(::sessionLength))
                    .joinToString(" · ")
                ListItem(
                    headlineContent = { Text(file.title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    supportingContent = if (supporting.isEmpty()) null else {
                        { Text(supporting, maxLines = 1, overflow = TextOverflow.Ellipsis) }
                    },
                    trailingContent = {
                        Column(horizontalAlignment = Alignment.End) {
                            Text(time, style = MaterialTheme.typography.labelMedium)
                            Text(size, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    },
                    leadingContent = {
                        Box(
                            Modifier.size(40.dp).clip(CircleShape).background(
                                if (open) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.secondaryContainer,
                            ),
                            contentAlignment = Alignment.Center,
                        ) {
                            Icon(
                                painterResource(if (open) R.drawable.ic_play else R.drawable.ic_recording),
                                contentDescription = null,
                                tint = if (open) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSecondaryContainer,
                                modifier = Modifier.size(20.dp),
                            )
                        }
                    },
                    colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
                    modifier = Modifier.clickable { onOpen(file) },
                )
            }
        }
        item { Spacer(Modifier.height(24.dp)) }
    }
}

/** A session's length as people say it: "48 min", "1 h 12 min", "35 s". */
private fun sessionLength(seconds: Double): String {
    val whole = seconds.toLong()
    return when {
        whole < 60 -> "$whole s"
        whole < 3600 -> "${whole / 60} min"
        else -> "${whole / 3600} h ${whole / 60 % 60} min"
    }
}
