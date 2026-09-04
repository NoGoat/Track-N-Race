package com.tracknrace.android;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.util.Base64;

import androidx.activity.result.ActivityResult;
import androidx.annotation.Nullable;
import androidx.webkit.WebMessageCompat;
import androidx.webkit.WebViewCompat;
import androidx.webkit.WebViewFeature;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.ActivityCallback;
import com.getcapacitor.annotation.CapacitorPlugin;
import com.google.zxing.integration.android.IntentIntegrator;
import com.google.zxing.integration.android.IntentResult;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayDeque;
import java.util.Collections;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Native owner for the Capacitor UI.
 *
 * Cold/control rows use ordinary plugin events. Hot rows never enter the JSON
 * bridge: complete BinaryRows.h batches are appended to an immediate drain and
 * posted to the WebView as an ArrayBuffer. There is no pacing timer and no
 * latest-value replacement in this layer.
 */
@CapacitorPlugin(name = "Telemetry")
public final class TelemetryPlugin extends Plugin implements NativeTelemetry.Listener,
    PairedTelemetryClient.Listener, NativePairDiscovery.Listener {
    private static final int UDP_PORT = 20777;
    private static final int MAX_PENDING_HOT_BYTES = 8 * 1024 * 1024;
    private static final int HOT_ENVELOPE_BYTES = 12;
    private static final Uri WEBVIEW_ORIGIN = Uri.parse("https://localhost");

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService sourceExecutor = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "tnrp-capacitor-source");
        thread.setDaemon(true);
        return thread;
    });
    private final Object hotLock = new Object();
    private final ArrayDeque<byte[]> pendingHot = new ArrayDeque<>();
    private int pendingHotBytes;
    private final Object coldLock = new Object();
    private final StringBuilder pendingCold = new StringBuilder(16 * 1024);

    private NativeTelemetry directTelemetry;
    private PairedTelemetryClient pairedTelemetry;
    private NativePairDiscovery discovery;
    private WifiManager.MulticastLock multicastLock;
    private volatile boolean sourceRequested;
    private volatile int sourceGeneration;
    private boolean hotDrainPosted;
    private boolean coldDrainPosted;
    private int hotSequence;
    private long hotOverflowBytes;
    private volatile PluginCall pendingPairCall;

    @Override public void load() {
        directTelemetry = new NativeTelemetry(this);
        pairedTelemetry = new PairedTelemetryClient(getContext(), this);
        discovery = new NativePairDiscovery(this);
    }

    @PluginMethod public void start(PluginCall call) {
        sourceRequested = true;
        restartConfiguredSource();
        call.resolve(settings());
    }

    @PluginMethod public void stop(PluginCall call) {
        sourceRequested = false;
        stopSourcesAsync();
        call.resolve();
    }

    @PluginMethod public void exitApp(PluginCall call) {
        call.resolve();
        getActivity().finish();
    }

    @PluginMethod public void getSettings(PluginCall call) {
        call.resolve(settings());
    }

    @PluginMethod public void reconnect(PluginCall call) {
        if (isDirectSource()) {
            call.reject("Reconnect is only available for a paired desktop");
            return;
        }
        if (!PairedTelemetryClient.hasSavedDesktop(getContext())) {
            call.reject("No paired desktop is saved");
            return;
        }
        sourceRequested = true;
        restartConfiguredSource();
        call.resolve();
    }

    @PluginMethod public void setSource(PluginCall call) {
        String source = call.getString("source", PairedTelemetryClient.SOURCE_DIRECT);
        if (!PairedTelemetryClient.SOURCE_DIRECT.equals(source) &&
            !PairedTelemetryClient.SOURCE_PAIRED.equals(source)) {
            call.reject("Unknown telemetry source");
            return;
        }
        if (PairedTelemetryClient.SOURCE_PAIRED.equals(source) &&
            !PairedTelemetryClient.hasSavedDesktop(getContext())) {
            call.reject("Pair a desktop before selecting paired mode");
            return;
        }
        preferences().edit().putString(PairedTelemetryClient.PREF_SOURCE, source).apply();
        if (sourceRequested) restartConfiguredSource();
        call.resolve(settings());
    }

    @PluginMethod public void setRecording(PluginCall call) {
        boolean enabled = Boolean.TRUE.equals(call.getBoolean("enabled", false));
        preferences().edit().putBoolean(RecordingStorage.PREF_RECORDING, enabled).apply();
        if (isDirectSource()) {
            String path = "";
            try {
                path = RecordingStorage.stagingDirectory(getContext()).getAbsolutePath();
            } catch (IllegalStateException error) {
                emitState("error", error.getMessage());
                enabled = false;
            }
            directTelemetry.setRecording(enabled, path);
        }
        call.resolve(settings());
    }

    @PluginMethod public void chooseRecordingDirectory(PluginCall call) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION |
                Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        Uri selected = RecordingStorage.selectedDirectory(getContext());
        if (selected != null) intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, selected);
        startActivityForResult(call, intent, "recordingDirectoryResult");
    }

    @ActivityCallback private void recordingDirectoryResult(PluginCall call, ActivityResult result) {
        if (call == null) return;
        Intent data = result.getData();
        if (result.getResultCode() != Activity.RESULT_OK || data == null || data.getData() == null) {
            call.resolve(settings());
            return;
        }
        Uri directory = data.getData();
        int flags = data.getFlags() &
            (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            getContext().getContentResolver().takePersistableUriPermission(directory, flags);
            RecordingStorage.setSelectedDirectory(getContext(), directory);
            RecordingStorage.exportCompletedRecordingsAsync(getContext(), this::emitExportResult);
            call.resolve(settings());
        } catch (SecurityException error) {
            call.reject("Could not retain access to the selected folder", null, error);
        }
    }

    @PluginMethod public void useDefaultRecordingDirectory(PluginCall call) {
        RecordingStorage.clearSelectedDirectory(getContext());
        call.resolve(settings());
    }

    @PluginMethod public void startDiscovery(PluginCall call) {
        stopDiscoveryInternal();
        WifiManager wifi = (WifiManager) getContext().getSystemService(android.content.Context.WIFI_SERVICE);
        if (wifi != null) {
            multicastLock = wifi.createMulticastLock("track-n-race-capacitor-pairing");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        }
        String error = discovery.start();
        if (error != null) {
            stopDiscoveryInternal();
            call.reject(error);
            return;
        }
        call.resolve();
    }

    @PluginMethod public void stopDiscovery(PluginCall call) {
        stopDiscoveryInternal();
        call.resolve();
    }

    @PluginMethod public void scanPairingQr(PluginCall call) {
        IntentIntegrator scanner = new IntentIntegrator(getActivity());
        scanner.setCaptureActivity(PortraitCaptureActivity.class);
        scanner.setDesiredBarcodeFormats(Collections.singleton(IntentIntegrator.QR_CODE));
        scanner.setPrompt(getContext().getString(R.string.pairing_scan_prompt));
        scanner.setBeepEnabled(false);
        scanner.setOrientationLocked(true);
        startActivityForResult(call, scanner.createScanIntent(), "pairingQrResult");
    }

    @ActivityCallback private void pairingQrResult(PluginCall call, ActivityResult result) {
        if (call == null) return;
        IntentResult scan = IntentIntegrator.parseActivityResult(
            result.getResultCode(), result.getData());
        JSObject response = new JSObject();
        response.put("payload", scan == null || scan.getContents() == null ? "" : scan.getContents());
        call.resolve(response);
    }

    @PluginMethod public void pairQr(PluginCall call) {
        try {
            Uri uri = Uri.parse(call.getString("payload", ""));
            if (!"tnrpair".equals(uri.getScheme()) || !"v1".equals(uri.getHost())) {
                throw new IllegalArgumentException("Unsupported pairing QR");
            }
            String serverId = uri.getPathSegments().isEmpty() ? "" : uri.getPathSegments().get(0);
            String host = uri.getQueryParameter("h");
            String secret = uri.getQueryParameter("s");
            int port = Integer.parseInt(uri.getQueryParameter("p"));
            long expiry = Long.parseLong(uri.getQueryParameter("e"));
            if (host == null || secret == null || serverId.isEmpty() ||
                expiry < System.currentTimeMillis()) {
                throw new IllegalArgumentException("Pairing QR has expired");
            }
            prepareForPairing(call);
            pairedTelemetry.pair(new PairedTelemetryClient.Endpoint(
                serverId, "Track N Race desktop", host, port), secret, null);
        } catch (Exception error) {
            call.reject(error.getMessage(), null, error);
        }
    }

    @PluginMethod public void pairCode(PluginCall call) {
        String serverId = call.getString("serverId", "");
        String name = call.getString("name", "Track N Race desktop");
        String host = call.getString("host", "");
        int port = call.getInt("port", 20779);
        String code = call.getString("code", "").trim();
        if (serverId.isEmpty() || host.isEmpty() || code.isEmpty()) {
            call.reject("Select a desktop and enter its matching code");
            return;
        }
        prepareForPairing(call);
        pairedTelemetry.pair(new PairedTelemetryClient.Endpoint(serverId, name, host, port),
            null, code);
    }

    @PluginMethod public void forgetDesktop(PluginCall call) {
        pairedTelemetry.close();
        PairedTelemetryClient.forgetDesktop(getContext());
        if (sourceRequested) restartConfiguredSource();
        call.resolve(settings());
    }

    @Override public void onTelemetryRow(String json) {
        emitRow(json);
    }

    @Override public void onTelemetryBinary(byte[] bytes) {
        enqueueHot(bytes);
    }

    @Override public void onRow(String json) {
        emitRow(json);
    }

    @Override public void onBinary(byte[] bytes) {
        enqueueHot(bytes);
    }

    @Override public void onState(String state, @Nullable String detail) {
        if (("error".equals(state) || "disconnected".equals(state)) && pendingPairCall != null) {
            PluginCall call = pendingPairCall;
            pendingPairCall = null;
            mainHandler.post(() -> call.reject(detail == null ? "Pairing failed" : detail));
        }
        emitState(state, detail);
    }

    @Override public void onPaired() {
        sourceRequested = true;
        emitState("paired", PairedTelemetryClient.savedDesktopName(getContext()));
        PluginCall call = pendingPairCall;
        pendingPairCall = null;
        mainHandler.post(() -> {
            notifyListeners("settingsChanged", settings(), true);
            if (call != null) call.resolve();
        });
    }

    @Override public void onService(NativePairDiscovery.Service service) {
        JSObject value = new JSObject();
        value.put("serverId", service.serverId);
        value.put("name", service.name);
        value.put("host", service.address);
        value.put("port", service.port);
        value.put("pairing", service.pairing);
        mainHandler.post(() -> notifyListeners("discoveredDesktop", value));
    }

    @Override protected void handleOnStop() {
        stopDiscoveryInternal();
        suspendSourcesAsync();
    }

    @Override protected void handleOnStart() {
        if (sourceRequested) restartConfiguredSource();
    }

    @Override protected void handleOnDestroy() {
        sourceRequested = false;
        ++sourceGeneration;
        PluginCall call = pendingPairCall;
        pendingPairCall = null;
        if (call != null) call.reject("Android host closed during pairing");
        stopDiscoveryInternal();
        directTelemetry.stop();
        pairedTelemetry.close();
        sourceExecutor.shutdownNow();
    }

    private SharedPreferences preferences() {
        return RecordingStorage.preferences(getContext());
    }

    private boolean isDirectSource() {
        return PairedTelemetryClient.SOURCE_DIRECT.equals(preferences().getString(
            PairedTelemetryClient.PREF_SOURCE, PairedTelemetryClient.SOURCE_DIRECT));
    }

    private JSObject settings() {
        JSObject value = new JSObject();
        value.put("source", preferences().getString(PairedTelemetryClient.PREF_SOURCE,
            PairedTelemetryClient.SOURCE_DIRECT));
        value.put("recordingEnabled", preferences().getBoolean(
            RecordingStorage.PREF_RECORDING, false));
        value.put("hasSavedDesktop", PairedTelemetryClient.hasSavedDesktop(getContext()));
        value.put("desktopName", PairedTelemetryClient.savedDesktopName(getContext()));
        value.put("recordingDirectory", RecordingStorage.selectedDirectoryLabel(getContext()));
        value.put("usingCustomDirectory", RecordingStorage.selectedDirectory(getContext()) != null);
        return value;
    }

    private void restartConfiguredSource() {
        final int generation = ++sourceGeneration;
        sourceExecutor.execute(() -> {
            directTelemetry.stop();
            pairedTelemetry.close();
            if (!sourceRequested || generation != sourceGeneration) return;
            if (!isDirectSource() && PairedTelemetryClient.hasSavedDesktop(getContext())) {
                emitState("connecting", PairedTelemetryClient.savedDesktopName(getContext()));
                pairedTelemetry.connectSaved();
                return;
            }

            String recordingPath;
            try {
                recordingPath = RecordingStorage.stagingDirectory(getContext()).getAbsolutePath();
            } catch (IllegalStateException error) {
                recordingPath = "";
                emitState("error", error.getMessage());
            }
            String error = directTelemetry.start(UDP_PORT);
            if (error != null) {
                emitState("error", error);
                return;
            }
            boolean recording = preferences().getBoolean(RecordingStorage.PREF_RECORDING, false);
            directTelemetry.setRecording(recording && !recordingPath.isEmpty(), recordingPath);
            emitState("listening", "UDP " + UDP_PORT);
        });
    }

    private void prepareForPairing(PluginCall call) {
        if (pendingPairCall != null) pendingPairCall.reject("A newer pairing request replaced this one");
        pendingPairCall = call;
        ++sourceGeneration;
        sourceExecutor.execute(directTelemetry::stop);
        pairedTelemetry.close();
    }

    private void stopSourcesAsync() {
        ++sourceGeneration;
        sourceExecutor.execute(() -> {
            directTelemetry.stop();
            pairedTelemetry.close();
        });
    }

    private void suspendSourcesAsync() {
        ++sourceGeneration;
        sourceExecutor.execute(() -> {
            directTelemetry.stop();
            pairedTelemetry.close();
            RecordingStorage.exportCompletedRecordingsAsync(getContext(), null);
        });
    }

    private void stopDiscoveryInternal() {
        if (discovery != null) discovery.stop();
        if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
        multicastLock = null;
    }

    private void emitRow(String json) {
        if (json == null || json.isEmpty()) return;
        boolean postDrain = false;
        synchronized (coldLock) {
            pendingCold.append(json).append('\n');
            if (!coldDrainPosted) {
                coldDrainPosted = true;
                postDrain = true;
            }
        }
        // Immediate coalescing only: there is no pacing timer, rate cap, or row
        // replacement. Rows already adjacent on an engine/WebSocket callback
        // cross into the WebView in one ordered message instead of one
        // Capacitor PluginCall per row.
        if (postDrain) mainHandler.post(this::drainCold);
    }

    private void drainCold() {
        String batch;
        synchronized (coldLock) {
            batch = pendingCold.toString();
            pendingCold.setLength(0);
        }

        try {
            if (!batch.isEmpty()) {
                getBridge().getWebView().postWebMessage(
                    new android.webkit.WebMessage(batch), WEBVIEW_ORIGIN);
            }
        } finally {
            synchronized (coldLock) {
                if (pendingCold.length() > 0) {
                    mainHandler.post(this::drainCold);
                } else {
                    coldDrainPosted = false;
                }
            }
        }
    }

    private void emitState(String state, @Nullable String detail) {
        JSObject value = new JSObject();
        value.put("state", state);
        if (detail != null) value.put("detail", detail);
        mainHandler.post(() -> notifyListeners("sourceState", value, true));
    }

    private void emitExportResult(RecordingStorage.ExportResult result) {
        JSObject value = new JSObject();
        value.put("movedFiles", result.movedFiles);
        if (result.error != null) value.put("error", result.error);
        mainHandler.post(() -> notifyListeners("recordingExport", value));
    }

    private void enqueueHot(byte[] bytes) {
        if (bytes == null || bytes.length == 0) return;
        boolean postDrain = false;
        boolean reportOverflow = false;
        synchronized (hotLock) {
            if (pendingHotBytes + bytes.length > MAX_PENDING_HOT_BYTES) {
                reportOverflow = hotOverflowBytes == 0;
                hotOverflowBytes += bytes.length;
            } else {
                pendingHot.addLast(bytes);
                pendingHotBytes += bytes.length;
                if (!hotDrainPosted) {
                    hotDrainPosted = true;
                    postDrain = true;
                }
            }
        }
        if (postDrain) mainHandler.post(this::drainHot);
        if (reportOverflow) emitState("stream_overrun",
            "The UI transport could not accept " + hotOverflowBytes + " telemetry bytes");
    }

    private void drainHot() {
        byte[] bytes;
        long overflowBytes;
        synchronized (hotLock) {
            if (pendingHotBytes == 0) {
                hotDrainPosted = false;
                return;
            }
            overflowBytes = hotOverflowBytes;
            hotOverflowBytes = 0;
            ByteBuffer envelope = ByteBuffer.allocate(HOT_ENVELOPE_BYTES + pendingHotBytes)
                .order(ByteOrder.LITTLE_ENDIAN);
            envelope.put((byte) 'T').put((byte) 'N').put((byte) 'R').put((byte) 'B');
            envelope.putInt(++hotSequence);
            envelope.putInt((int) Math.min(Integer.MAX_VALUE, overflowBytes));
            while (!pendingHot.isEmpty()) envelope.put(pendingHot.removeFirst());
            pendingHotBytes = 0;
            bytes = envelope.array();
        }

        try {
            if (WebViewFeature.isFeatureSupported(WebViewFeature.WEB_MESSAGE_ARRAY_BUFFER)) {
                WebViewCompat.postWebMessage(getBridge().getWebView(),
                    new WebMessageCompat(bytes), WEBVIEW_ORIGIN);
            } else {
                JSObject fallback = new JSObject();
                fallback.put("base64", Base64.encodeToString(bytes, Base64.NO_WRAP));
                notifyListeners("telemetryBinaryFallback", fallback);
            }
        } finally {
            synchronized (hotLock) {
                if (pendingHotBytes > 0) {
                    mainHandler.post(this::drainHot);
                } else {
                    hotDrainPosted = false;
                }
            }
        }
    }
}
