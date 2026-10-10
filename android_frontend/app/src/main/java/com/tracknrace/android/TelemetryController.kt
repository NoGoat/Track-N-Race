package com.tracknrace.android

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Build
import android.provider.DocumentsContract
import com.journeyapps.barcodescanner.ScanOptions
import java.util.concurrent.Executors

/** Native owner for telemetry, recording, discovery, and desktop pairing. */
internal class TelemetryController(
    activity: Activity,
    internal val store: TelemetryStore,
) : NativeTelemetry.Listener, PairedTelemetryClient.Listener, PairDiscovery.Listener {
    companion object {
        private const val UDP_PORT = 20777
        private const val PREF_TIMING_ONE_LINE = "timing_one_line"
    }

    private val context: Context = activity.applicationContext
    private val sourceExecutor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "tnrp-native-source").apply { isDaemon = true }
    }
    private val directTelemetry = NativeTelemetry(this)
    private val pairedTelemetry = PairedTelemetryClient(context, this)
    private val discovery = PairDiscovery(context, this)
    internal val analysis = AnalysisController(
        context,
        store,
        pairedTelemetry::requestLapData,
        pairedTelemetry::requestLapDelta,
    )

    @Volatile private var sourceRequested = false
    @Volatile private var sourceGeneration = 0
    @Volatile private var pairingPending = false
    @Volatile private var discoveryRequested = false
    // Looking for the saved desktop at a new address while reconnecting.
    @Volatile private var desktopSearch = false
    @Volatile private var qrScannerActive = false
    private var lowLatencyLock: WifiManager.WifiLock? = null

    init {
        store.setLapDeltaRequester(pairedTelemetry::requestLapDelta)
        store.setAnalysisDeltaSink(analysis::onDeltaReply)
        publishSettings()
    }

    fun onHostStart() {
        // CaptureActivity temporarily stops MainActivity while remaining inside
        // this app. Keep the current source and discovery browser alive across
        // that handoff instead of racing the activity-result pairing attempt.
        if (qrScannerActive) return
        holdLowLatencyWifi(true)
        sourceRequested = true
        restartConfiguredSource()
        if (discoveryRequested) startDiscoveryInternal(clear = true)
    }

    fun onHostStop() {
        if (qrScannerActive) return
        holdLowLatencyWifi(false)
        desktopSearch = false
        stopDiscoveryInternal()
        suspendSourcesAsync()
    }

    fun destroy() {
        store.setLapDeltaRequester(null)
        store.setAnalysisDeltaSink(null)
        sourceRequested = false
        sourceGeneration++
        pairingPending = false
        discoveryRequested = false
        desktopSearch = false
        qrScannerActive = false
        stopDiscoveryInternal()
        holdLowLatencyWifi(false)
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

    fun requestParticipants() {
        if (!isDirectSource()) pairedTelemetry.requestParticipants()
    }

    fun requestDriverRestriction() {
        if (!isDirectSource()) pairedTelemetry.requestDriverRestriction()
    }

    fun setActivePage(page: PairedTelemetryPage) {
        pairedTelemetry.setPage(page)
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

    fun setTimingOneLine(enabled: Boolean) {
        preferences().edit().putBoolean(PREF_TIMING_ONE_LINE, enabled).apply()
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
        startDiscoveryInternal(clear = true)
    }

    // DNS-SD runs in the system mDNS stack, so no multicast lock is needed.
    private fun startDiscoveryInternal(clear: Boolean) {
        if (clear) {
            discovery.stop()
            store.clearDiscovery()
        }
        discovery.start()
    }

    /**
     * Keeps the Wi-Fi radio out of power save while the app is on screen. In
     * power save the radio dozes between access point beacons and the router
     * holds incoming packets until it wakes: on a Galaxy M12 that stalls the
     * telemetry stream (and every screen with it) for up to 250 ms about once
     * a second. The low-latency lock only applies while the app is in the
     * foreground with the screen on, and is released when it stops.
     */
    private fun holdLowLatencyWifi(hold: Boolean) {
        if (!hold) {
            lowLatencyLock?.let { if (it.isHeld) it.release() }
            return
        }
        val lock = lowLatencyLock ?: run {
            val wifi = context.getSystemService(Context.WIFI_SERVICE) as? WifiManager ?: return
            @Suppress("DEPRECATION")
            val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            } else {
                WifiManager.WIFI_MODE_FULL_HIGH_PERF
            }
            wifi.createWifiLock(mode, "track-n-race-telemetry").apply {
                setReferenceCounted(false)
                lowLatencyLock = this
            }
        }
        if (!lock.isHeld) lock.acquire()
    }

    fun stopDiscovery() {
        discoveryRequested = false
        if (!desktopSearch) stopDiscoveryInternal()
    }

    private fun stopDiscoveryInternal() {
        discovery.stop()
    }

    fun qrScanOptions(): ScanOptions = ScanOptions().apply {
        setCaptureActivity(PortraitCaptureActivity::class.java)
        setDesiredBarcodeFormats(ScanOptions.QR_CODE)
        setPrompt(context.getString(R.string.pairing_scan_prompt))
        setBeepEnabled(false)
        setOrientationLocked(true)
    }

    fun onQrScannerLaunching() {
        qrScannerActive = true
    }

    fun onQrScannerFinished() {
        qrScannerActive = false
    }

    fun pairQr(payload: String) {
        try {
            val uri = Uri.parse(payload)
            require(uri.scheme == "tnrpair") { "Unsupported pairing QR" }
            require(uri.host == "v3") { "Update Track N Race on the desktop, then show the QR again" }
            val serverId = uri.pathSegments.firstOrNull().orEmpty()
            val host = uri.getQueryParameter("h")
            val secret = uri.getQueryParameter("s")
            val identityKey = uri.getQueryParameter("k")
            val port = uri.getQueryParameter("p")?.toIntOrNull()
            val expiry = uri.getQueryParameter("e")?.toLongOrNull()
            require(
                serverId.isNotEmpty() && !host.isNullOrEmpty() && !secret.isNullOrEmpty() &&
                    !identityKey.isNullOrEmpty() && port != null && expiry != null &&
                    expiry >= System.currentTimeMillis(),
            ) { "Pairing QR has expired or is incomplete" }
            prepareForPairing()
            pairedTelemetry.pair(
                PairedTelemetryClient.Endpoint(
                    serverId,
                    "Track N Race desktop",
                    host,
                    port,
                ),
                // The QR pins the desktop's key: no other machine can answer.
                PairedTelemetryClient.Credential.Qr(identityKey, secret),
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
            // CPace: the code is proven, never sent.
            PairedTelemetryClient.Credential.Code(code),
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

    override fun onLapData(json: String) = analysis.onLapData(json)

    override fun onState(state: String, detail: String?) {
        if (pairingPending && (state == "error" || state == "disconnected")) {
            pairingPending = false
            store.updatePairingBusy(false)
            detail?.let(store::showMessage)
        }
        store.updateSource(state, detail)
    }

    override fun onPaired() {
        endDesktopSearch()
        pairingPending = false
        sourceRequested = true
        store.updatePairingBusy(false)
        store.updateSource("paired", PairedTelemetryClient.savedDesktopName(context))
        publishSettings()
        store.showMessage(context.getString(R.string.pairing_success))
        store.notifyPairingSucceeded()
    }

    override fun onDesktopReached() = endDesktopSearch()

    override fun onSearchForDesktop(serverId: String) {
        if (desktopSearch) return
        desktopSearch = true
        startDiscoveryInternal(clear = false)
    }

    private fun endDesktopSearch() {
        if (!desktopSearch) return
        desktopSearch = false
        if (!discoveryRequested) stopDiscoveryInternal()
    }

    override fun onService(service: PairDiscovery.Service) {
        store.discovered(
            DiscoveredDesktop(
                service.serverId,
                service.name,
                service.address,
                service.port,
                service.pairing,
            ),
        )
        // The saved desktop may have a new address after a DHCP renewal or a
        // network change; its identity is checked by the handshake.
        if (desktopSearch) {
            pairedTelemetry.onDesktopFound(service.serverId, service.address, service.port)
        }
    }

    override fun onDiscoveryError(message: String) {
        if (discoveryRequested) {
            store.showMessage("LAN discovery unavailable: $message. QR pairing still works.")
        }
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
        timingOneLine = preferences().getBoolean(PREF_TIMING_ONE_LINE, false),
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
            store.resetLapComparison()
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
