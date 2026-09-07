package com.tracknrace.android

import android.content.Context
import android.content.SharedPreferences
import android.provider.Settings
import java.util.UUID
import java.util.concurrent.atomic.AtomicLong
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import org.json.JSONObject

private const val ROW_TELEMETRY = 1 shl 1
private const val ROW_STATUS = 1 shl 2
private const val ROW_DAMAGE = 1 shl 3
private const val ROW_LAP = 1 shl 4
private const val ROW_SESSION = 1 shl 5
private const val ROW_TIMING = 1 shl 7
private const val ROW_PARTICIPANTS = 1 shl 8
private const val ROW_ALL_STATUS = 1 shl 9
private const val ROW_TYRE_SETS = 1 shl 10

/** Complete paired-stream requirement for one visible Android page. */
internal enum class PairedTelemetryPage(
    val pageId: String,
    val streamMask: Int,
) {
    DASHBOARD(
        "dashboard",
        ROW_TELEMETRY or ROW_STATUS or ROW_DAMAGE or ROW_LAP or ROW_SESSION,
    ),
    TIMING(
        "timing",
        ROW_TIMING or ROW_PARTICIPANTS or ROW_ALL_STATUS,
    ),
    TYRES(
        "tyres",
        ROW_SESSION or ROW_TYRE_SETS,
    ),
    NONE("none", 0),
}

internal class PairedTelemetryClient(
    context: Context,
    private val listener: Listener,
) {
    interface Listener {
        fun onRow(json: String)

        fun onBinary(bytes: ByteArray) = Unit

        fun onState(state: String, detail: String?)

        fun onPaired() = Unit
    }

    data class Endpoint(
        val serverId: String,
        val name: String,
        val host: String,
        val port: Int,
    )

    companion object {
        const val PREF_SOURCE = "telemetry.source"
        const val SOURCE_DIRECT = "direct"
        const val SOURCE_PAIRED = "paired"

        private const val PREF_SERVER_ID = "pairing.server_id"
        private const val PREF_SERVER_NAME = "pairing.server_name"
        private const val PREF_HOST = "pairing.host"
        private const val PREF_PORT = "pairing.port"
        private const val PREF_TOKEN = "pairing.token"
        private const val PREF_DEVICE_ID = "pairing.device_id"
        private const val PAIR_PROTOCOL_VERSION = 1
        private const val BINARY_ROWS_VERSION = 2

        private fun preferences(context: Context): SharedPreferences =
            RecordingStorage.preferences(context)

        fun hasSavedDesktop(context: Context): Boolean =
            preferences(context).getString(PREF_TOKEN, "").orEmpty().isNotEmpty()

        fun savedDesktopName(context: Context): String =
            preferences(context).getString(PREF_SERVER_NAME, "Paired desktop")
                ?: "Paired desktop"

        fun forgetDesktop(context: Context) {
            preferences(context).edit()
                .remove(PREF_SERVER_ID)
                .remove(PREF_SERVER_NAME)
                .remove(PREF_HOST)
                .remove(PREF_PORT)
                .remove(PREF_TOKEN)
                .putString(PREF_SOURCE, SOURCE_DIRECT)
                .apply()
        }
    }

    private val context = context.applicationContext
    private val http = OkHttpClient.Builder().retryOnConnectionFailure(true).build()

    @Volatile
    private var socket: WebSocket? = null

    private val subscriptionLock = Any()
    private val subscriptionIds = AtomicLong()
    private var subscribedSocket: WebSocket? = null
    private var activePage = PairedTelemetryPage.DASHBOARD

    fun setPage(page: PairedTelemetryPage) {
        synchronized(subscriptionLock) {
            if (activePage == page) return
            activePage = page
            subscribedSocket?.let { sendSubscription(it, page) }
        }
    }

    fun requestParticipants() {
        val active = synchronized(subscriptionLock) { subscribedSocket }
        active?.send(
            JSONObject()
                .put("type", "request_latest")
                .put("rowType", "participants")
                .toString(),
        )
    }

    fun connectSaved() {
        val preferences = preferences(context)
        val endpoint = Endpoint(
            serverId = preferences.getString(PREF_SERVER_ID, "").orEmpty(),
            name = preferences.getString(PREF_SERVER_NAME, "Desktop") ?: "Desktop",
            host = preferences.getString(PREF_HOST, "").orEmpty(),
            port = preferences.getInt(PREF_PORT, 20779),
        )
        connect(endpoint, secret = null, code = null, token = preferences.getString(PREF_TOKEN, ""))
    }

    fun pair(endpoint: Endpoint, secret: String?, code: String?) {
        connect(endpoint, secret, code, token = null)
    }

    private fun connect(endpoint: Endpoint, secret: String?, code: String?, token: String?) {
        close()
        if (endpoint.host.isEmpty() || endpoint.port !in 1..65535) {
            listener.onState("error", "Invalid desktop address")
            return
        }

        listener.onState("connecting", endpoint.name)
        val request = Request.Builder()
            .url("ws://${endpoint.host}:${endpoint.port}")
            .build()
        socket = http.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                if (socket !== webSocket) {
                    webSocket.close(1000, "Superseded connection")
                    return
                }
                try {
                    val hello = JSONObject()
                        .put("type", if (token == null) "pair" else "resume")
                        .put("pairProtocol", PAIR_PROTOCOL_VERSION)
                        .put("binaryRowsVersion", BINARY_ROWS_VERSION)
                        .put("deviceId", deviceId())
                        .put("name", "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}")
                    secret?.let { hello.put("secret", it) }
                    code?.let { hello.put("code", it) }
                    token?.let { hello.put("token", it) }
                    webSocket.send(hello.toString())
                } catch (error: Exception) {
                    listener.onState("error", error.message)
                }
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                if (socket === webSocket) handleText(webSocket, endpoint, text)
            }

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                if (socket === webSocket) listener.onBinary(bytes.toByteArray())
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                if (socket !== webSocket) return
                socket = null
                clearSubscribedSocket(webSocket)
                listener.onState("disconnected", reason)
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                if (socket !== webSocket) return
                socket = null
                clearSubscribedSocket(webSocket)
                listener.onState("error", t.message)
            }
        })
    }

    private fun handleText(webSocket: WebSocket, endpoint: Endpoint, text: String) {
        try {
            val message = JSONObject(text)
            when (message.optString("type")) {
                "welcome" -> handleWelcome(webSocket, endpoint, message)
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
                "error" -> listener.onState(
                    "error",
                    message.optString("code", "Pairing failed"),
                )
            }
        } catch (error: Exception) {
            listener.onState("error", error.message)
        }
    }

    private fun handleWelcome(webSocket: WebSocket, endpoint: Endpoint, message: JSONObject) {
        if (message.optInt("pairProtocol", -1) != PAIR_PROTOCOL_VERSION ||
            message.optInt("binaryRowsVersion", -1) != BINARY_ROWS_VERSION
        ) {
            socket = null
            webSocket.close(1002, "Unsupported telemetry protocol version")
            listener.onState("error", "Desktop app needs the matching Track N Race version")
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
            listener.onRow(protocolContext.toString())
        }

        val token = message.optString("token")
        preferences(context).edit()
            .putString(PREF_SERVER_ID, endpoint.serverId)
            .putString(PREF_SERVER_NAME, endpoint.name)
            .putString(PREF_HOST, endpoint.host)
            .putInt(PREF_PORT, endpoint.port)
            .putString(PREF_TOKEN, token)
            .putString(PREF_SOURCE, SOURCE_PAIRED)
            .apply()

        // Invalidate the previous connection's roster before asking the
        // desktop for this page's snapshot. Doing this from onPaired() happens
        // after subscribe() and can race the snapshot, clearing names that
        // have already arrived.
        listener.onRow("{\"type\":\"participants_reset\"}")
        synchronized(subscriptionLock) {
            if (socket !== webSocket) return
            subscribedSocket = webSocket
            sendSubscription(webSocket, activePage)
        }
        listener.onState("connected", endpoint.name)
        listener.onPaired()
    }

    private fun sendSubscription(webSocket: WebSocket, page: PairedTelemetryPage) {
        webSocket.send(
            JSONObject()
                .put("type", "subscribe")
                .put("pageId", page.pageId)
                .put("streamMask", page.streamMask)
                .put("historyMask", 0)
                .put("backfill", "none")
                .put("requestId", subscriptionIds.incrementAndGet())
                .toString(),
        )
    }

    private fun clearSubscribedSocket(webSocket: WebSocket) {
        synchronized(subscriptionLock) {
            if (subscribedSocket === webSocket) subscribedSocket = null
        }
    }

    private fun deviceId(): String {
        val preferences = preferences(context)
        preferences.getString(PREF_DEVICE_ID, "").orEmpty()
            .takeIf(String::isNotEmpty)
            ?.let { return it }

        val id = Settings.Secure.getString(context.contentResolver, Settings.Secure.ANDROID_ID)
            ?.takeIf(String::isNotEmpty)
            ?: UUID.randomUUID().toString()
        preferences.edit().putString(PREF_DEVICE_ID, id).apply()
        return id
    }

    fun close() {
        val active = socket
        socket = null
        synchronized(subscriptionLock) {
            subscribedSocket = null
        }
        active?.close(1000, "Android page closed")
    }
}
