package com.tracknrace.android

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.net.wifi.WifiManager
import android.provider.DocumentsContract
import com.journeyapps.barcodescanner.ScanOptions
import java.util.concurrent.Executors

/** Native owner for telemetry, recording, discovery, and desktop pairing. */
internal class TelemetryController(
    activity: Activity,
    internal val store: TelemetryStore,
) : NativeTelemetry.Listener, PairedTelemetryClient.Listener, NativePairDiscovery.Listener {
    companion object {
        private const val UDP_PORT = 20777
    }

    private val context: Context = activity.applicationContext
    private val sourceExecutor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "tnrp-native-source").apply { isDaemon = true }
    }
    private val directTelemetry = NativeTelemetry(this)
    private val pairedTelemetry = PairedTelemetryClient(context, this)
    private val discovery = NativePairDiscovery(this)

    @Volatile private var sourceRequested = false
    @Volatile private var sourceGeneration = 0
    @Volatile private var pairingPending = false
    @Volatile private var discoveryRequested = false
    private var multicastLock: WifiManager.MulticastLock? = null

    init {
        publishSettings()
    }

    fun onHostStart() {
        sourceRequested = true
        restartConfiguredSource()
        if (discoveryRequested) startDiscoveryInternal()
    }

    fun onHostStop() {
        stopDiscoveryInternal()
        suspendSourcesAsync()
    }

    fun destroy() {
        sourceRequested = false
        sourceGeneration++
        pairingPending = false
        discoveryRequested = false
        stopDiscoveryInternal()
        directTelemetry.stop()
        pairedTelemetry.close()
        sourceExecutor.shutdownNow()
    }

    fun reconnect() {
        if (isDirectSource()) {
            store.showMessage("Reconnect is only available for a paired desktop")
            return
        }
        if (!PairedTelemetryClient.hasSavedDesktop(context)) {
            store.showMessage("No paired desktop is saved")
            return
        }
        sourceRequested = true
        restartConfiguredSource()
    }

    fun setSource(source: String) {
        if (source != PairedTelemetryClient.SOURCE_DIRECT &&
            source != PairedTelemetryClient.SOURCE_PAIRED
        ) {
            store.showMessage("Unknown telemetry source")
            return
        }
        if (source == PairedTelemetryClient.SOURCE_PAIRED &&
            !PairedTelemetryClient.hasSavedDesktop(context)
        ) {
            store.showMessage("Pair a desktop before selecting paired mode")
            return
        }
        preferences().edit().putString(PairedTelemetryClient.PREF_SOURCE, source).apply()
        publishSettings()
        if (sourceRequested) restartConfiguredSource()
    }

    fun setRecording(enabled: Boolean) {
        preferences().edit().putBoolean(RecordingStorage.PREF_RECORDING, enabled).apply()
        var active = enabled
        if (isDirectSource()) {
            val path = try {
                RecordingStorage.stagingDirectory(context).absolutePath
            } catch (error: IllegalStateException) {
                active = false
                store.updateSource("error", error.message)
                store.showMessage("Recording disabled: ${error.message}")
                ""
            }
            directTelemetry.setRecording(active, path)
        }
        if (active != enabled) {
            preferences().edit().putBoolean(RecordingStorage.PREF_RECORDING, false).apply()
        }
        publishSettings()
    }

    fun recordingDirectoryIntent(): Intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
        .addFlags(
            Intent.FLAG_GRANT_READ_URI_PERMISSION or
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or
                Intent.FLAG_GRANT_PREFIX_URI_PERMISSION,
        )
        .also { intent ->
            RecordingStorage.selectedDirectory(context)?.let {
                intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, it)
            }
        }

    fun acceptRecordingDirectory(uri: Uri, resultFlags: Int) {
        val flags = resultFlags and
            (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        try {
            context.contentResolver.takePersistableUriPermission(uri, flags)
            RecordingStorage.setSelectedDirectory(context, uri)
            publishSettings()
            store.showMessage(context.getString(R.string.settings_storage_updated))
            RecordingStorage.exportCompletedRecordingsAsync(context, ::onRecordingExport)
        } catch (error: SecurityException) {
            store.showMessage(context.getString(R.string.settings_storage_permission_error))
        }
    }

    fun useDefaultRecordingDirectory() {
        RecordingStorage.clearSelectedDirectory(context)
        publishSettings()
    }

    fun startDiscovery() {
        discoveryRequested = true
        startDiscoveryInternal()
    }

    private fun startDiscoveryInternal() {
        stopDiscoveryInternal()
        store.clearDiscovery()
        val wifi = context.getSystemService(Context.WIFI_SERVICE) as? WifiManager
        multicastLock = wifi?.createMulticastLock("track-n-race-compose-pairing")?.apply {
            setReferenceCounted(false)
            acquire()
        }
        discovery.start()?.let { error ->
            stopDiscoveryInternal()
            store.showMessage("LAN discovery unavailable: $error. QR pairing still works.")
        }
    }

    fun stopDiscovery() {
        discoveryRequested = false
        stopDiscoveryInternal()
    }

    private fun stopDiscoveryInternal() {
        discovery.stop()
        multicastLock?.let { if (it.isHeld) it.release() }
        multicastLock = null
    }

    fun qrScanOptions(): ScanOptions = ScanOptions().apply {
        setCaptureActivity(PortraitCaptureActivity::class.java)
        setDesiredBarcodeFormats(ScanOptions.QR_CODE)
        setPrompt(context.getString(R.string.pairing_scan_prompt))
        setBeepEnabled(false)
        setOrientationLocked(true)
    }

    fun pairQr(payload: String) {
        try {
            val uri = Uri.parse(payload)
            require(uri.scheme == "tnrpair" && uri.host == "v1") { "Unsupported pairing QR" }
            val serverId = uri.pathSegments.firstOrNull().orEmpty()
            val host = uri.getQueryParameter("h")
            val secret = uri.getQueryParameter("s")
            val port = uri.getQueryParameter("p")?.toIntOrNull()
            val expiry = uri.getQueryParameter("e")?.toLongOrNull()
            require(
                serverId.isNotEmpty() && !host.isNullOrEmpty() && !secret.isNullOrEmpty() &&
                    port != null && expiry != null && expiry >= System.currentTimeMillis(),
            ) { "Pairing QR has expired or is incomplete" }
            prepareForPairing()
            pairedTelemetry.pair(
                PairedTelemetryClient.Endpoint(
                    serverId,
                    "Track N Race desktop",
                    host,
                    port,
                ),
                secret,
                null,
            )
        } catch (error: Exception) {
            pairingPending = false
            store.updatePairingBusy(false)
            store.showMessage(error.message ?: "Could not use that QR")
        }
    }

    fun pairCode(desktop: DiscoveredDesktop?, code: String) {
        if (desktop == null || code.isBlank()) {
            store.showMessage("Select a desktop and enter its matching code")
            return
        }
        prepareForPairing()
        pairedTelemetry.pair(
            PairedTelemetryClient.Endpoint(
                desktop.serverId,
                desktop.name,
                desktop.host,
                desktop.port,
            ),
            null,
            code.trim(),
        )
    }

    fun forgetDesktop() {
        pairedTelemetry.close()
        PairedTelemetryClient.forgetDesktop(context)
        publishSettings()
        if (sourceRequested) restartConfiguredSource()
    }

    override fun onTelemetryRow(json: String) = store.acceptColdRow(json)

    override fun onTelemetryBinary(bytes: ByteArray) = store.acceptBinary(bytes)

    override fun onRow(json: String) = store.acceptColdRow(json)

    override fun onBinary(bytes: ByteArray) = store.acceptBinary(bytes)

    override fun onState(state: String, detail: String?) {
        if (pairingPending && (state == "error" || state == "disconnected")) {
            pairingPending = false
            store.updatePairingBusy(false)
        }
        store.updateSource(state, detail)
    }

    override fun onPaired() {
        pairingPending = false
        sourceRequested = true
        store.updatePairingBusy(false)
        store.updateSource("paired", PairedTelemetryClient.savedDesktopName(context))
        publishSettings()
        store.showMessage(context.getString(R.string.pairing_success))
    }

    override fun onService(service: NativePairDiscovery.Service) {
        store.discovered(
            DiscoveredDesktop(
                service.serverId,
                service.name,
                service.address,
                service.port,
                service.pairing,
            ),
        )
    }

    private fun preferences() = RecordingStorage.preferences(context)

    private fun isDirectSource(): Boolean =
        preferences().getString(
            PairedTelemetryClient.PREF_SOURCE,
            PairedTelemetryClient.SOURCE_DIRECT,
        ) == PairedTelemetryClient.SOURCE_DIRECT

    private fun currentSettings() = AndroidSettings(
        source = preferences().getString(
            PairedTelemetryClient.PREF_SOURCE,
            PairedTelemetryClient.SOURCE_DIRECT,
        ) ?: PairedTelemetryClient.SOURCE_DIRECT,
        recordingEnabled = preferences().getBoolean(RecordingStorage.PREF_RECORDING, false),
        hasSavedDesktop = PairedTelemetryClient.hasSavedDesktop(context),
        desktopName = PairedTelemetryClient.savedDesktopName(context),
        recordingDirectory = RecordingStorage.selectedDirectoryLabel(context),
        usingCustomDirectory = RecordingStorage.selectedDirectory(context) != null,
    )

    private fun publishSettings() = store.updateSettings(currentSettings())

    private fun restartConfiguredSource() {
        val generation = ++sourceGeneration
        sourceExecutor.execute {
            directTelemetry.stop()
            pairedTelemetry.close()
            if (!sourceRequested || generation != sourceGeneration) return@execute
            if (!isDirectSource() && PairedTelemetryClient.hasSavedDesktop(context)) {
                store.updateSource("connecting", PairedTelemetryClient.savedDesktopName(context))
                pairedTelemetry.connectSaved()
                return@execute
            }

            val recordingPath = try {
                RecordingStorage.stagingDirectory(context).absolutePath
            } catch (error: IllegalStateException) {
                store.updateSource("error", error.message)
                ""
            }
            val error = directTelemetry.start(UDP_PORT)
            if (error != null) {
                store.updateSource("error", error)
                return@execute
            }
            val recording = preferences().getBoolean(RecordingStorage.PREF_RECORDING, false)
            directTelemetry.setRecording(recording && recordingPath.isNotEmpty(), recordingPath)
            store.updateSource("listening", "UDP $UDP_PORT")
        }
    }

    private fun prepareForPairing() {
        pairingPending = true
        store.updatePairingBusy(true)
        sourceGeneration++
        sourceExecutor.execute(directTelemetry::stop)
        pairedTelemetry.close()
    }

    private fun suspendSourcesAsync() {
        sourceGeneration++
        sourceExecutor.execute {
            directTelemetry.stop()
            pairedTelemetry.close()
            RecordingStorage.exportCompletedRecordingsAsync(context, null)
        }
    }

    private fun onRecordingExport(result: RecordingStorage.ExportResult) {
        result.error?.let { error ->
            store.showMessage("Could not move a recording: $error")
            return
        }
        if (result.movedFiles > 0) {
            store.showMessage("Moved ${result.movedFiles} recording${if (result.movedFiles == 1) "" else "s"}")
        }
    }
}
