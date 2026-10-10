package com.tracknrace.android

import android.content.Context
import android.os.Trace
import android.provider.Settings
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong
import kotlin.random.Random
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import okio.ByteString.Companion.toByteString
import org.json.JSONArray
import org.json.JSONObject

/**
 * The data one visible Android page needs, as the consumers it shows. The
 * desktop is asked for exactly their union: row families and V6 fields.
 */
internal enum class PairedTelemetryPage(
    val pageId: String,
    val consumers: List<DataConsumer>,
) {
    // DRIVER_ROSTER is on every top-bar page: the app bar names the selected
    // driver, and the desktop only sends the roster to a subscription that asks.
    DASHBOARD(
        "dashboard",
        listOf(
            DataConsumer.DRIVER_ROSTER,
            DataConsumer.SESSION_INFO,
            DataConsumer.DRIVING_INPUTS,
            DataConsumer.TYRE_TEMPERATURES,
            DataConsumer.POWER_UNIT,
            DataConsumer.FITTED_TYRE,
            DataConsumer.LAP_PROGRESS,
            // Race intervals to the cars ahead and behind.
            DataConsumer.TIMING_TOWER,
        ),
    ),
    TIMING(
        "timing",
        listOf(
            DataConsumer.DRIVER_ROSTER,
            DataConsumer.TIMING_TOWER,
            DataConsumer.TIMING_TYRES,
        ),
    ),
    TYRES(
        "tyres",
        listOf(
            DataConsumer.DRIVER_ROSTER,
            DataConsumer.SESSION_INFO,
            DataConsumer.TYRE_SETS,
        ),
    ),
    // Lap samples are requested one lap at a time (request_lap_data), so the
    // stream only needs the track and the roster; the playback cursor and lap
    // catalogue are control rows every phone receives.
    ANALYSIS(
        "analysis",
        listOf(
            DataConsumer.DRIVER_ROSTER,
            DataConsumer.SESSION_INFO,
        ),
    ),
    NONE("none", emptyList());

    /** Row families this page needs: the union of its consumers'. */
    val streamMask: Int = consumers.fold(0) { mask, consumer -> mask or consumer.streamMask }

    /** V6 fields this page needs: the union of its consumers', ascending. */
    val v6Types: List<Int> = consumers.flatMap { it.v6Types }.distinct().sorted()
}

internal class PairedTelemetryClient(
    context: Context,
    private val listener: Listener,
) {
    interface Listener {
        fun onRow(json: String)

        fun onBinary(bytes: ByteArray) = Unit

        fun onState(state: String, detail: String?)

        /** A new desktop was paired and saved. */
        fun onPaired() = Unit

        /** The saved desktop answered again. */
        fun onDesktopReached() = Unit

        /** Retrying the saved desktop: look for it at a new address. */
        fun onSearchForDesktop(serverId: String) = Unit

        /** A `lap_data` reply, still as text: it is parsed straight into columns. */
        fun onLapData(json: String) = Unit
    }

    data class Endpoint(
        val serverId: String,
        val name: String,
        val host: String,
        val port: Int,
    )

    /** How a connection proves itself; see PairCrypto.h. */
    sealed interface Credential {
        data class Resume(val identityKey: String, val token: String) : Credential

        data class Qr(val identityKey: String, val secret: String) : Credential

        data class Code(val code: String) : Credential
    }

    companion object {
        const val PREF_SOURCE = "telemetry.source"
        const val SOURCE_DIRECT = "direct"
        const val SOURCE_PAIRED = "paired"

        private const val PREF_DEVICE_ID = "pairing.device_id"
        // 3: encrypted channel and desktop approval. 2: v6Types and patch rows.
        private const val PAIR_PROTOCOL_VERSION = 3
        private const val BINARY_ROWS_VERSION = 2
        private const val LAP_DATA_PREFIX = "{\"type\":\"lap_data\""
        // Backoff while the saved desktop is unreachable, with ±20 % jitter.
        private val RETRY_DELAYS_MS = longArrayOf(500, 1_000, 2_000, 4_000, 8_000)

        // Refusals that another attempt cannot fix.
        private val FINAL_REFUSALS = setOf(
            "unknown_device",
            "desktop_identity_mismatch",
            "unsupported_pair_protocol",
            "unsupported_binary_rows",
        )

        fun hasSavedDesktop(context: Context): Boolean = PairingStore.load(context) != null

        fun savedDesktopName(context: Context): String =
            PairingStore.load(context)?.name ?: "Paired desktop"

        fun forgetDesktop(context: Context) {
            PairingStore.clear(context)
            RecordingStorage.preferences(context).edit()
                .putString(PREF_SOURCE, SOURCE_DIRECT)
                .apply()
        }

        fun describe(code: String?): String = when (code) {
            "pairing_closed" -> "Pairing is not open on the desktop. Select Pair a device there first."
            "pairing_busy" -> "The desktop is already pairing another phone."
            "invalid_pairing_code" -> "That code does not match the desktop."
            "pairing_denied" -> "The desktop did not allow this phone."
            "approval_timeout" -> "The desktop did not allow this phone in time."
            "unknown_device" -> "The desktop no longer knows this phone. Pair again."
            "desktop_identity_mismatch" ->
                "This is not the desktop this phone paired with. Pair again if Track N Race was reinstalled there."
            "unsupported_pair_protocol", "unsupported_binary_rows" ->
                "Desktop app needs the matching Track N Race version"
            "pairing_failed" -> "Pairing failed. Try again."
            "protocol_error", "invalid_server_hello", "invalid_handshake" ->
                "The desktop's reply could not be verified."
            null, "" -> "Desktop disconnected"
            else -> code
        }
    }

    private val context = context.applicationContext
    private val http = OkHttpClient.Builder().retryOnConnectionFailure(true).build()
    private val scheduler = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "tnr-pair-retry").apply { isDaemon = true }
    }

    // Guards current, retry state and the active page.
    private val lock = Any()
    private var current: Connection? = null
    private var activePage = PairedTelemetryPage.DASHBOARD
    private val subscriptionIds = AtomicLong()

    // Retrying is on while the saved desktop is the wanted source.
    private var retrySaved = false
    private var retryAttempt = 0
    private var retryTask: ScheduledFuture<*>? = null

    fun setPage(page: PairedTelemetryPage) {
        val connection = synchronized(lock) {
            if (activePage == page) return
            activePage = page
            current?.takeIf { it.admitted }
        }
        connection?.subscribe(page)
    }

    private fun admitted(): Connection? = synchronized(lock) { current?.takeIf { it.admitted } }

    fun requestParticipants() {
        admitted()?.send(
            JSONObject()
                .put("type", "request_latest")
                .put("rowType", "participants")
                .toString(),
        )
    }

    /**
     * Asks the desktop to restate the selected driver's telemetry restriction,
     * for when the subscription snapshot was missed or the connection dropped.
     * The desktop always replies, with driver -1 when nothing is playing.
     */
    fun requestDriverRestriction() {
        admitted()?.takeIf { it.driverRestrictionSupported }?.send(
            JSONObject()
                .put("type", "request_latest")
                .put("rowType", "driver_restriction")
                .toString(),
        )
    }

    fun requestLapDelta(request: PlaybackLapDeltaRequest): Boolean {
        val connection = admitted()?.takeIf { it.lapDeltaSupported } ?: return false
        return connection.send(
            JSONObject()
                .put("type", "request_lap_delta")
                .put("requestId", request.requestId)
                .put("currentLap", request.currentLap)
                .put("comparisonLap", request.comparisonLap)
                .put("sectorDelta", request.sectorDelta)
                .toString(),
        )
    }

    /**
     * Asks for one lap of the desktop's selected driver, reduced to the
     * "family.field" channels given. False when the desktop cannot answer.
     */
    fun requestLapData(requestId: Long, lapNumber: Int, channels: Collection<String>): Boolean {
        val connection = admitted()?.takeIf { it.lapDataSupported } ?: return false
        return connection.send(
            JSONObject()
                .put("type", "request_lap_data")
                .put("requestId", requestId)
                .put("lapNum", lapNumber)
                .put("channels", JSONArray(channels.toList()))
                .toString(),
        )
    }

    /** Connects to the saved desktop and keeps retrying until [close]. */
    fun connectSaved() {
        synchronized(lock) {
            retrySaved = true
            retryAttempt = 0
        }
        openSaved()
    }

    fun pair(endpoint: Endpoint, credential: Credential) {
        close()
        open(endpoint, credential)
    }

    /**
     * The saved desktop was discovered, possibly at a new address. While it
     * is unreachable this retries at once instead of waiting out the backoff.
     */
    fun onDesktopFound(serverId: String, host: String, port: Int) {
        val saved = PairingStore.load(context) ?: return
        if (saved.serverId != serverId) return
        val moved = saved.host != host || saved.port != port
        if (moved) PairingStore.updateEndpoint(context, host, port)
        val retryNow = synchronized(lock) {
            val connection = current
            retrySaved && (connection == null || (moved && !connection.admitted))
        }
        if (retryNow) openSaved()
    }

    private fun openSaved() {
        val saved = PairingStore.load(context)
        if (saved == null) {
            listener.onState("error", "No paired desktop is saved")
            return
        }
        val retrying = synchronized(lock) {
            if (!retrySaved) return
            retryTask?.cancel(false)
            retryTask = null
            retryAttempt > 0
        }
        open(
            Endpoint(saved.serverId, saved.name, saved.host, saved.port),
            Credential.Resume(saved.identityKey, saved.token),
            announce = !retrying,
        )
    }

    // announce is false for retries, which keep showing "reconnecting".
    private fun open(endpoint: Endpoint, credential: Credential, announce: Boolean = true) {
        if (endpoint.host.isEmpty() || endpoint.port !in 1..65535) {
            listener.onState("error", "Invalid desktop address")
            return
        }
        val channel = when (credential) {
            is Credential.Resume ->
                PairChannel.begin(PairChannel.Mode.RESUME, endpoint.serverId, credential.identityKey, "")
            is Credential.Qr ->
                PairChannel.begin(PairChannel.Mode.QR, endpoint.serverId, credential.identityKey, "")
            is Credential.Code ->
                PairChannel.begin(PairChannel.Mode.CODE, endpoint.serverId, "", credential.code)
        }.getOrElse { error ->
            listener.onState("error", error.message)
            return
        }
        val connection = Connection(endpoint, credential, channel)
        val previous = synchronized(lock) {
            val replaced = current
            current = connection
            replaced
        }
        previous?.shutdown("Superseded connection")
        if (announce) listener.onState("connecting", endpoint.name)
        connection.start()
    }

    fun close() {
        val connection = synchronized(lock) {
            retrySaved = false
            retryTask?.cancel(false)
            retryTask = null
            val active = current
            current = null
            active
        }
        connection?.shutdown("Android page closed")
    }

    private fun deviceId(): String {
        val preferences = RecordingStorage.preferences(context)
        preferences.getString(PREF_DEVICE_ID, "").orEmpty()
            .takeIf(String::isNotEmpty)
            ?.let { return it }

        val id = Settings.Secure.getString(context.contentResolver, Settings.Secure.ANDROID_ID)
            ?.takeIf(String::isNotEmpty)
            ?: UUID.randomUUID().toString()
        preferences.edit().putString(PREF_DEVICE_ID, id).apply()
        return id
    }

    // The name the user knows the phone by (Settings ▸ About phone, e.g.
    // "Galaxy M12"). Some makers report Build.MANUFACTURER in lowercase.
    private fun deviceName(): String {
        Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME)
            ?.trim()
            ?.takeIf(String::isNotEmpty)
            ?.let { return it }
        val manufacturer = android.os.Build.MANUFACTURER.replaceFirstChar(Char::uppercaseChar)
        val model = android.os.Build.MODEL
        return if (model.startsWith(manufacturer, ignoreCase = true)) model else "$manufacturer $model"
    }

    // Called once per connection, after it ended for any reason.
    private fun ended(connection: Connection, detail: String?) {
        val refusal = connection.refusal
        val retry = synchronized(lock) {
            if (current !== connection) return
            current = null
            // A pairing attempt is never retried; a paired connection is.
            val again = retrySaved &&
                (connection.credential is Credential.Resume || connection.admitted) &&
                (refusal == null || refusal !in FINAL_REFUSALS)
            if (!again) retrySaved = false
            again
        }
        if (refusal == "unknown_device") forgetDesktop(context)
        if (retry) {
            scheduleRetry(connection.endpoint)
            return
        }
        if (refusal != null) {
            listener.onState("error", describe(refusal))
        } else {
            listener.onState("disconnected", detail?.takeIf(String::isNotBlank) ?: describe(null))
        }
    }

    private fun scheduleRetry(endpoint: Endpoint) {
        synchronized(lock) {
            if (!retrySaved) return
            val base = RETRY_DELAYS_MS[minOf(retryAttempt, RETRY_DELAYS_MS.lastIndex)]
            retryAttempt++
            val delay = (base * (0.8 + Random.nextDouble() * 0.4)).toLong()
            retryTask?.cancel(false)
            retryTask = scheduler.schedule(Runnable { openSaved() }, delay, TimeUnit.MILLISECONDS)
        }
        listener.onState("reconnecting", "Reconnecting to ${endpoint.name}…")
        listener.onSearchForDesktop(endpoint.serverId)
    }

    /** One WebSocket to one desktop, from hello to close. */
    private inner class Connection(
        val endpoint: Endpoint,
        val credential: Credential,
        private val channel: PairChannel,
    ) : WebSocketListener() {
        // Seal and send under one lock: the desktop rejects frames out of order.
        private val sendLock = Any()
        private var socket: WebSocket? = null

        @Volatile private var secured = false

        @Volatile var admitted = false
            private set

        @Volatile var refusal: String? = null
            private set

        @Volatile private var finished = false

        @Volatile var lapDeltaSupported = false
            private set

        @Volatile var lapDataSupported = false
            private set

        @Volatile var driverRestrictionSupported = false
            private set

        fun start() {
            val request = Request.Builder()
                .url("ws://${endpoint.host}:${endpoint.port}")
                .build()
            synchronized(sendLock) { socket = http.newWebSocket(request, this) }
        }

        fun send(text: String): Boolean = synchronized(sendLock) {
            val active = socket ?: return false
            if (!secured) return false
            val sealed = channel.seal(text) ?: return false
            active.send(sealed.toByteString())
        }

        fun subscribe(page: PairedTelemetryPage) {
            send(
                JSONObject()
                    .put("type", "subscribe")
                    .put("pageId", page.pageId)
                    .put("streamMask", page.streamMask)
                    .put("v6Types", JSONArray(page.v6Types))
                    .put("historyMask", 0)
                    .put("backfill", "none")
                    .put("requestId", subscriptionIds.incrementAndGet())
                    .toString(),
            )
        }

        /** Closed by this phone: no state report, no retry. */
        fun shutdown(reason: String) {
            finished = true
            synchronized(sendLock) {
                socket?.close(1000, reason)
                socket = null
            }
            channel.close()
        }

        private fun refuse(code: String, webSocket: WebSocket) {
            refusal = code
            webSocket.close(1000, null)
        }

        private fun finish(detail: String?) {
            if (finished) return
            finished = true
            channel.close()
            ended(this, detail)
        }

        override fun onOpen(webSocket: WebSocket, response: Response) {
            if (finished) {
                webSocket.close(1000, "Superseded connection")
                return
            }
            // The hello holds only public values: an ephemeral key, and for
            // code pairing a CPace message. The code itself never leaves.
            webSocket.send(channel.helloJson())
        }

        // Plaintext is only ever the desktop's server_hello or a refusal.
        override fun onMessage(webSocket: WebSocket, text: String) {
            if (finished) return
            if (secured) {
                refuse("protocol_error", webSocket)
                return
            }
            val error = channel.accept(text)
            if (error != null) {
                refuse(error, webSocket)
                return
            }
            secured = true
            val auth = JSONObject()
                .put("type", "auth")
                .put("deviceId", deviceId())
                .put("name", deviceName())
            when (credential) {
                is Credential.Resume -> auth.put("token", credential.token)
                is Credential.Qr -> auth.put("secret", credential.secret)
                is Credential.Code -> auth.put("confirm", channel.confirmation())
            }
            send(auth.toString())
        }

        // Trace sections mark when each frame arrives, so a system trace
        // shows gaps in the desktop's stream against the phone's frames.
        override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
            if (finished) return
            Trace.beginSection("pair frame")
            try {
                val kind = IntArray(1)
                val payload = channel.open(bytes.toByteArray(), kind)
                if (payload == null) {
                    refuse("protocol_error", webSocket)
                    return
                }
                if (kind[0] == PairChannel.KIND_BINARY) {
                    listener.onBinary(payload)
                } else {
                    handleText(webSocket, String(payload, Charsets.UTF_8))
                }
            } finally {
                Trace.endSection()
            }
        }

        override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
            finish(reason)
        }

        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
            finish(t.message)
        }

        private fun handleText(webSocket: WebSocket, text: String) {
            // A lap is hundreds of KB; hand it over unparsed rather than building
            // a JSONObject tree only to walk it once.
            if (text.startsWith(LAP_DATA_PREFIX)) {
                listener.onLapData(text)
                return
            }
            try {
                val message = JSONObject(text)
                when (message.optString("type")) {
                    "welcome" -> handleWelcome(webSocket, message)
                    "approval_pending" -> listener.onState("awaiting_approval", endpoint.name)
                    "rows" -> {
                        val rows = message.optJSONArray("rows") ?: return
                        for (index in 0 until rows.length()) {
                            val row = when (val value = rows.opt(index)) {
                                is String -> value
                                is JSONObject -> value.toString()
                                else -> continue
                            }
                            if (row.isNotBlank()) listener.onRow(row)
                        }
                    }
                    "error" -> refuse(message.optString("code", "pairing_failed"), webSocket)
                }
            } catch (error: Exception) {
                listener.onState("error", error.message)
            }
        }

        private fun handleWelcome(webSocket: WebSocket, message: JSONObject) {
            if (message.optInt("pairProtocol", -1) != PAIR_PROTOCOL_VERSION ||
                message.optInt("binaryRowsVersion", -1) != BINARY_ROWS_VERSION
            ) {
                refuse("unsupported_pair_protocol", webSocket)
                return
            }

            val protocolYear = message.optInt("protocolYear", 0)
            if (protocolYear > 0) {
                val protocolContext = JSONObject()
                    .put("type", "protocol_context")
                    .put("protocol_year", protocolYear)
                if (message.has("formula") && !message.isNull("formula")) {
                    protocolContext.put("formula", message.getInt("formula"))
                }
                if (message.has("regulations2026") && !message.isNull("regulations2026")) {
                    protocolContext.put("regulations_2026", message.getBoolean("regulations2026"))
                }
                listener.onRow(protocolContext.toString())
            }

            val desktopName = message.optString("name").ifEmpty { endpoint.name }
            val newlyPaired = credential !is Credential.Resume
            if (newlyPaired) {
                val token = message.optString("token")
                if (token.isEmpty()) {
                    refuse("pairing_failed", webSocket)
                    return
                }
                PairingStore.save(
                    context,
                    PairingStore.SavedDesktop(
                        serverId = channel.serverId(),
                        name = desktopName,
                        host = endpoint.host,
                        port = endpoint.port,
                        identityKey = channel.identityKey(),
                        token = token,
                    ),
                )
            } else {
                PairingStore.updateEndpoint(context, endpoint.host, endpoint.port)
                PairingStore.updateName(context, desktopName)
            }
            RecordingStorage.preferences(context).edit()
                .putString(PREF_SOURCE, SOURCE_PAIRED)
                .apply()

            // Invalidate the previous connection's roster before asking the
            // desktop for this page's snapshot. Doing this from onPaired() happens
            // after subscribe() and can race the snapshot, clearing names that
            // have already arrived.
            listener.onRow("{\"type\":\"participants_reset\"}")
            val capabilities = message.optJSONArray("capabilities")
            val advertised = buildSet {
                if (capabilities != null) {
                    repeat(capabilities.length()) { add(capabilities.optString(it)) }
                }
            }
            lapDeltaSupported = "lap-delta" in advertised
            lapDataSupported = "lap-data" in advertised
            driverRestrictionSupported = "driver-restriction" in advertised
            val page = synchronized(lock) {
                if (current !== this) return
                admitted = true
                retryAttempt = 0
                // A new pairing becomes the saved desktop to keep reconnecting to.
                retrySaved = true
                activePage
            }
            subscribe(page)
            listener.onState("connected", desktopName)
            if (newlyPaired) listener.onPaired() else listener.onDesktopReached()
        }
    }
}
