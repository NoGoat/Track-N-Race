package com.tracknrace.android;

import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import com.google.android.material.appbar.AppBarLayout;
import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.bottomnavigation.BottomNavigationView;

public final class MainActivity extends AppCompatActivity implements NativeTelemetry.Listener {
    private static final int UDP_PORT = 20777;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final TimingStore store = new TimingStore();
    private NativeTelemetry telemetry;
    private MaterialToolbar toolbar;
    private FrameLayout pageHost;
    private BottomNavigationView navigation;
    private StandingsPageView standingsPage;
    private boolean receiving;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        WindowInsetsControllerCompat systemBars = WindowCompat.getInsetsController(
            getWindow(), getWindow().getDecorView());
        systemBars.setAppearanceLightStatusBars(false);
        systemBars.setAppearanceLightNavigationBars(false);
        getWindow().setStatusBarColor(getColor(R.color.tnr_surface));
        getWindow().setNavigationBarColor(getColor(R.color.tnr_surface));
        telemetry = new NativeTelemetry(this);
        setContentView(buildShell());
        navigation.setSelectedItemId(R.id.navigation_standings);
    }

    @Override protected void onStart() {
        super.onStart();
        setConnectionSubtitle("Starting UDP listener…");
        Thread starter = new Thread(() -> {
            String error = telemetry.start(UDP_PORT);
            mainHandler.post(() -> setConnectionSubtitle(error == null
                ? "Listening on UDP " + UDP_PORT + " • Auto-detect"
                : "UDP error • " + error));
        }, "tnrp-start");
        starter.start();
    }

    @Override protected void onStop() {
        telemetry.stop();
        receiving = false;
        super.onStop();
    }

    @Override public void onTelemetryRow(String json) {
        try {
            if (!store.accept(json)) return;
            mainHandler.post(() -> {
                standingsPage.setCars(store.snapshot());
                int format = store.activeFormat();
                if (format > 0) receiving = true;
                setConnectionSubtitle(receiving
                    ? "Receiving • F1 " + format + " • UDP " + UDP_PORT
                    : "Listening on UDP " + UDP_PORT + " • Auto-detect");
            });
        } catch (Exception ignored) {
            // Unknown or malformed rows must never stop the native receiver.
        }
    }

    private View buildShell() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(getColor(R.color.tnr_background));

        AppBarLayout appBar = new AppBarLayout(this);
        appBar.setBackgroundColor(getColor(R.color.tnr_surface));
        toolbar = new MaterialToolbar(this);
        toolbar.setTitleTextColor(getColor(R.color.tnr_on_surface));
        toolbar.setSubtitleTextColor(getColor(R.color.tnr_on_surface_muted));
        toolbar.setTitle("Standings");
        toolbar.setSubtitle("Starting UDP listener…");
        toolbar.setTitleTextAppearance(this, com.google.android.material.R.style.TextAppearance_MaterialComponents_Headline6);
        toolbar.setContentInsetsRelative(Ui.dp(this, 20), Ui.dp(this, 16));
        appBar.addView(toolbar, new AppBarLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(this, 72)));
        root.addView(appBar);

        pageHost = new FrameLayout(this);
        standingsPage = new StandingsPageView(this);
        root.addView(pageHost, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        navigation = new BottomNavigationView(this);
        navigation.inflateMenu(R.menu.main_navigation);
        navigation.setLabelVisibilityMode(BottomNavigationView.LABEL_VISIBILITY_LABELED);
        navigation.setBackgroundColor(getColor(R.color.tnr_surface));
        navigation.setItemIconTintList(Ui.navigationColors(this));
        navigation.setItemTextColor(Ui.navigationColors(this));
        navigation.setElevation(Ui.dp(this, 12));
        navigation.setOnItemSelectedListener(item -> {
            showDestination(item.getItemId());
            return true;
        });
        root.addView(navigation, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(this, 72)));
        applySystemBarInsets(root, appBar);
        return root;
    }

    private void applySystemBarInsets(View root, AppBarLayout appBar) {
        final int navigationHeight = Ui.dp(this, 72);
        ViewCompat.setOnApplyWindowInsetsListener(root, (view, windowInsets) -> {
            Insets bars = windowInsets.getInsets(
                WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout());

            // Draw the same surfaces behind Android's bars, while keeping all
            // interactive content below the status icons and above gestures.
            appBar.setPadding(bars.left, bars.top, bars.right, 0);
            pageHost.setPadding(bars.left, 0, bars.right, 0);
            navigation.setPadding(bars.left, 0, bars.right, bars.bottom);
            ViewGroup.LayoutParams params = navigation.getLayoutParams();
            params.height = navigationHeight + bars.bottom;
            navigation.setLayoutParams(params);
            return windowInsets;
        });
        ViewCompat.requestApplyInsets(root);
    }

    private void showDestination(int id) {
        if (id == R.id.navigation_standings) {
            showPage("Standings", standingsPage);
        } else if (id == R.id.navigation_overview) {
            showPage("Overview", new PlaceholderPageView(this, "Overview",
                "The mobile dashboard shell is ready for live speed, gear and race-state cards."));
        } else if (id == R.id.navigation_session) {
            showPage("Session", new PlaceholderPageView(this, "Session",
                "Weather, session clock, flags and proximity will live here."));
        } else if (id == R.id.navigation_strategy) {
            showPage("Strategy", new PlaceholderPageView(this, "Strategy",
                "Pit windows, tyre plans and rival comparisons will live here."));
        } else {
            showPage("More", new MorePageView(this, this::showSecondaryPage));
        }
    }

    private void showSecondaryPage(String title, String description) {
        showPage(title, new PlaceholderPageView(this, title, description));
    }

    private void showPage(String title, View content) {
        toolbar.setTitle(title);
        pageHost.removeAllViews();
        pageHost.addView(content, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));
    }

    private void setConnectionSubtitle(String value) {
        toolbar.setSubtitle(value);
    }
}
