package com.tracknrace.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.provider.Settings;

import androidx.annotation.Nullable;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.UUID;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

final class PairedTelemetryClient {
    static final String PREF_SOURCE = "telemetry.source";
    static final String SOURCE_DIRECT = "direct";
    static final String SOURCE_PAIRED = "paired";
    private static final String PREF_SERVER_ID = "pairing.server_id";
    private static final String PREF_SERVER_NAME = "pairing.server_name";
    private static final String PREF_HOST = "pairing.host";
    private static final String PREF_PORT = "pairing.port";
    private static final String PREF_TOKEN = "pairing.token";
    private static final int PAIR_PROTOCOL_VERSION = 1;
    private static final int BINARY_ROWS_VERSION = 2;
    private static final int RACE_DASHBOARD_MASK = (1 << 1) | (1 << 2) | (1 << 4) | (1 << 5);

    interface Listener {
        void onRow(String json);
        default void onBinary(byte[] bytes) {}
        void onState(String state, @Nullable String detail);
        default void onPaired() {}
    }

    static final class Endpoint {
        final String serverId;
        final String name;
        final String host;
        final int port;
        Endpoint(String serverId, String name, String host, int port) {
            this.serverId = serverId;
            this.name = name;
            this.host = host;
            this.port = port;
        }
    }

    private final Context context;
    private final Listener listener;
    private final OkHttpClient http = new OkHttpClient.Builder().retryOnConnectionFailure(true).build();
    private volatile WebSocket socket;

    PairedTelemetryClient(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.listener = listener;
    }

    static SharedPreferences preferences(Context context) {
        return RecordingStorage.preferences(context);
    }

    static boolean hasSavedDesktop(Context context) {
        return !preferences(context).getString(PREF_TOKEN, "").isEmpty();
    }

    static String savedDesktopName(Context context) {
        return preferences(context).getString(PREF_SERVER_NAME, "Paired desktop");
    }

    static void forgetDesktop(Context context) {
        preferences(context).edit()
            .remove(PREF_SERVER_ID).remove(PREF_SERVER_NAME).remove(PREF_HOST)
            .remove(PREF_PORT).remove(PREF_TOKEN)
            .putString(PREF_SOURCE, SOURCE_DIRECT).apply();
    }

    void connectSaved() {
        SharedPreferences prefs = preferences(context);
        Endpoint endpoint = new Endpoint(
            prefs.getString(PREF_SERVER_ID, ""), prefs.getString(PREF_SERVER_NAME, "Desktop"),
            prefs.getString(PREF_HOST, ""), prefs.getInt(PREF_PORT, 20779));
        connect(endpoint, null, null, prefs.getString(PREF_TOKEN, ""));
    }

    void pair(Endpoint endpoint, @Nullable String secret, @Nullable String code) {
        connect(endpoint, secret, code, null);
    }

    private void connect(Endpoint endpoint, @Nullable String secret,
                         @Nullable String code, @Nullable String token) {
        close();
        if (endpoint.host.isEmpty() || endpoint.port < 1 || endpoint.port > 65535) {
            listener.onState("error", "Invalid desktop address");
            return;
        }
        listener.onState("connecting", endpoint.name);
        Request request = new Request.Builder()
            .url("ws://" + endpoint.host + ":" + endpoint.port).build();
        socket = http.newWebSocket(request, new WebSocketListener() {
            @Override public void onOpen(WebSocket webSocket, Response response) {
                if (socket != webSocket) {
                    webSocket.close(1000, "Superseded connection");
                    return;
                }
                try {
                    JSONObject hello = new JSONObject()
                        .put("type", token == null ? "pair" : "resume")
                        .put("pairProtocol", PAIR_PROTOCOL_VERSION)
                        .put("binaryRowsVersion", BINARY_ROWS_VERSION)
                        .put("deviceId", deviceId())
                        .put("name", android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL);
                    if (secret != null) hello.put("secret", secret);
                    if (code != null) hello.put("code", code);
                    if (token != null) hello.put("token", token);
                    webSocket.send(hello.toString());
                } catch (Exception error) {
                    listener.onState("error", error.getMessage());
                }
            }

            @Override public void onMessage(WebSocket webSocket, String text) {
                if (socket != webSocket) return;
                handleText(webSocket, endpoint, text);
            }

            @Override public void onMessage(WebSocket webSocket, ByteString bytes) {
                if (socket != webSocket) return;
                listener.onBinary(bytes.toByteArray());
            }

            @Override public void onClosed(WebSocket webSocket, int code, String reason) {
                if (socket != webSocket) return;
                socket = null;
                listener.onState("disconnected", reason);
            }

            @Override public void onFailure(WebSocket webSocket, Throwable error, Response response) {
                if (socket != webSocket) return;
                socket = null;
                listener.onState("error", error.getMessage());
            }
        });
    }

    private void handleText(WebSocket webSocket, Endpoint endpoint, String text) {
        try {
            JSONObject message = new JSONObject(text);
            String type = message.optString("type");
            if ("welcome".equals(type)) {
                if (message.optInt("pairProtocol", -1) != PAIR_PROTOCOL_VERSION
                        || message.optInt("binaryRowsVersion", -1) != BINARY_ROWS_VERSION) {
                    socket = null;
                    webSocket.close(1002, "Unsupported telemetry protocol version");
                    listener.onState("error", "Desktop app needs the matching Track N Race version");
                    return;
                }
                int protocolYear = message.optInt("protocolYear", 0);
                if (protocolYear > 0) {
                    JSONObject context = new JSONObject()
                        .put("type", "protocol_context")
                        .put("protocol_year", protocolYear);
                    if (message.has("formula") && !message.isNull("formula")) {
                        context.put("formula", message.getInt("formula"));
                    }
                    listener.onRow(context.toString());
                }
                String token = message.optString("token");
                preferences(context).edit()
                    .putString(PREF_SERVER_ID, endpoint.serverId)
                    .putString(PREF_SERVER_NAME, endpoint.name)
                    .putString(PREF_HOST, endpoint.host)
                    .putInt(PREF_PORT, endpoint.port)
                    .putString(PREF_TOKEN, token)
                    .putString(PREF_SOURCE, SOURCE_PAIRED).apply();
                webSocket.send(new JSONObject().put("type", "subscribe")
                    .put("page", "race-dashboard").put("streamMask", RACE_DASHBOARD_MASK)
                    .put("historyMask", 0).put("backfill", "none").toString());
                listener.onState("connected", endpoint.name);
                listener.onPaired();
            } else if ("rows".equals(type)) {
                JSONArray rows = message.optJSONArray("rows");
                if (rows != null) for (int i = 0; i < rows.length(); ++i)
                    listener.onRow(rows.optString(i));
            } else if ("error".equals(type)) {
                listener.onState("error", message.optString("code", "Pairing failed"));
            }
        } catch (Exception error) {
            listener.onState("error", error.getMessage());
        }
    }

    private String deviceId() {
        SharedPreferences prefs = preferences(context);
        String id = prefs.getString("pairing.device_id", "");
        if (!id.isEmpty()) return id;
        id = Settings.Secure.getString(context.getContentResolver(), Settings.Secure.ANDROID_ID);
        if (id == null || id.isEmpty()) id = UUID.randomUUID().toString();
        prefs.edit().putString("pairing.device_id", id).apply();
        return id;
    }

    void close() {
        WebSocket active = socket;
        socket = null;
        if (active != null) active.close(1000, "Android page closed");
    }
}
