package com.tracknrace.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.FileInputStream;
import java.io.OutputStream;
import java.util.Arrays;
import java.util.Comparator;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Bridges libtnrp's filesystem writer to user-selected Android document trees. */
final class RecordingStorage {
    static final String PREFS = "track_n_race_android";
    static final String PREF_RECORDING = "recording_enabled";
    private static final String PREF_DIRECTORY_URI = "recording_directory_uri";
    private static final String TNRD_MIME_TYPE = "application/octet-stream";
    private static final ExecutorService EXPORT_EXECUTOR =
        Executors.newSingleThreadExecutor(runnable -> {
            Thread thread = new Thread(runnable, "tnrp-recording-export");
            thread.setDaemon(true);
            return thread;
        });

    interface ExportCallback {
        void onComplete(ExportResult result);
    }

    static final class ExportResult {
        final int movedFiles;
        final String error;

        ExportResult(int movedFiles, String error) {
            this.movedFiles = movedFiles;
            this.error = error;
        }
    }

    private RecordingStorage() {}

    static SharedPreferences preferences(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static File stagingDirectory(Context context) {
        File documents = context.getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS);
        if (documents == null) documents = context.getFilesDir();
        File directory = new File(documents, "Track N Race");
        if (directory.exists() && !directory.isDirectory()) {
            throw new IllegalStateException(
                "Track N Race exists at the recording path but is not a folder.");
        }
        if (!directory.isDirectory() && !directory.mkdirs() && !directory.isDirectory()) {
            throw new IllegalStateException("Could not create the Track N Race folder.");
        }
        if (!directory.canWrite()) {
            throw new IllegalStateException("The Track N Race folder is not writable.");
        }
        return directory;
    }

    static Uri selectedDirectory(Context context) {
        String value = preferences(context).getString(PREF_DIRECTORY_URI, null);
        if (value == null || value.isEmpty()) return null;
        return Uri.parse(value);
    }

    static void setSelectedDirectory(Context context, Uri directory) {
        preferences(context).edit().putString(PREF_DIRECTORY_URI, directory.toString()).apply();
    }

    static void clearSelectedDirectory(Context context) {
        Uri directory = selectedDirectory(context);
        preferences(context).edit().remove(PREF_DIRECTORY_URI).apply();
        if (directory == null) return;
        try {
            context.getContentResolver().releasePersistableUriPermission(directory,
                android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION |
                    android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (SecurityException ignored) {
            // The provider may already have revoked the grant.
        }
    }

    static String selectedDirectoryLabel(Context context) {
        Uri directory = selectedDirectory(context);
        if (directory == null) {
            try {
                return stagingDirectory(context).getAbsolutePath();
            } catch (IllegalStateException error) {
                return context.getString(R.string.settings_storage_unavailable);
            }
        }
        try {
            String documentId = Uri.decode(DocumentsContract.getTreeDocumentId(directory));
            int rootSeparator = documentId.indexOf(':');
            String relative = rootSeparator >= 0 ? documentId.substring(rootSeparator + 1) : documentId;
            return relative.isEmpty() ? context.getString(R.string.settings_storage_root) : relative;
        } catch (IllegalArgumentException ignored) {
            return directory.getLastPathSegment() == null
                ? directory.toString() : Uri.decode(directory.getLastPathSegment());
        }
    }

    static void exportCompletedRecordingsAsync(Context context, ExportCallback callback) {
        Context appContext = context.getApplicationContext();
        Uri selectedTree = selectedDirectory(appContext);
        File[] completedFiles = null;
        String preparationError = null;
        try {
            completedFiles = stagingDirectory(appContext).listFiles(
                file -> file.isFile() && file.getName().endsWith(".tnrd"));
        } catch (IllegalStateException error) {
            preparationError = error.getMessage();
        }
        File[] files = completedFiles;
        String directoryError = preparationError;
        EXPORT_EXECUTOR.execute(() -> {
            ExportResult result = directoryError == null
                ? exportCompletedRecordings(appContext, selectedTree, files)
                : new ExportResult(0, directoryError);
            if (callback != null) callback.onComplete(result);
        });
    }

    private static ExportResult exportCompletedRecordings(Context context, Uri tree, File[] files) {
        if (tree == null) return new ExportResult(0, null);
        if (files == null || files.length == 0) return new ExportResult(0, null);
        Arrays.sort(files, Comparator.comparingLong(File::lastModified));

        Uri parent;
        try {
            parent = DocumentsContract.buildDocumentUriUsingTree(
                tree, DocumentsContract.getTreeDocumentId(tree));
        } catch (IllegalArgumentException error) {
            return new ExportResult(0, error.getMessage());
        }

        int moved = 0;
        for (File source : files) {
            Uri destination = null;
            try {
                destination = DocumentsContract.createDocument(context.getContentResolver(),
                    parent, TNRD_MIME_TYPE, source.getName());
                if (destination == null) {
                    return new ExportResult(moved, "The selected folder did not create the file.");
                }
                try (FileInputStream input = new FileInputStream(source);
                     OutputStream output = context.getContentResolver()
                         .openOutputStream(destination, "w")) {
                    if (output == null) throw new IllegalStateException(
                        "The selected folder did not open the file for writing.");
                    byte[] buffer = new byte[256 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                    output.flush();
                }
                if (!source.delete()) {
                    return new ExportResult(moved,
                        "The recording was copied, but its staging file could not be removed.");
                }
                moved++;
            } catch (Exception error) {
                if (destination != null) {
                    try {
                        DocumentsContract.deleteDocument(context.getContentResolver(), destination);
                    } catch (Exception ignored) {
                        // Best effort cleanup of an incomplete destination file.
                    }
                }
                String message = error.getMessage();
                return new ExportResult(moved,
                    message == null || message.isEmpty() ? error.getClass().getSimpleName() : message);
            }
        }
        return new ExportResult(moved, null);
    }
}
