package com.nogoat.tracknrace

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.concurrent.TimeUnit

/** A newer release than the installed one. */
internal data class AvailableUpdate(
    val installedVersion: String,
    val latestVersion: String,
    val releaseUrl: String,
    /** The signed APK to install; null when this build can only open the download page. */
    val file: UpdateFile?,
)

internal data class UpdateFile(val name: String, val url: String, val sha512: String, val size: Long)

internal sealed interface UpdateProgress {
    data object Idle : UpdateProgress
    data class Downloading(val received: Long, val total: Long) : UpdateProgress
    data object Verifying : UpdateProgress
    /** Handed to PackageInstaller; Android may ask the user to confirm. */
    data object Installing : UpdateProgress
    data class Failed(val message: String) : UpdateProgress
}

/**
 * Self-update, as the desktop apps do it (electron-frontend/src/main/updateChecker.ts).
 * Once a day at start-up it asks the update service for the newest signed
 * manifest; libtnrp checks the signature and picks the APK (tnrp/Updates.h).
 * "Update now" downloads that APK, checks its size and SHA-512 against the
 * signed manifest, and installs it with PackageInstaller. A build that isn't
 * signed with the release key can't install a release APK over itself, so it
 * only offers the download page.
 */
internal object AppUpdater {
    var available by mutableStateOf<AvailableUpdate?>(null)
        private set
    var progress by mutableStateOf<UpdateProgress>(UpdateProgress.Idle)
        private set

    private const val TAG = "AppUpdater"
    const val PREF_ENABLED = "updates.enabled"
    private const val PREF_LAST_CHECK = "updates.lastCheckAt"
    private const val PREF_SKIPPED = "updates.skippedVersion"
    private const val CHECK_INTERVAL_MS = 24L * 60 * 60 * 1000
    private const val RELEASE_API_URL = "https://api.github.com/repos/NoGoat/Track-N-Race/releases/latest"
    private const val RELEASE_PAGE_URL = "https://github.com/NoGoat/Track-N-Race/releases/latest"
    private const val PRODUCT = "android"
    private const val OS = "android"
    private const val ARCH = "universal"
    private const val FORMAT = "apk"

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val main = Handler(Looper.getMainLooper())
    private val client by lazy {
        OkHttpClient.Builder()
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(30, TimeUnit.SECONDS)
            .build()
    }
    private var started = false

    /** Once per process: clears old downloads, then checks if the setting and the daily limit allow. */
    fun checkOnStartup(context: Context) {
        if (started) return
        started = true
        val app = context.applicationContext
        scope.launch {
            available = withContext(Dispatchers.IO) {
                removeOldDownloads(app)
                try {
                    check(app)
                } catch (error: Exception) {
                    Log.w(TAG, "update check failed", error)
                    null
                }
            }
        }
    }

    /** Remind me later: gone until the next check. */
    fun dismiss() {
        if (progress.busy) return
        available = null
        progress = UpdateProgress.Idle
    }

    fun skip(context: Context) {
        val update = available ?: return
        preferences(context).edit().putString(PREF_SKIPPED, update.latestVersion).apply()
        dismiss()
    }

    fun openReleasePage(context: Context) {
        val update = available ?: return
        runCatching {
            context.startActivity(
                Intent(Intent.ACTION_VIEW, Uri.parse(update.releaseUrl)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            )
        }
        dismiss()
    }

    /** Android lets this app install packages ("Install unknown apps"). */
    fun canInstall(context: Context): Boolean = context.packageManager.canRequestPackageInstalls()

    /** The system page where the user allows this app to install packages. */
    fun installPermissionIntent(context: Context): Intent =
        Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${context.packageName}"))

    fun fail(message: String) {
        progress = UpdateProgress.Failed(message)
    }

    /** Update now: download, verify, install. The caller has made sure [canInstall]. */
    fun install(context: Context) {
        val file = available?.file ?: return
        if (progress.busy) return
        val app = context.applicationContext
        progress = UpdateProgress.Downloading(0, file.size)
        scope.launch {
            val apk = try {
                withContext(Dispatchers.IO) { download(app, file) }
            } catch (error: IOException) {
                fail(error.message ?: "the download failed")
                return@launch
            }
            progress = UpdateProgress.Verifying
            val mismatch = withContext(Dispatchers.IO) {
                NativeTelemetry.nativeUpdateVerifyFile(apk.absolutePath, file.sha512, file.size)
            }
            if (mismatch.isNotEmpty()) {
                apk.delete()
                fail(mismatch)
                return@launch
            }
            progress = UpdateProgress.Installing
            try {
                withContext(Dispatchers.IO) { commit(app, apk) }
            } catch (error: Exception) {
                Log.w(TAG, "install session failed", error)
                fail(error.message ?: "Android could not start the install")
            }
        }
    }

    private val UpdateProgress.busy: Boolean
        get() = this is UpdateProgress.Downloading || this is UpdateProgress.Verifying || this is UpdateProgress.Installing

    private fun preferences(context: Context) = RecordingStorage.preferences(context)

    private fun check(context: Context): AvailableUpdate? {
        val preferences = preferences(context)
        if (!preferences.getBoolean(PREF_ENABLED, true)) return null
        val testEndpoint = BuildConfig.UPDATE_TEST_ENDPOINT
        val now = System.currentTimeMillis()
        val lastCheckAt = preferences.getLong(PREF_LAST_CHECK, 0)
        if (testEndpoint.isEmpty() && now - lastCheckAt in 0 until CHECK_INTERVAL_MS) return null
        // Recorded before the request so an outage can't cause a request on every launch.
        preferences.edit().putLong(PREF_LAST_CHECK, now).apply()

        val current = BuildConfig.VERSION_NAME.trim().removePrefix("v")
        val serviceUrl = testEndpoint.ifEmpty { NativeTelemetry.nativeUpdateServiceUrl() }
        if (serviceUrl.isNotEmpty()) {
            try {
                return checkService(context, serviceUrl, current)
            } catch (error: Exception) {
                Log.w(TAG, "update service check failed, falling back to GitHub", error)
            }
        }
        // A test build never asks the real GitHub.
        if (testEndpoint.isNotEmpty()) return null
        return checkGitHub(context, current)
    }

    private fun checkService(context: Context, serviceUrl: String, current: String): AvailableUpdate? {
        val url = serviceUrl + NativeTelemetry.nativeUpdateCheckPath(PRODUCT, OS, ARCH, FORMAT, current)
        val body = client.newCall(request(url)).execute().use { response ->
            if (response.code == 204) return null
            if (!response.isSuccessful) throw IOException("update service returned HTTP ${response.code}")
            response.body.string()
        }
        val offer = JSONObject(NativeTelemetry.nativeUpdateEvaluateJson(body, PRODUCT, OS, ARCH, FORMAT, current))
        val status = offer.optString("status")
        if (status == "invalid") throw IOException(offer.optString("error").ifEmpty { "invalid update response" })
        val version = offer.optString("version")
        if (version.isEmpty() || !newer(version, current) || skipped(context, version)) return null
        val file = if (status == "available" && signedForUpdates(context)) {
            UpdateFile(
                name = offer.getString("fileName"),
                url = offer.getString("url"),
                sha512 = offer.getString("sha512"),
                size = offer.getLong("size"),
            )
        } else {
            null
        }
        return AvailableUpdate(
            installedVersion = current,
            latestVersion = version,
            releaseUrl = offer.optString("notesUrl").ifEmpty { RELEASE_PAGE_URL },
            file = file,
        )
    }

    /** Fallback while the update service can't be reached: the GitHub release tag, download page only. */
    private fun checkGitHub(context: Context, current: String): AvailableUpdate? = try {
        val body = client.newCall(
            Request.Builder()
                .url(RELEASE_API_URL)
                .header("Accept", "application/vnd.github+json")
                .header("User-Agent", "Track-N-Race")
                .header("X-GitHub-Api-Version", "2022-11-28")
                .build(),
        ).execute().use { response ->
            if (!response.isSuccessful) throw IOException("GitHub returned HTTP ${response.code}")
            response.body.string()
        }
        val version = JSONObject(body).getString("tag_name").trim().removePrefix("v")
        if (!newer(version, current) || skipped(context, version)) {
            null
        } else {
            AvailableUpdate(current, version, RELEASE_PAGE_URL, file = null)
        }
    } catch (error: Exception) {
        Log.w(TAG, "GitHub release check failed", error)
        null
    }

    private fun request(url: String) = Request.Builder().url(url).header("User-Agent", "Track-N-Race").build()

    private fun newer(version: String, current: String) =
        NativeTelemetry.nativeUpdateCompareVersions(version, current) == 1

    private fun skipped(context: Context, version: String): Boolean {
        val skipped = preferences(context).getString(PREF_SKIPPED, null).orEmpty()
        return skipped.isNotEmpty() && NativeTelemetry.nativeUpdateCompareVersions(skipped, version) == 0
    }

    /** This APK carries the certificate update APKs are signed with, so one can install over it. */
    @Suppress("DEPRECATION")
    private fun signedForUpdates(context: Context): Boolean {
        val pm = context.packageManager
        val certificates = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            pm.getPackageInfo(context.packageName, PackageManager.GET_SIGNING_CERTIFICATES)
                .signingInfo?.apkContentsSigners
        } else {
            pm.getPackageInfo(context.packageName, PackageManager.GET_SIGNATURES).signatures
        } ?: return false
        val expected = BuildConfig.UPDATE_CERT_SHA256
        return certificates.any { signature ->
            MessageDigest.getInstance("SHA-256").digest(signature.toByteArray())
                .joinToString("") { "%02x".format(it) } == expected
        }
    }

    private fun downloadDirectory(context: Context) = File(context.cacheDir, "update")

    private fun removeOldDownloads(context: Context) {
        downloadDirectory(context).listFiles()?.forEach { it.delete() }
    }

    private fun download(context: Context, file: UpdateFile): File {
        val directory = downloadDirectory(context).apply { mkdirs() }
        val part = File(directory, "${file.name}.part")
        val target = File(directory, file.name)
        client.newCall(request(file.url)).execute().use { response ->
            if (!response.isSuccessful) throw IOException("the download returned HTTP ${response.code}")
            response.body.byteStream().use { input ->
                part.outputStream().use { output ->
                    val buffer = ByteArray(64 * 1024)
                    var received = 0L
                    var reported = -1L
                    while (true) {
                        val count = input.read(buffer)
                        if (count < 0) break
                        received += count
                        if (received > file.size) throw IOException("the download is larger than the signed size")
                        output.write(buffer, 0, count)
                        // About every 1% is plenty for the bar.
                        if (received - reported >= file.size / 100 || received == file.size) {
                            reported = received
                            val snapshot = received
                            main.post {
                                if (progress is UpdateProgress.Downloading) {
                                    progress = UpdateProgress.Downloading(snapshot, file.size)
                                }
                            }
                        }
                    }
                }
            }
        }
        target.delete()
        if (!part.renameTo(target)) throw IOException("could not save the download")
        return target
    }

    private fun commit(context: Context, apk: File) {
        val installer = context.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(context.packageName)
            setSize(apk.length())
            // Once this app is the recorded installer, Android 12+ may update without asking.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
            }
        }
        val sessionId = installer.createSession(params)
        installer.openSession(sessionId).use { session ->
            session.openWrite("base.apk", 0, apk.length()).use { output ->
                apk.inputStream().use { it.copyTo(output) }
                session.fsync(output)
            }
            // PackageInstaller fills in the result extras, so the intent must be mutable.
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
            val result = PendingIntent.getBroadcast(
                context,
                sessionId,
                Intent(context, UpdateInstallReceiver::class.java),
                flags,
            )
            session.commit(result.intentSender)
        }
    }
}
