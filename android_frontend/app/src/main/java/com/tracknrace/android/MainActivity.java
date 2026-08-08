package com.tracknrace.android;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.AnimationUtils;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

import androidx.activity.OnBackPressedCallback;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;

import com.google.android.material.appbar.AppBarLayout;
import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.bottomnavigation.BottomNavigationView;
import com.google.android.material.color.MaterialColors;

import java.util.concurrent.atomic.AtomicBoolean;

public final class MainActivity extends AppCompatActivity implements NativeTelemetry.Listener {
    private static final int UDP_PORT = 20777;
    private static final long UI_REFRESH_INTERVAL_MS = 100L;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final TimingStore store = new TimingStore();
    private final AtomicBoolean refreshScheduled = new AtomicBoolean();
    private NativeTelemetry telemetry;
    private MaterialToolbar toolbar;
    private ConnectionStatusView connectionStatus;
    private FrameLayout pageHost;
    private BottomNavigationView navigation;
    private StandingsPageView standingsPage;
    private OnBackPressedCallback secondaryBackCallback;
    private boolean receiving;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        telemetry = new NativeTelemetry(this);
        setContentView(buildShell());

        secondaryBackCallback = new OnBackPressedCallback(false) {
            @Override public void handleOnBackPressed() {
                showMorePage();
            }
        };
        getOnBackPressedDispatcher().addCallback(this, secondaryBackCallback);
        navigation.setSelectedItemId(R.id.navigation_standings);
    }

    @Override protected void onStart() {
        super.onStart();
        setConnectionStatus(ConnectionStatusView.State.STARTING,
            getString(R.string.status_starting));
        new Thread(() -> {
            String error = telemetry.start(UDP_PORT);
            mainHandler.post(() -> setConnectionStatus(
                error == null ? ConnectionStatusView.State.LISTENING : ConnectionStatusView.State.ERROR,
                error == null
                    ? getString(R.string.status_listening, UDP_PORT)
                    : getString(R.string.status_error, error)));
        }, "tnrp-start").start();
    }

    @Override protected void onStop() {
        telemetry.stop();
        receiving = false;
        refreshScheduled.set(false);
        super.onStop();
    }

    @Override public void onTelemetryRow(String json) {
        try {
            if (store.accept(json)) scheduleUiRefresh();
        } catch (Exception ignored) {
            // Unknown or malformed rows must never stop the native receiver.
        }
    }

    private void scheduleUiRefresh() {
        if (!refreshScheduled.compareAndSet(false, true)) return;
        mainHandler.postDelayed(() -> {
            refreshScheduled.set(false);
            standingsPage.setCars(store.snapshot());
            int format = store.activeFormat();
            if (format > 0) receiving = true;
            setConnectionStatus(
                receiving ? ConnectionStatusView.State.RECEIVING : ConnectionStatusView.State.LISTENING,
                receiving
                    ? getString(R.string.status_receiving, format, UDP_PORT)
                    : getString(R.string.status_listening, UDP_PORT));
        }, UI_REFRESH_INTERVAL_MS);
    }

    private View buildShell() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(MaterialColors.getColor(
            root, android.R.attr.colorBackground));

        AppBarLayout appBar = new AppBarLayout(this);
        toolbar = new MaterialToolbar(this);
        toolbar.setTitle(R.string.navigation_standings);
        appBar.addView(toolbar, new AppBarLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, Ui.dimen(this, R.dimen.m2_toolbar_height)));
        root.addView(appBar);

        connectionStatus = new ConnectionStatusView(this);
        connectionStatus.setStatus(ConnectionStatusView.State.STARTING,
            getString(R.string.status_starting));
        root.addView(connectionStatus, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            Ui.dimen(this, R.dimen.m2_connection_status_height)));

        pageHost = new FrameLayout(this);
        standingsPage = new StandingsPageView(this);
        root.addView(pageHost, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        navigation = new BottomNavigationView(this);
        navigation.inflateMenu(R.menu.main_navigation);
        navigation.setLabelVisibilityMode(BottomNavigationView.LABEL_VISIBILITY_LABELED);
        navigation.setItemHorizontalTranslationEnabled(false);
        ViewCompat.setElevation(navigation,
            Ui.dimen(this, R.dimen.m2_bottom_navigation_elevation));
        navigation.setOnItemSelectedListener(item -> {
            showDestination(item.getItemId());
            return true;
        });
        root.addView(navigation, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            Ui.dimen(this, R.dimen.m2_bottom_navigation_height)));
        applySystemBarInsets(root, appBar);
        return root;
    }

    private void applySystemBarInsets(View root, AppBarLayout appBar) {
        final int navigationHeight = Ui.dimen(this, R.dimen.m2_bottom_navigation_height);
        final int horizontalMargin = Ui.dimen(this, R.dimen.m2_screen_margin);
        ViewCompat.setOnApplyWindowInsetsListener(root, (view, windowInsets) -> {
            Insets bars = windowInsets.getInsets(
                WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout());
            appBar.setPadding(bars.left, bars.top, bars.right, 0);
            connectionStatus.setPadding(bars.left + horizontalMargin, 0,
                bars.right + horizontalMargin, 0);
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
            showPrimaryPage(R.string.navigation_standings, standingsPage);
        } else if (id == R.id.navigation_overview) {
            showPrimaryPage(R.string.navigation_overview,
                new PlaceholderPageView(this, R.string.overview_placeholder));
        } else if (id == R.id.navigation_session) {
            showPrimaryPage(R.string.navigation_session,
                new PlaceholderPageView(this, R.string.session_placeholder));
        } else if (id == R.id.navigation_strategy) {
            showPrimaryPage(R.string.navigation_strategy,
                new PlaceholderPageView(this, R.string.strategy_placeholder));
        } else {
            showMorePage();
        }
    }

    private void showMorePage() {
        showPrimaryPage(R.string.navigation_more,
            new MorePageView(this, this::showSecondaryPage));
    }

    private void showSecondaryPage(int title, int description) {
        secondaryBackCallback.setEnabled(true);
        toolbar.setTitle(title);
        toolbar.setNavigationIcon(R.drawable.ic_arrow_back);
        toolbar.setNavigationContentDescription(R.string.navigation_up);
        toolbar.setNavigationOnClickListener(view -> showMorePage());
        replacePage(new PlaceholderPageView(this, description));
    }

    private void showPrimaryPage(int title, View content) {
        if (secondaryBackCallback != null) secondaryBackCallback.setEnabled(false);
        toolbar.setTitle(title);
        toolbar.setNavigationIcon(null);
        toolbar.setNavigationOnClickListener(null);
        replacePage(content);
    }

    private void replacePage(View content) {
        pageHost.removeAllViews();
        content.setAlpha(0f);
        pageHost.addView(content, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        content.animate()
            .alpha(1f)
            .setDuration(Ui.MOTION_DURATION_SHORT)
            .setInterpolator(AnimationUtils.loadInterpolator(
                this, android.R.interpolator.fast_out_slow_in))
            .start();
    }

    private void setConnectionStatus(ConnectionStatusView.State state, String text) {
        connectionStatus.setStatus(state, text);
    }
}
