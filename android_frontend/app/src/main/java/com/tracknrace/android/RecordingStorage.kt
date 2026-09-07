package com.tracknrace.android

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import java.io.File
import java.io.FileInputStream
import java.util.concurrent.Executors

/** Bridges libtnrp's filesystem writer to user-selected Android document trees. */
internal object RecordingStorage {
    const val PREFS = "track_n_race_android"
    const val PREF_RECORDING = "recording_enabled"

    private const val PREF_DIRECTORY_URI = "recording_directory_uri"
    private const val TNRD_MIME_TYPE = "application/octet-stream"
    private const val COPY_BUFFER_SIZE = 256 * 1024
    private val exportExecutor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "tnrp-recording-export").apply { isDaemon = true }
    }

    fun interface ExportCallback {
        fun onComplete(result: ExportResult)
    }

    data class ExportResult(
        val movedFiles: Int,
        val error: String?,
    )

    fun preferences(context: Context): SharedPreferences =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun stagingDirectory(context: Context): File {
        val documents = context.getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS)
            ?: context.filesDir
        val directory = File(documents, "Track N Race")
        check(!directory.exists() || directory.isDirectory) {
            "Track N Race exists at the recording path but is not a folder."
        }
        check(directory.isDirectory || directory.mkdirs() || directory.isDirectory) {
            "Could not create the Track N Race folder."
        }
        check(directory.canWrite()) { "The Track N Race folder is not writable." }
        return directory
    }

    fun selectedDirectory(context: Context): Uri? =
        preferences(context).getString(PREF_DIRECTORY_URI, null)
            ?.takeIf(String::isNotEmpty)
            ?.let(Uri::parse)

    fun setSelectedDirectory(context: Context, directory: Uri) {
        preferences(context).edit().putString(PREF_DIRECTORY_URI, directory.toString()).apply()
    }

    fun clearSelectedDirectory(context: Context) {
        val directory = selectedDirectory(context)
        preferences(context).edit().remove(PREF_DIRECTORY_URI).apply()
        if (directory == null) return
        try {
            context.contentResolver.releasePersistableUriPermission(
                directory,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
            )
        } catch (_: SecurityException) {
            // The provider may already have revoked the grant.
        }
    }

    fun selectedDirectoryLabel(context: Context): String {
        val directory = selectedDirectory(context)
        if (directory == null) {
            return try {
                stagingDirectory(context).absolutePath
            } catch (_: IllegalStateException) {
                context.getString(R.string.settings_storage_unavailable)
            }
        }

        return try {
            val documentId = Uri.decode(DocumentsContract.getTreeDocumentId(directory))
            val rootSeparator = documentId.indexOf(':')
            val relative = if (rootSeparator >= 0) documentId.substring(rootSeparator + 1) else documentId
            relative.ifEmpty { context.getString(R.string.settings_storage_root) }
        } catch (_: IllegalArgumentException) {
            directory.lastPathSegment?.let(Uri::decode) ?: directory.toString()
        }
    }

    fun exportCompletedRecordingsAsync(context: Context, callback: ExportCallback?) {
        val appContext = context.applicationContext
        val selectedTree = selectedDirectory(appContext)
        var completedFiles: Array<File>? = null
        var preparationError: String? = null
        try {
            completedFiles = stagingDirectory(appContext).listFiles { file: File ->
                file.isFile && file.name.endsWith(".tnrd")
            }
        } catch (error: IllegalStateException) {
            preparationError = error.message
        }

        exportExecutor.execute {
            val result = if (preparationError == null) {
                exportCompletedRecordings(appContext, selectedTree, completedFiles)
            } else {
                ExportResult(0, preparationError)
            }
            callback?.onComplete(result)
        }
    }

    private fun exportCompletedRecordings(
        context: Context,
        tree: Uri?,
        files: Array<File>?,
    ): ExportResult {
        if (tree == null || files.isNullOrEmpty()) return ExportResult(0, null)

        val parent = try {
            DocumentsContract.buildDocumentUriUsingTree(
                tree,
                DocumentsContract.getTreeDocumentId(tree),
            )
        } catch (error: IllegalArgumentException) {
            return ExportResult(0, error.message)
        }

        var moved = 0
        for (source in files.sortedBy(File::lastModified)) {
            var destination: Uri? = null
            try {
                destination = DocumentsContract.createDocument(
                    context.contentResolver,
                    parent,
                    TNRD_MIME_TYPE,
                    source.name,
                ) ?: return ExportResult(moved, "The selected folder did not create the file.")

                FileInputStream(source).use { input ->
                    val output = context.contentResolver.openOutputStream(destination, "w")
                        ?: error("The selected folder did not open the file for writing.")
                    output.use { input.copyTo(it, COPY_BUFFER_SIZE) }
                }
                if (!source.delete()) {
                    return ExportResult(
                        moved,
                        "The recording was copied, but its staging file could not be removed.",
                    )
                }
                moved++
            } catch (error: Exception) {
                destination?.let { incomplete ->
                    try {
                        DocumentsContract.deleteDocument(context.contentResolver, incomplete)
                    } catch (_: Exception) {
                        // Best effort cleanup of an incomplete destination file.
                    }
                }
                return ExportResult(
                    moved,
                    error.message?.takeIf(String::isNotEmpty) ?: error.javaClass.simpleName,
                )
            }
        }
        return ExportResult(moved, null)
    }
}
