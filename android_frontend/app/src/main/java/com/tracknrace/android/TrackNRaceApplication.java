package com.tracknrace.android;

import android.app.Application;

import com.google.android.material.color.DynamicColors;
import com.google.android.material.color.DynamicColorsOptions;

public final class TrackNRaceApplication extends Application {
    @Override public void onCreate() {
        super.onCreate();
        DynamicColorsOptions options = new DynamicColorsOptions.Builder()
            .setThemeOverlay(R.style.ThemeOverlay_TrackNRace_DynamicColors)
            .build();
        DynamicColors.applyToActivitiesIfAvailable(this, options);
    }
}
