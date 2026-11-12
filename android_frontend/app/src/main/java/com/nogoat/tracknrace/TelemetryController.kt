package com.nogoat.tracknrace

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Build
import android.os.PowerManager
import android.provider.DocumentsContract
import android.system.ErrnoException
import android.system.Os
import com.journeyapps.barcodescanner.ScanOptions
import com.nogoat.tracknrace.ui.theme.ThemeMode
import java.nio.ByteBuffer
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong

/** Native owner for telemetry, recording, discovery, and desktop pairing. */
internal class TelemetryController(
    context: Context,
    internal val store: TelemetryStore,
) : NativeTelemetry.Listener, PairedTelemetryClient.Listener, PairDiscovery.Listener {
    companion object {
        private const val PREF_UDP_PORT = "udp.port"
        private const val PREF_UDP_FORWARDING = "udp.forwardingEnabled"
        // One "address:port" per line.
        private const val PREF_UDP_FORWARD_TARGETS = "udp.forwardTargets"
        private const val PREF_THEME_MODE = "theme_mode"
        private const val PREF_WHEEL_DISPLAY = "wheel_display"
    }

    private val context: Context = context.applicationContext
    private val sourceExecutor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "tnrp-native-source").apply { isDaemon = true }
    }
    // Player commands for a recording on the phone, in order; a seek may run
    // long and is overtaken by a newer one rather than queued behind it.
    private val playbackExecutor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "tnrp-playback").apply { isDaemon = true }
    }
    // Lap reads for Analysis: hundreds of KB each, kept off the player's queue.
    private val lapExecutor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "tnrp-playback-laps").apply { isDaemon = true }
    }
    private val seekIds = AtomicLong()
    private val directTelemetry = NativeTelemetry(this)
    private val pairedTelemetry = PairedTelemetryClient(context, this)
    private val discovery = PairDiscovery(context, this)
    internal val analysis = AnalysisController(
        context,
        store,
        ::requestLapData,
        ::requestLapDelta,
        ::requestDriverLaps,
        ::driverLapsSupported,
        ::requestBrakingZones,
        ::requestAnalysisMap,
    )
    internal val graphs = GraphsController(context, store, ::requestLiveHistory) { requestId, lapNumber, channels ->
        requestLapData(requestId, -1, lapNumber, channels)
    }

    /** A recording open on the phone; it replaces the game or desktop source. */
    private class OpenRecording(val file: RecordingFile, val handle: RecordingLibrary.Handle)

    @Volatile private var playback: OpenRecording? = null
    @Volatile private var activePage = PairedTelemetryPage.DASHBOARD
    // The window the listening engine was last asked to refill the open graph
    // page's histories over, for an engine started since; negative for the
    // whole session.
    @Volatile private var liveHistoryWindow = 0f

    @Volatile private var sourceRequested = false
    @Volatile private var sourceGeneration = 0
    @Volatile private var pairingPending = false
    @Volatile private var discoveryRequested = false
    // Looking for the saved desktop at a new address while reconnecting.
    @Volatile private var desktopSearch = false
    @Volatile private var qrScannerActive = false
    private var lowLatencyLock: WifiManager.WifiLock? = null

    // Who owns the session: between onStart and onStop the activity does; from
    // then, the listener service keeps it until it is stopped. The service is
    // what makes background listening survive, so the handbook's suspended
    // listener does not happen while it runs.
    private var listenerServiceActive = false
    private var hostVisible = false
    // The task was swiped away from recents; until the app is opened again,
    // a late onHostStop must not hand the session back to the service.
    private var taskRemoved = false
    private var backgroundWifiLock: WifiManager.WifiLock? = null
    private var backgroundWakeLock: PowerManager.WakeLock? = null

    /** Shows the system local network prompt; set by the UI that owns the launcher. */
    @Volatile var localNetworkRequester: (() -> Unit)? = null

    init {
        // Older recordings (V2–V5) are decompressed to a temporary file, and
        // Android sets no temporary folder for native code.
        try {
            Os.setenv("TMPDIR", context.cacheDir.absolutePath, true)
        } catch (_: ErrnoException) {
            // V6 recordings, which the phone itself writes, need none.
        }
        store.setLapDeltaRequester(::requestLapDelta)
        store.setAnalysisDeltaSink(analysis::onDeltaReply)
        store.setAnalysisDriverLapsSink(analysis::onDriverLaps)
        store.setAnalysisBrakingSink(analysis::onBrakingZones)
        store.setAnalysisMapSink(analysis::onAnalysisMap)
        publishSettings()
    }

    fun onHostStart() {
        // CaptureActivity temporarily stops MainActivity while remaining inside
        // this app. Keep the current source and discovery browser alive across
        // that handoff instead of racing the activity-result pairing attempt.
        if (qrScannerActive) return
        hostVisible = true
        taskRemoved = false
        holdLowLatencyWifi(true)
        sourceRequested = true
        if (!hasLocalNetworkAccess()) localNetworkRequester?.invoke()
        // A recording stays open, paused where it was left.
        if (playback != null) return
        // While the listener service carries the session, keep the direct
        // stream bound instead of rebinding it on every foreground return: a
        // rebind drops everything the background streamed and resets the
        // charts' All Laps history. A paired desktop reconnects cheaply under
        // its own state machine, so it takes the restart.
        if (listenerServiceActive && isDirectSource() && store.sourceStatus.state == "listening") return
        restartConfiguredSource()
        if (discoveryRequested) startDiscoveryInternal(clear = true)
    }

    fun onHostStop() {
        if (qrScannerActive) return
        hostVisible = false
        holdLowLatencyWifi(false)
        desktopSearch = false
        stopDiscoveryInternal()
        if (playback != null) {
            playbackPause()
            return
        }
        if (listenerServiceActive) {
            // The service keeps the sources and holds the locks that work off
            // screen (acquired when it took over); the dashboard's session,
            // recording and chart histories carry on behind the drawer.
            return
        }
        // The activity's stop can land after the service's onTaskRemoved; the
        // service has already gone, and the app is closing with its task.
        if (taskRemoved) {
            sourceRequested = false
            suspendSourcesAsync()
            return
        }
        // Without the local network permission there is nothing to carry: the
        // listener cannot hold a socket, so suspend as before the service
        // existed. A running session with a state error (a broken recording
        // folder, a paired desktop's refusal) is still worth carrying; the
        // notification shows the state.
        if (!hasLocalNetworkAccess()) {
            suspendSourcesAsync()
            return
        }
        // Hand the session to the persistent listener: with a notification on
        // record it keeps the sources, radio and CPU awake past this stop.
        try {
            context.startForegroundService(Intent(context, ListenerService::class.java))
            listenerServiceActive = true
        } catch (error: Exception) {
            // A background service start was denied; suspend as before it existed.
            suspendSourcesAsync()
        }
    }

    // ── The listener service ────────────────────────────────────────────────

    /**
     * The service took over as the session's owner while no host is up
     * (system restarts land here too). It holds the locks that work off
     * screen, and starts the sources if nothing is streaming yet.
     */
    fun onListenerServiceStarted() {
        listenerServiceActive = true
        holdBackgroundRadio(true)
        if (hostVisible || playback != null) return
        if (!sourceRequested) {
            sourceRequested = true
            restartConfiguredSource()
        }
    }

    /** The service saw the app's task swiped away, and is stopping itself. */
    fun onTaskRemoved() {
        taskRemoved = true
    }

    /**
     * The service stopped: with a host up the foreground keeps the sources as
     * before the service existed; with none, everything suspends and the
     * completed recordings are exported, as an app left in the background.
     */
    fun onListenerServiceStopped() {
        listenerServiceActive = false
        holdBackgroundRadio(false)
        if (hostVisible) return
        sourceRequested = false
        suspendSourcesAsync()
    }

    /**
     * Complement of [holdLowLatencyWifi]: while no host owns the screen, the
     * high performance Wi-Fi lock keeps the radio out of manufacturer power
     * save and the partial wake lock keeps the CPU out of doze. The low
     * latency lock only applies to a foreground, lit screen; through doze the
     * receive thread would stop and datagrams would be lost, undoing the
     * whole point of the persistent listener.
     */
    private fun holdBackgroundRadio(hold: Boolean) {
        if (!hold) {
            backgroundWifiLock?.let { if (it.isHeld) it.release() }
            backgroundWakeLock?.let { if (it.isHeld) it.release() }
            return
        }
        (context.getSystemService(Context.WIFI_SERVICE) as? WifiManager)?.let { wifi ->
            @Suppress("DEPRECATION")
            val lock = backgroundWifiLock ?: wifi
                .createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "track-n-race-background")
                .apply { setReferenceCounted(false); backgroundWifiLock = this }
            if (!lock.isHeld) lock.acquire()
        }
        (context.getSystemService(Context.POWER_SERVICE) as? PowerManager)?.let { power ->
            val lock = backgroundWakeLock ?: power
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "track-n-race:listener")
                .apply { setReferenceCounted(false); backgroundWakeLock = this }
            if (!lock.isHeld) lock.acquire()
        }
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
        if (!requireLocalNetwork()) return
        sourceRequested = true
        restartConfiguredSource()
    }

    fun requestParticipants() {
        if (!isDirectSource()) pairedTelemetry.requestParticipants()
    }

    fun requestDriverRestriction() {
        if (!isDirectSource()) pairedTelemetry.requestDriverRestriction()
    }

    /**
     * One car's laps for the Timing page's laps sheet, which asks again while
     * it is open: from the phone's own engine (listening or a recording it
     * plays), else from the desktop. The row arrives through the store either way.
     */
    fun requestDriverLapHistory(car: Int) {
        if (playback != null || isDirectSource()) {
            if (lapExecutor.isShutdown) return
            lapExecutor.execute {
                directTelemetry.driverLapHistory(car)?.let(store::acceptColdRow)
            }
        } else {
            pairedTelemetry.requestDriverLapHistory(car)
        }
    }

    fun setActivePage(page: PairedTelemetryPage) {
        activePage = page
        for ((family, history) in store.graphHistories) history.setEnabled(family in page.graphFamilies)
        pairedTelemetry.setPage(page)
        playbackCommand { directTelemetry.setRequirements(page.streamMask, page.v6Types.toIntArray()) }
        if (!sourceExecutor.isShutdown) sourceExecutor.execute {
            applyLiveStrategy()
            // A graph page asks for its own history once it is up (requestLiveHistory).
            if (page.graphFamilies.isEmpty()) applyLiveHistory()
        }
    }

    /**
     * Listening for the game streams a fixed set of rows whatever page is open,
     * plus the Strategy snapshot while a page shows it. A recording follows the
     * page's own requirements instead. Runs on the source executor, after any
     * listener start it was queued behind.
     */
    private fun applyLiveStrategy() {
        if (playback != null) return
        directTelemetry.setLiveStrategy(activePage.streamMask and DataRow.STRATEGY != 0)
    }

    /**
     * Listening for the game, the engine keeps the whole session, so the open
     * graph page's histories are read back from it: their own rows, the rows
     * their held fields come from, and the lap rows that place samples by
     * distance. Runs on the source executor, as [applyLiveStrategy].
     */
    private fun applyLiveHistory() {
        if (playback != null) return
        directTelemetry.setLiveHistory(liveHistoryRows(activePage), liveHistoryWindow)
    }

    private fun liveHistoryRows(page: PairedTelemetryPage): Int {
        if (page.graphFamilies.isEmpty()) return 0
        var rows = DataRow.LAP
        for (family in page.graphFamilies) for (field in family.fields) {
            rows = rows or when (field.source) {
                RowFamily.STATUS -> DataRow.STATUS
                RowFamily.TELEMETRY -> DataRow.TELEMETRY
                RowFamily.DAMAGE -> DataRow.DAMAGE
                RowFamily.MOTION -> DataRow.MOTION
                RowFamily.MOTION_EX -> DataRow.MOTION_EX
            }
        }
        return rows
    }

    /**
     * Graphs, while listening for the game: has the engine refill the open
     * graph page's histories, [windowSeconds] behind now or the whole session
     * when infinite. False when their samples come from elsewhere: a
     * recording, or the paired desktop.
     */
    private fun requestLiveHistory(windowSeconds: Double): Boolean {
        if (playback != null || store.sourceStatus.state != "listening") return false
        if (activePage.graphFamilies.isEmpty()) return false
        liveHistoryWindow = if (windowSeconds.isFinite()) windowSeconds.toFloat() else -1f
        sourceExecutor.execute { applyLiveHistory() }
        return true
    }

    // ── A recording played on the phone ─────────────────────────────────────

    /**
     * Opens [file] in place of the current source: the game listener or the
     * paired desktop stops until the recording is closed.
     */
    fun openRecording(file: RecordingFile) {
        val dropped = playback
        playback = null
        val generation = ++sourceGeneration
        store.updateSource("opening", file.title)
        sourceExecutor.execute {
            directTelemetry.stop()
            pairedTelemetry.close()
            dropped?.handle?.close()
            if (generation != sourceGeneration) return@execute
            store.resetLapComparison()
            store.beginLocalPlayback(file.title)
            val handle = try {
                RecordingLibrary.open(context, file)
            } catch (error: Exception) {
                failRecording(file, error.message)
                return@execute
            }
            val error = directTelemetry.openRecording(handle.path)
            if (error != null) {
                handle.close()
                failRecording(file, error)
                return@execute
            }
            playback = OpenRecording(file, handle)
            val page = activePage
            directTelemetry.setRequirements(page.streamMask, page.v6Types.toIntArray())
            store.updateSource("playback", file.title)
        }
    }

    private fun failRecording(file: RecordingFile, error: String?) {
        store.endLocalPlayback()
        store.showMessage("Could not open ${file.title}: ${error ?: "unknown error"}")
        restartConfiguredSource()
    }

    /** Closes the recording; the game listener or paired desktop resumes. */
    fun closeRecording() {
        if (playback == null) return
        restartConfiguredSource()
    }

    fun playbackPlay() = playbackCommand { directTelemetry.play() }

    fun playbackPause() = playbackCommand { directTelemetry.pause() }

    fun playbackSetSpeed(speed: Float) = playbackCommand { directTelemetry.setSpeed(speed) }

    fun playbackSetDriver(index: Int) = playbackCommand { directTelemetry.setDriver(index) }

    /** Moves to [fraction] of the recording; a newer seek overtakes this one. */
    fun playbackSeek(fraction: Float) {
        if (playback == null) return
        val id = seekIds.incrementAndGet()
        directTelemetry.requestSeek(id)
        playbackExecutor.execute { if (playback != null) directTelemetry.seek(fraction.coerceIn(0f, 1f), id) }
    }

    private fun playbackCommand(block: () -> Unit) {
        if (playback == null) return
        playbackExecutor.execute { if (playback != null) block() }
    }

    /**
     * Lap columns for Analysis or Graphs: from the phone's recording when one
     * is open, else the desktop. [driver] is a car index, -1 the selected driver.
     */
    private fun requestLapData(requestId: Long, driver: Int, lapNumber: Int, channels: Collection<String>): Boolean {
        if (playback == null) return pairedTelemetry.requestLapData(requestId, lapNumber, channels, driver)
        val names = channels.toTypedArray()
        lapExecutor.execute {
            val data = directTelemetry.lapData(lapNumber, names, forCharts = requestId >= GRAPHS_REQUEST_BASE, driver)
            onLapData("{\"type\":\"lap_data\",\"requestId\":$requestId,\"lapNum\":$lapNumber,\"data\":${data ?: "null"}}")
        }
        return true
    }

    /** A lap delta: from the phone's recording when one is open, else the desktop. */
    private fun requestLapDelta(request: PlaybackLapDeltaRequest): Boolean {
        if (playback == null) return pairedTelemetry.requestLapDelta(request)
        lapExecutor.execute {
            val data = directTelemetry.lapDelta(
                request.currentLap,
                request.comparisonLap,
                request.sectorDelta,
                request.currentDriver,
                request.comparisonDriver,
            )
            store.acceptColdRow("{\"type\":\"lap_delta\",\"requestId\":${request.requestId},\"data\":${data ?: "null"}}")
        }
        return true
    }

    /** Another driver's lap catalogue: from the phone's recording when one is open, else the desktop. */
    private fun requestDriverLaps(requestId: Long, driver: Int): Boolean {
        if (playback == null) return pairedTelemetry.requestDriverLaps(requestId, driver)
        lapExecutor.execute {
            val data = directTelemetry.driverLaps(driver)
            store.acceptColdRow(
                "{\"type\":\"driver_laps\",\"requestId\":$requestId,\"driverIndex\":$driver,\"data\":${data ?: "null"}}",
            )
        }
        return true
    }

    /** Braking zones: from the phone's recording when one is open, else the desktop. */
    private fun requestBrakingZones(request: BrakingZonesRequest): Boolean {
        if (playback == null) return pairedTelemetry.requestBrakingZones(request)
        lapExecutor.execute {
            val data = directTelemetry.brakingZones(
                request.currentLap,
                request.comparisonLap,
                request.currentDriver,
                request.comparisonDriver,
            )
            store.acceptColdRow("{\"type\":\"braking_zones\",\"requestId\":${request.requestId},\"data\":${data ?: "null"}}")
        }
        return true
    }

    /** Analysis map layers: from the phone's recording when one is open, else the desktop. */
    private fun requestAnalysisMap(request: AnalysisMapRequest): Boolean {
        if (playback == null) return pairedTelemetry.requestAnalysisMap(request)
        lapExecutor.execute {
            val data = directTelemetry.analysisMap(request)
            store.acceptColdRow("{\"type\":\"analysis_map\",\"requestId\":${request.requestId},\"data\":${data ?: "null"}}")
        }
        return true
    }

    /** Whether the source can serve another driver's laps: the phone's own reader, or a desktop that says so. */
    private fun driverLapsSupported(): Boolean = playback != null || pairedTelemetry.driverLapsSupported()

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

    /**
     * The Direct UDP listener's port and forwarding destinations. A listener
     * that is running is rebound to them in place, keeping its session.
     */
    fun setUdpSettings(port: Int, forwardingEnabled: Boolean, forwardTargets: List<UdpForwardTarget>) {
        val targets = forwardTargets
            .map { it.copy(address = it.address.trim()) }
            .take(MAX_UDP_FORWARD_TARGETS)
        preferences().edit()
            .putInt(PREF_UDP_PORT, port)
            .putBoolean(PREF_UDP_FORWARDING, forwardingEnabled)
            .putString(PREF_UDP_FORWARD_TARGETS, targets.joinToString("\n") { "${it.address}:${it.port}" })
            .apply()
        publishSettings()
        // A recording stays open; it and a paired desktop pick the listener's
        // settings up when Direct UDP next starts.
        if (!sourceRequested || playback != null || !isDirectSource()) return
        // Nothing bound to rebind (a failed bind, or still starting).
        if (store.sourceStatus.state != "listening") {
            restartConfiguredSource()
            return
        }
        val generation = sourceGeneration
        sourceExecutor.execute {
            if (generation != sourceGeneration) return@execute
            val error = directTelemetry.restartUdp(port, if (forwardingEnabled) targets else emptyList())
            if (error != null) {
                store.updateSource("error", error)
            } else {
                store.updateSource("listening", "UDP $port")
            }
        }
    }

    private fun udpPort(): Int = preferences().getInt(PREF_UDP_PORT, DEFAULT_UDP_PORT)

    private fun storedForwardTargets(): List<UdpForwardTarget> =
        preferences().getString(PREF_UDP_FORWARD_TARGETS, null).orEmpty()
            .lineSequence()
            .mapNotNull { line ->
                val port = line.substringAfterLast(':', "").toIntOrNull() ?: return@mapNotNull null
                UdpForwardTarget(line.substringBeforeLast(':'), port)
            }
            .take(MAX_UDP_FORWARD_TARGETS)
            .toList()

    fun setThemeMode(mode: ThemeMode) {
        preferences().edit().putString(PREF_THEME_MODE, mode.key).apply()
        publishSettings()
    }

    fun setWheelDisplay(enabled: Boolean) {
        preferences().edit().putBoolean(PREF_WHEEL_DISPLAY, enabled).apply()
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
        if (!requireLocalNetwork()) return
        startDiscoveryInternal(clear = true)
    }

    // DNS-SD runs in the system mDNS stack, so no multicast lock is needed.
    private fun startDiscoveryInternal(clear: Boolean) {
        if (!hasLocalNetworkAccess()) return
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
        if (!requireLocalNetwork()) return
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
        if (!requireLocalNetwork()) return
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

    /**
     * Android 17 blocks every LAN socket and NsdManager call for apps targeting
     * it until the user grants ACCESS_LOCAL_NETWORK (the "Nearby devices"
     * permission). Game UDP, desktop discovery and the paired connection are
     * all local traffic; earlier Android versions allow it implicitly.
     */
    fun hasLocalNetworkAccess(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.CINNAMON_BUN ||
            context.checkSelfPermission(Manifest.permission.ACCESS_LOCAL_NETWORK) ==
            PackageManager.PERMISSION_GRANTED

    /** Whether LAN use may go ahead; otherwise asks for access and says why it stopped. */
    private fun requireLocalNetwork(): Boolean {
        if (hasLocalNetworkAccess()) return true
        store.showMessage(context.getString(R.string.local_network_required))
        localNetworkRequester?.invoke()
        return false
    }

    fun onLocalNetworkResult(granted: Boolean) {
        if (!granted) {
            store.showMessage(context.getString(R.string.local_network_required))
            return
        }
        // Start what the missing permission held back.
        if (sourceRequested && playback == null) restartConfiguredSource()
        if (discoveryRequested) startDiscoveryInternal(clear = true)
    }

    fun forgetDesktop() {
        pairedTelemetry.close()
        PairedTelemetryClient.forgetDesktop(context)
        publishSettings()
        if (sourceRequested) restartConfiguredSource()
    }

    override fun onTelemetryRow(json: String) = store.acceptColdRow(json)

    override fun onTelemetryBinary(bytes: ByteArray) = store.acceptBinary(bytes)

    override fun onTelemetryHistory(payload: ByteBuffer) = store.acceptLiveHistory(payload)

    override fun onRow(json: String) = store.acceptColdRow(json)

    override fun onBinary(bytes: ByteArray) = store.acceptBinary(bytes)

    override fun onLapData(json: String) {
        val requestId = AnalysisLapParser.requestIdOf(json) ?: return
        if (requestId >= GRAPHS_REQUEST_BASE) graphs.onLapData(json) else analysis.onLapData(json)
    }

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
        hasSavedDesktop = PairedTelemetryClient.hasSavedDesktop(context),
        desktopName = PairedTelemetryClient.savedDesktopName(context),
        recordingDirectory = RecordingStorage.selectedDirectoryLabel(context),
        usingCustomDirectory = RecordingStorage.selectedDirectory(context) != null,
        themeMode = ThemeMode.fromKey(preferences().getString(PREF_THEME_MODE, null)),
        wheelDisplay = if (preferences().contains(PREF_WHEEL_DISPLAY)) {
            preferences().getBoolean(PREF_WHEEL_DISPLAY, true)
        } else {
            null
        },
        udpPort = udpPort(),
        udpForwardingEnabled = preferences().getBoolean(PREF_UDP_FORWARDING, false),
        udpForwardTargets = storedForwardTargets(),
    )

    private fun publishSettings() = store.updateSettings(currentSettings())

    private fun restartConfiguredSource() {
        val generation = ++sourceGeneration
        val dropped = playback
        playback = null
        sourceExecutor.execute {
            directTelemetry.stop()
            pairedTelemetry.close()
            if (dropped != null) {
                dropped.handle.close()
                store.endLocalPlayback()
            }
            store.resetLapComparison()
            if (!sourceRequested || generation != sourceGeneration) return@execute
            if (!hasLocalNetworkAccess()) {
                store.updateSource("error", context.getString(R.string.local_network_off))
                return@execute
            }
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
            val port = udpPort()
            val forwardTargets =
                if (preferences().getBoolean(PREF_UDP_FORWARDING, false)) storedForwardTargets() else emptyList()
            val error = directTelemetry.start(port, forwardTargets)
            if (error != null) {
                store.updateSource("error", error)
                return@execute
            }
            val recording = preferences().getBoolean(RecordingStorage.PREF_RECORDING, false)
            directTelemetry.setRecording(recording && recordingPath.isNotEmpty(), recordingPath)
            applyLiveStrategy()
            applyLiveHistory()
            store.updateSource("listening", "UDP $port")
        }
    }

    private fun prepareForPairing() {
        pairingPending = true
        store.updatePairingBusy(true)
        sourceGeneration++
        val dropped = playback
        playback = null
        sourceExecutor.execute {
            directTelemetry.stop()
            if (dropped != null) {
                dropped.handle.close()
                store.endLocalPlayback()
            }
        }
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
