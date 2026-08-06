package com.tracknrace.android;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import com.google.android.material.card.MaterialCardView;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.stream.Collectors;

final class MorePageView extends ScrollView {
    interface Navigator { void open(String title, String description); }

    MorePageView(Context context, Navigator navigator) {
        super(context);
        setFillViewport(true);
        LinearLayout list = Ui.vertical(context);
        list.setPadding(Ui.dp(context, 12), Ui.dp(context, 12), Ui.dp(context, 12), Ui.dp(context, 24));

        TextView dashboard = Ui.text(context, "DASHBOARD", 11, context.getColor(R.color.tnr_on_surface_muted));
        dashboard.setPadding(Ui.dp(context, 4), Ui.dp(context, 4), 0, Ui.dp(context, 8));
        list.addView(dashboard);
        addItem(list, "Analysis", "Compare laps and inspect traces", () -> navigator.open("Analysis", "Lap comparison and telemetry traces will live here."));
        addItem(list, "Tyres", "Wear, temperature and compound state", () -> navigator.open("Tyres", "Mobile tyre cards and stint history will live here."));
        addItem(list, "Input", "Throttle, brake and steering", () -> navigator.open("Input", "Pedal and steering telemetry will live here."));
        addItem(list, "Power", "ERS, fuel and engine systems", () -> navigator.open("Power", "Energy deployment, harvesting and fuel will live here."));
        addItem(list, "Misc", "Damage and remaining telemetry", () -> navigator.open("Misc", "Damage and secondary telemetry will live here."));

        TextView app = Ui.text(context, "APP", 11, context.getColor(R.color.tnr_on_surface_muted));
        app.setPadding(Ui.dp(context, 4), Ui.dp(context, 18), 0, Ui.dp(context, 8));
        list.addView(app);
        addItem(list, "Settings", "UDP, appearance and recording preferences", () -> navigator.open("Settings", "Mobile settings will live here."));
        addItem(list, "About & licences", "Track N Race and open-source notices", this::showLicences);
        addView(list);
    }

    private void addItem(LinearLayout parent, String title, String subtitle, Runnable action) {
        Context context = getContext();
        MaterialCardView card = Ui.card(context);
        card.setClickable(true);
        card.setFocusable(true);
        LinearLayout row = Ui.horizontal(context);
        row.setPadding(Ui.dp(context, 16), Ui.dp(context, 14), Ui.dp(context, 14), Ui.dp(context, 14));
        LinearLayout labels = Ui.vertical(context);
        labels.addView(Ui.title(context, title, 16));
        labels.addView(Ui.text(context, subtitle, 12, context.getColor(R.color.tnr_on_surface_muted)));
        row.addView(labels, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        TextView arrow = Ui.text(context, "›", 28, context.getColor(R.color.tnr_on_surface_muted));
        arrow.setGravity(Gravity.CENTER);
        row.addView(arrow);
        card.addView(row);
        card.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.setMargins(0, 0, 0, Ui.dp(context, 8));
        parent.addView(card, params);
    }

    private void showLicences() {
        String apache;
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
            getResources().openRawResource(R.raw.apache_2_0)))) {
            apache = reader.lines().collect(Collectors.joining("\n"));
        } catch (Exception error) {
            apache = "Apache License 2.0 — https://www.apache.org/licenses/LICENSE-2.0";
        }
        TextView body = Ui.text(getContext(),
            "Track N Race\nLicensed under GNU GPL v3\n\n" +
            "Material Components for Android 1.13.0\nApache License 2.0\n\n" + apache,
            13, getContext().getColor(R.color.tnr_on_surface));
        body.setPadding(Ui.dp(getContext(), 24), Ui.dp(getContext(), 8), Ui.dp(getContext(), 24), Ui.dp(getContext(), 8));
        ScrollView scroll = new ScrollView(getContext());
        scroll.addView(body, new ScrollView.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        new MaterialAlertDialogBuilder(getContext())
            .setTitle("About & licences")
            .setView(scroll)
            .setPositiveButton("Close", null)
            .show();
    }
}
