package com.tracknrace.android

import android.Manifest
import android.content.ContentUris
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.MediaStore
import android.provider.Settings
import java.io.Closeable
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import org.json.JSONObject

/** One .tnrd recording found on the phone. */
internal data class RecordingFile(
    /** Name, size and time: the same file found two ways is listed once. */
    val key: String,
    val name: String,
    /** "Monza · Race": the recording's own header, else its file name. */
    val title: String,
    /** "F1 25 · 20 drivers"; may be empty. */
    val detail: String,
    val sizeBytes: Long,
    val modifiedMs: Long,
    /** When the session was recorded, from the header; the file's time when unknown. */
    val recordedMs: Long = modifiedMs,
    /** The recording's length in seconds, when its index says. */
    val durationSeconds: Double? = null,
    /** A readable path, when the file can be opened directly. */
    val path: String?,
    /** A content URI, for files in a folder picked through the system picker. */
    val uri: Uri?,
)

/**
 * Finds every recording on the phone the way a gallery finds photos: by asking
 * the system's media index, which with "All files access" covers every file in
 * shared storage. Folders picked through the system picker are walked as well,
 * as is this app's own recording folder. Without "All files access" only those
 * folders are searched.
 */
internal object RecordingLibrary {
    private const val PREF_FOLDERS = "recordings.folders"
    private const val EXTENSION = ".tnrd"
    private const val MAX_FOLDER_DEPTH = 8

    /** Whether the media index may list every file, not only media. */
    fun hasAllFilesAccess(context: Context): Boolean = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
        Environment.isExternalStorageManager()
    } else {
        context.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
    }

    /** Android 11+: the system page that grants "All files access" to this app. */
    fun allFilesAccessIntent(context: Context): Intent =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:${context.packageName}"))
        } else {
            Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:${context.packageName}"))
        }

    fun folderPickerIntent(): Intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)

    /** Folders picked for recordings, kept across restarts. */
    fun folders(context: Context): List<Uri> =
        RecordingStorage.preferences(context).getStringSet(PREF_FOLDERS, emptySet()).orEmpty()
            .map(Uri::parse)
            .sortedBy { it.toString() }

    /** Keeps read access to a picked folder and searches it from now on. */
    fun addFolder(context: Context, uri: Uri): Boolean = try {
        context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        val preferences = RecordingStorage.preferences(context)
        val folders = preferences.getStringSet(PREF_FOLDERS, emptySet()).orEmpty() + uri.toString()
        preferences.edit().putStringSet(PREF_FOLDERS, folders).apply()
        true
    } catch (_: SecurityException) {
        false
    }

    fun removeFolder(context: Context, uri: Uri) {
        val preferences = RecordingStorage.preferences(context)
        val folders = preferences.getStringSet(PREF_FOLDERS, emptySet()).orEmpty() - uri.toString()
        preferences.edit().putStringSet(PREF_FOLDERS, folders).apply()
        try {
            context.contentResolver.releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (_: SecurityException) {
            // Already gone.
        }
    }

    /** A folder's name as the picker showed it, e.g. "Download/Recordings". */
    fun folderLabel(uri: Uri): String = try {
        val id = Uri.decode(DocumentsContract.getTreeDocumentId(uri))
        id.substringAfter(':').ifEmpty { id.substringBefore(':') }
    } catch (_: IllegalArgumentException) {
        uri.lastPathSegment.orEmpty()
    }

    /** Every recording found, newest first. Blocking: call off the UI thread. */
    fun scan(context: Context): List<RecordingFile> {
        val found = LinkedHashMap<String, RecordingFile>()
        fun add(file: RecordingFile) {
            found.putIfAbsent(file.key, file)
        }
        if (hasAllFilesAccess(context)) scanMediaIndex(context, ::add)
        // The app's own recordings wait in its private folder until moved.
        try {
            RecordingStorage.stagingDirectory(context).listFiles()
                ?.filter { it.isFile && it.name.endsWith(EXTENSION, ignoreCase = true) }
                ?.forEach { add(fileEntry(it)) }
        } catch (_: IllegalStateException) {
            // No private folder: nothing recorded there.
        }
        val trees = (folders(context) + listOfNotNull(RecordingStorage.selectedDirectory(context))).distinct()
        for (tree in trees) scanTree(context, tree, ::add)
        return found.values.map { withDetails(context, it) }.sortedByDescending { it.recordedMs }
    }

    /** Header details by file key; a file's details never change while it keeps its size and time. */
    private val details = ConcurrentHashMap<String, Details>()

    private class Details(val title: String, val detail: String, val recordedMs: Long?, val durationSeconds: Double?)

    /** The file with its header's track, session, game, drivers, date and length, when it has an index. */
    private fun withDetails(context: Context, file: RecordingFile): RecordingFile {
        val known = details[file.key] ?: readDetails(context, file)?.also { details[file.key] = it } ?: return file
        return file.copy(
            title = known.title.ifEmpty { file.title },
            detail = known.detail.ifEmpty { file.detail },
            recordedMs = known.recordedMs ?: file.recordedMs,
            durationSeconds = known.durationSeconds,
        )
    }

    private fun readDetails(context: Context, file: RecordingFile): Details? {
        val json = try {
            open(context, file).use { handle -> NativeTelemetry.nativeSummarize(handle.path) }
        } catch (_: Exception) {
            null
        } ?: return null
        val row = JSONObject(json)
        val track = row.optString("trackName").takeUnless { it.isEmpty() || it == "Unknown" }
        val session = row.optString("sessionName").takeUnless { it.isEmpty() || it == "Unknown" }
        val protocol = row.optInt("protocol")
        val drivers = row.optInt("drivers")
        val detail = listOfNotNull(
            if (protocol >= 2000) "F1 ${protocol % 100}" else null,
            when {
                drivers == 1 -> "Your car"
                drivers > 1 -> "$drivers drivers"
                else -> null
            },
        ).joinToString(" · ")
        return Details(
            title = listOfNotNull(track, session).joinToString(" · "),
            detail = detail,
            recordedMs = row.optLong("startTime").takeIf { it > 0 },
            durationSeconds = row.optDouble("duration", 0.0).takeIf { it > 0.0 },
        )
    }

    private fun scanMediaIndex(context: Context, add: (RecordingFile) -> Unit) {
        val collection = MediaStore.Files.getContentUri("external")
        @Suppress("DEPRECATION")
        val projection = arrayOf(
            MediaStore.Files.FileColumns._ID,
            MediaStore.Files.FileColumns.DISPLAY_NAME,
            MediaStore.Files.FileColumns.SIZE,
            MediaStore.Files.FileColumns.DATE_MODIFIED,
            MediaStore.Files.FileColumns.DATA,
        )
        try {
            context.contentResolver.query(
                collection,
                projection,
                "${MediaStore.Files.FileColumns.DISPLAY_NAME} LIKE ?",
                arrayOf("%$EXTENSION"),
                null,
            )?.use { cursor ->
                while (cursor.moveToNext()) {
                    val name = cursor.getString(1) ?: continue
                    if (!name.endsWith(EXTENSION, ignoreCase = true)) continue
                    val path = cursor.getString(4)?.takeIf { File(it).canRead() }
                    val uri = ContentUris.withAppendedId(collection, cursor.getLong(0))
                    add(entry(name, cursor.getLong(2), cursor.getLong(3) * 1000, path, if (path == null) uri else null))
                }
            }
        } catch (_: SecurityException) {
            // Access was withdrawn while scanning.
        }
    }

    private fun scanTree(context: Context, tree: Uri, add: (RecordingFile) -> Unit) {
        val resolver = context.contentResolver
        val root = try {
            DocumentsContract.getTreeDocumentId(tree)
        } catch (_: IllegalArgumentException) {
            return
        }
        val pending = ArrayDeque(listOf(root to 0))
        val projection = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        )
        while (pending.isNotEmpty()) {
            val (parent, depth) = pending.removeFirst()
            try {
                resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent), projection, null, null, null)?.use { cursor ->
                    while (cursor.moveToNext()) {
                        val id = cursor.getString(0) ?: continue
                        val name = cursor.getString(1).orEmpty()
                        if (cursor.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR) {
                            if (depth < MAX_FOLDER_DEPTH && !name.startsWith(".")) pending.addLast(id to depth + 1)
                        } else if (name.endsWith(EXTENSION, ignoreCase = true)) {
                            add(entry(name, cursor.getLong(3), cursor.getLong(4), null, DocumentsContract.buildDocumentUriUsingTree(tree, id)))
                        }
                    }
                }
            } catch (_: SecurityException) {
                return // The folder's grant was withdrawn.
            } catch (_: IllegalArgumentException) {
                return
            }
        }
    }

    private fun fileEntry(file: File) = entry(file.name, file.length(), file.lastModified(), file.absolutePath, null)

    private fun entry(name: String, size: Long, modifiedMs: Long, path: String?, uri: Uri?): RecordingFile {
        val (title, detail) = describe(name)
        return RecordingFile(
            key = "$name|$size|${modifiedMs / 1000}",
            name = name,
            title = title,
            detail = detail,
            sizeBytes = size,
            modifiedMs = modifiedMs,
            path = path,
            uri = uri,
        )
    }

    // The recorder's names: f1_24_20_azerbaijan_grand_prix_race_2026-08-16T09-30-53-240Z.tnrd,
    // that is game, track id, track, session and start time (UTC), and since
    // V6 a -all or -driver scope.
    private val RECORDER_NAME = Regex("""^f1_(\d{2})_\d+_(.+)_\d{4}-\d{2}-\d{2}T[\dZ-]+?(?:-(all|driver))?\.tnrd$""", RegexOption.IGNORE_CASE)
    private val SESSIONS = listOf(
        "Practice 1", "Practice 2", "Practice 3", "Short Practice",
        "Qualifying 1", "Qualifying 2", "Qualifying 3", "Short Qualifying", "One-Shot Qualifying",
        "Sprint Shootout 1", "Sprint Shootout 2", "Sprint Shootout 3", "Short Sprint Shootout",
        "One-Shot Sprint Shootout", "Race 2", "Race 3", "Race", "Time Trial", "Unknown",
    )

    /** Title and detail from a recorder file name; other names are shown as they are. */
    private fun describe(name: String): Pair<String, String> {
        val match = RECORDER_NAME.matchEntire(name) ?: return name.removeSuffix(EXTENSION) to ""
        val (game, trackAndSession, scope) = match.destructured
        val session = SESSIONS.firstOrNull { trackAndSession.endsWith("_" + sanitize(it)) }
        val track = (if (session != null) trackAndSession.removeSuffix("_" + sanitize(session)) else trackAndSession)
            .split('_').filter(String::isNotEmpty)
            .joinToString(" ") { word -> word.replaceFirstChar(Char::uppercaseChar) }
        val title = listOfNotNull(track.ifEmpty { null }, session).joinToString(" · ")
        val detail = listOfNotNull(
            "F1 $game",
            when (scope.lowercase()) {
                "all" -> "All drivers"
                "driver" -> "Your car"
                else -> null
            },
        ).joinToString(" · ")
        return title to detail
    }

    private fun sanitize(value: String) = value.lowercase().map { if (it.isLetterOrDigit()) it else '_' }.joinToString("")

    /** An opened recording for the engine: a path it can read, held open until [close]. */
    class Handle(val path: String, private val descriptor: ParcelFileDescriptor?) : Closeable {
        override fun close() {
            descriptor?.close()
        }
    }

    /**
     * Opens [file] for the engine. A content URI is opened as a descriptor and
     * handed over as /proc/self/fd/N, which the engine can open again for its
     * readers as long as the descriptor stays open.
     */
    fun open(context: Context, file: RecordingFile): Handle {
        file.path?.let { if (File(it).canRead()) return Handle(it, null) }
        val uri = requireNotNull(file.uri) { "This recording can no longer be read." }
        val descriptor = requireNotNull(context.contentResolver.openFileDescriptor(uri, "r")) {
            "This recording could not be opened."
        }
        return Handle("/proc/self/fd/${descriptor.fd}", descriptor)
    }
}
