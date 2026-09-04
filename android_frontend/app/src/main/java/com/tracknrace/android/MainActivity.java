package com.tracknrace.android;

import android.os.Bundle;
import android.view.WindowManager;

import androidx.activity.OnBackPressedCallback;

import com.getcapacitor.BridgeActivity;

/** Capacitor host. All telemetry and platform work stays behind TelemetryPlugin. */
public final class MainActivity extends BridgeActivity {
    @Override protected void onCreate(Bundle state) {
        registerPlugin(TelemetryPlugin.class);
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getOnBackPressedDispatcher().addCallback(this, new OnBackPressedCallback(true) {
            @Override public void handleOnBackPressed() {
                if (getBridge() != null) {
                    getBridge().triggerWindowJSEvent("tnrBackButton");
                    return;
                }
                setEnabled(false);
                MainActivity.this.getOnBackPressedDispatcher().onBackPressed();
            }
        });
    }
}
