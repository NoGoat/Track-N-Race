package com.tracknrace.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.color.MaterialColors;
import com.google.android.material.snackbar.Snackbar;

import java.io.File;
import java.util.concurrent.atomic.AtomicBoolean;

/** Single-screen host for the steering-wheel dashboard. */
public final class MainActivity extends AppCompatActivity implements NativeTelemetry.Listener {
    private static final int UDP_PORT = 20777;
    private static final long FRAME_INTERVAL_MS = 33L;
    private static final String PREFS = "track_n_race_android";
    private static final String PREF_RECORDING = "recording_enabled";

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final DashboardTelemetryStore store = new DashboardTelemetryStore();
    private final AtomicBoolean frameScheduled = new AtomicBoolean();

    private NativeTelemetry telemetry;
    private WheelDashboardView dashboard;
    private MaterialButton recordButton;
    private SharedPreferences preferences;
    private volatile boolean engineRunning;
    private boolean recordingEnabled;
    private String lastRecordingError;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        preferences = getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        recordingEnabled = preferences.getBoolean(PREF_RECORDING, true);
        telemetry = new NativeTelemetry(this);
        setContentView(buildDashboard());
    }

    @Override protected void onStart() {
        super.onStart();
        dashboard.setHostState(WheelDashboardView.HostState.STARTING, UDP_PORT, 0);
        new Thread(() -> {
            String error = telemetry.start(UDP_PORT);
            if (error == null) {
                engineRunning = true;
                telemetry.setRecording(recordingEnabled, recordingDirectory().getAbsolutePath());
            }
            mainHandler.post(() -> {
                dashboard.setHostState(
                    error == null ? WheelDashboardView.HostState.LISTENING
                        : WheelDashboardView.HostState.ERROR,
                    UDP_PORT, store.snapshot().activeFormat);
                if (error != null) Snackbar.make(dashboard,
                    getString(R.string.status_error, error), Snackbar.LENGTH_INDEFINITE).show();
                syncRecordButton();
            });
        }, "tnrp-start").start();
    }

    @Override protected void onStop() {
        engineRunning = false;
        telemetry.stop();
        frameScheduled.set(false);
        super.onStop();
    }

    @Override public void onTelemetryRow(String json) {
        try {
            if (store.accept(json)) scheduleFrame();
        } catch (Exception ignored) {
            // A malformed or future row must never stop libtnrp's receiver thread.
        }
    }

    private View buildDashboard() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(getColor(R.color.dashboard_background));

        dashboard = new WheelDashboardView(this);
        root.addView(dashboard, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        recordButton = new MaterialButton(this, null,
            com.google.android.material.R.attr.materialButtonOutlinedStyle);
        recordButton.setAllCaps(true);
        recordButton.setCheckable(true);
        recordButton.setMinWidth(0);
        recordButton.setInsetTop(0);
        recordButton.setInsetBottom(0);
        recordButton.setTextSize(12f);
        recordButton.setOnClickListener(view -> setRecordingEnabled(!recordingEnabled));
        FrameLayout.LayoutParams recordParams = new FrameLayout.LayoutParams(
            Ui.dp(this, 82), Ui.dp(this, 40), Gravity.TOP | Gravity.END);
        recordParams.setMargins(0, Ui.dp(this, 12), Ui.dp(this, 14), 0);
        root.addView(recordButton, recordParams);

        ViewCompat.setOnApplyWindowInsetsListener(root, (view, windowInsets) -> {
            Insets bars = windowInsets.getInsets(
                WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout());
            dashboard.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) recordButton.getLayoutParams();
            params.topMargin = bars.top + Ui.dp(this, 12);
            params.rightMargin = bars.right + Ui.dp(this, 14);
            recordButton.setLayoutParams(params);
            return windowInsets;
        });
        ViewCompat.requestApplyInsets(root);
        syncRecordButton();
        return root;
    }

    private void scheduleFrame() {
        if (!frameScheduled.compareAndSet(false, true)) return;
        mainHandler.postDelayed(() -> {
            frameScheduled.set(false);
            DashboardTelemetryStore.Snapshot snapshot = store.snapshot();
            dashboard.setTelemetry(snapshot);
            dashboard.setHostState(
                snapshot.receiving ? WheelDashboardView.HostState.RECEIVING
                    : WheelDashboardView.HostState.LISTENING,
                UDP_PORT, snapshot.activeFormat);
            if (snapshot.recordingError != null &&
                !snapshot.recordingError.equals(lastRecordingError)) {
                lastRecordingError = snapshot.recordingError;
                Snackbar.make(dashboard, snapshot.recordingError, Snackbar.LENGTH_LONG).show();
            }
        }, FRAME_INTERVAL_MS);
    }

    private void setRecordingEnabled(boolean enabled) {
        recordingEnabled = enabled;
        preferences.edit().putBoolean(PREF_RECORDING, enabled).apply();
        if (engineRunning) {
            new Thread(() -> telemetry.setRecording(
                enabled, recordingDirectory().getAbsolutePath()), "tnrp-recording").start();
        }
        syncRecordButton();
        Snackbar.make(dashboard,
            enabled ? getString(R.string.recording_enabled,
                recordingDirectory().getAbsolutePath()) : getString(R.string.recording_disabled),
            Snackbar.LENGTH_LONG).show();
    }

    private File recordingDirectory() {
        File documents = getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS);
        if (documents == null) documents = getFilesDir();
        File directory = new File(documents, "Track N Race");
        if (!directory.exists()) directory.mkdirs();
        return directory;
    }

    private void syncRecordButton() {
        if (recordButton == null) return;
        recordButton.setChecked(recordingEnabled);
        recordButton.setText(recordingEnabled ? R.string.recording_on : R.string.recording_off);
        int color = getColor(recordingEnabled
            ? R.color.dashboard_recording : R.color.dashboard_text_secondary);
        recordButton.setTextColor(color);
        recordButton.setStrokeColor(ColorStateList.valueOf(color));
        recordButton.setRippleColor(ColorStateList.valueOf(
            MaterialColors.getColor(recordButton,
                android.R.attr.colorControlHighlight)));
    }
}
