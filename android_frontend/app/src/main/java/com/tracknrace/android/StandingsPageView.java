package com.tracknrace.android;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import com.google.android.material.card.MaterialCardView;
import com.google.android.material.chip.Chip;
import com.google.android.material.chip.ChipGroup;

import java.util.Collections;
import java.util.List;
import java.util.Locale;

final class StandingsPageView extends ScrollView {
    private final LinearLayout content;
    private List<TimingStore.Car> cars = Collections.emptyList();
    private int selectedCar = -1;

    StandingsPageView(Context context) {
        super(context);
        setFillViewport(true);
        setClipToPadding(false);
        content = Ui.vertical(context);
        content.setPadding(Ui.dp(context, 12), Ui.dp(context, 12), Ui.dp(context, 12), Ui.dp(context, 24));
        addView(content, new ScrollView.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        render();
    }

    void setCars(List<TimingStore.Car> value) {
        cars = value;
        render();
    }

    private void render() {
        content.removeAllViews();
        if (cars.isEmpty()) {
            content.addView(emptyState(), cardMargins(0, 8));
            return;
        }

        TextView count = Ui.text(getContext(), cars.size() + " cars • Tap a driver to focus", 12,
            getContext().getColor(R.color.tnr_on_surface_muted));
        count.setPadding(Ui.dp(getContext(), 4), 0, 0, Ui.dp(getContext(), 8));
        content.addView(count);
        for (TimingStore.Car car : cars) {
            content.addView(driverCard(car), cardMargins(0, 8));
        }
    }

    private View emptyState() {
        MaterialCardView card = Ui.card(getContext());
        LinearLayout body = Ui.vertical(getContext());
        body.setGravity(Gravity.CENTER);
        body.setPadding(Ui.dp(getContext(), 28), Ui.dp(getContext(), 42),
            Ui.dp(getContext(), 28), Ui.dp(getContext(), 42));
        TextView title = Ui.title(getContext(), "Waiting for live timing", 20);
        title.setGravity(Gravity.CENTER);
        TextView message = Ui.text(getContext(),
            "Set the F1 telemetry destination to this phone’s Wi-Fi address on UDP port 20777.",
            14, getContext().getColor(R.color.tnr_on_surface_muted));
        message.setGravity(Gravity.CENTER);
        message.setPadding(0, Ui.dp(getContext(), 10), 0, 0);
        body.addView(title);
        body.addView(message);
        card.addView(body);
        return card;
    }

    private View driverCard(TimingStore.Car car) {
        Context context = getContext();
        MaterialCardView card = Ui.card(context);
        boolean selected = car.idx == selectedCar;
        if (selected) {
            card.setStrokeColor(context.getColor(R.color.tnr_secondary));
            card.setStrokeWidth(Ui.dp(context, 2));
            card.setCardBackgroundColor(context.getColor(R.color.tnr_surface_high));
        } else if (car.fastest) {
            card.setStrokeColor(Color.rgb(191, 95, 255));
        }
        card.setClickable(true);
        card.setFocusable(true);
        card.setOnClickListener(v -> {
            selectedCar = car.idx;
            render();
        });

        LinearLayout body = Ui.vertical(context);
        body.setPadding(Ui.dp(context, 14), Ui.dp(context, 12), Ui.dp(context, 14), Ui.dp(context, 12));
        body.addView(driverHeader(car));
        body.addView(metricsRow(car), new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        ChipGroup statuses = statusChips(car);
        if (statuses.getChildCount() > 0) body.addView(statuses);
        card.addView(body);
        return card;
    }

    private View driverHeader(TimingStore.Car car) {
        Context context = getContext();
        LinearLayout row = Ui.horizontal(context);

        TextView position = Ui.title(context, "P" + car.position, 21);
        position.setTextColor(positionColor(car.position));
        row.addView(position, new LinearLayout.LayoutParams(Ui.dp(context, 48), ViewGroup.LayoutParams.WRAP_CONTENT));

        View teamBar = new View(context);
        teamBar.setBackgroundColor(safeColor(car.driver == null ? null : car.driver.color));
        LinearLayout.LayoutParams barParams = new LinearLayout.LayoutParams(Ui.dp(context, 4), Ui.dp(context, 36));
        barParams.setMargins(0, 0, Ui.dp(context, 10), 0);
        row.addView(teamBar, barParams);

        LinearLayout identity = Ui.vertical(context);
        TextView code = Ui.title(context, TimingStore.abbreviation(car.driver, car.idx), 17);
        String name = car.driver == null ? "Car " + car.idx : car.driver.name;
        if (car.player) name += "  •  YOU";
        TextView fullName = Ui.text(context, name, 12, context.getColor(R.color.tnr_on_surface_muted));
        identity.addView(code);
        identity.addView(fullName);
        row.addView(identity, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));

        LinearLayout gap = Ui.vertical(context);
        gap.setGravity(Gravity.END);
        TextView gapValue = Ui.title(context, resultOrGap(car), 16);
        gapValue.setGravity(Gravity.END);
        TextView gapLabel = Ui.text(context, car.position == 1 ? "RACE LEAD" : "TO LEADER", 10,
            context.getColor(R.color.tnr_on_surface_muted));
        gapLabel.setGravity(Gravity.END);
        gap.addView(gapValue);
        gap.addView(gapLabel);
        row.addView(gap);
        return row;
    }

    private View metricsRow(TimingStore.Car car) {
        Context context = getContext();
        LinearLayout outer = Ui.vertical(context);
        outer.setPadding(0, Ui.dp(context, 12), 0, 0);
        LinearLayout primary = Ui.horizontal(context);
        primary.addView(metric("LAP", String.valueOf(car.lapNum)), weighted());
        primary.addView(metric("LAST LAP", formatLap(car.lastLapMs)), weighted());
        primary.addView(metric("TYRE", compoundLabel(car.actualCompound)), weighted());
        outer.addView(primary);

        LinearLayout sectors = Ui.horizontal(context);
        sectors.setPadding(0, Ui.dp(context, 8), 0, 0);
        sectors.addView(metric("S1", formatSector(car.s1)), weighted());
        sectors.addView(metric("S2", formatSector(car.s2)), weighted());
        sectors.addView(metric("S3", formatSector(car.s3)), weighted());
        outer.addView(sectors);
        return outer;
    }

    private View metric(String label, String value) {
        Context context = getContext();
        LinearLayout metric = Ui.vertical(context);
        TextView labelView = Ui.text(context, label, 10, context.getColor(R.color.tnr_on_surface_muted));
        TextView valueView = Ui.title(context, value, 14);
        valueView.setPadding(0, Ui.dp(context, 2), 0, 0);
        metric.addView(labelView);
        metric.addView(valueView);
        return metric;
    }

    private ChipGroup statusChips(TimingStore.Car car) {
        Context context = getContext();
        ChipGroup group = new ChipGroup(context);
        group.setChipSpacingHorizontal(Ui.dp(context, 6));
        group.setPadding(0, Ui.dp(context, 8), 0, 0);
        if (car.pitStatus > 0) group.addView(chip(car.pitStatus == 1 ? "PIT" : "PIT LANE", Color.rgb(214, 171, 33)));
        if (car.invalid) group.addView(chip("INVALID LAP", context.getColor(R.color.tnr_primary)));
        if (car.penalties > 0) group.addView(chip("+" + car.penalties + " SEC", Color.rgb(196, 125, 14)));
        if (car.driveThroughs > 0) group.addView(chip((car.driveThroughs > 1 ? car.driveThroughs + "× " : "") + "DRIVE THROUGH", context.getColor(R.color.tnr_primary)));
        if (car.stopGos > 0) group.addView(chip((car.stopGos > 1 ? car.stopGos + "× " : "") + "STOP/GO", context.getColor(R.color.tnr_primary)));
        return group;
    }

    private Chip chip(String text, int color) {
        Chip chip = new Chip(getContext());
        chip.setText(text);
        chip.setTextSize(10);
        chip.setTextColor(color);
        chip.setChipBackgroundColor(android.content.res.ColorStateList.valueOf(
            Color.argb(30, Color.red(color), Color.green(color), Color.blue(color))));
        chip.setChipStrokeColor(android.content.res.ColorStateList.valueOf(color));
        chip.setChipStrokeWidth(Ui.dp(getContext(), 1));
        chip.setClickable(false);
        chip.setEnsureMinTouchTargetSize(false);
        return chip;
    }

    private LinearLayout.LayoutParams weighted() {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
    }

    private LinearLayout.LayoutParams cardMargins(int top, int bottom) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.setMargins(0, Ui.dp(getContext(), top), 0, Ui.dp(getContext(), bottom));
        return params;
    }

    private static String resultOrGap(TimingStore.Car car) {
        if (car.resultStatus == 4) return "DNF";
        if (car.resultStatus == 5) return "DSQ";
        if (car.resultStatus == 7) return "RET";
        if (car.position == 1) return "LEADER";
        if (car.gapMs <= 0) return "—";
        return String.format(Locale.ROOT, "+%.3f", car.gapMs / 1000.0);
    }

    private static String formatLap(int ms) {
        if (ms <= 0) return "--:--.---";
        return String.format(Locale.ROOT, "%d:%02d.%03d", ms / 60_000, (ms % 60_000) / 1000, ms % 1000);
    }

    private static String formatSector(int ms) {
        if (ms <= 0) return "—";
        return String.format(Locale.ROOT, "%d.%03d", ms / 1000, ms % 1000);
    }

    private static String compoundLabel(int actual) {
        switch (actual) {
            case 16: return "C5 • SOFT";
            case 17: return "C4 • MED";
            case 18: return "C3 • HARD";
            case 19: return "C2";
            case 20: return "C1";
            case 7: return "INTER";
            case 8: return "WET";
            default: return "—";
        }
    }

    private static int positionColor(int position) {
        if (position == 1) return Color.rgb(255, 215, 0);
        if (position == 2) return Color.rgb(192, 192, 192);
        if (position == 3) return Color.rgb(205, 127, 50);
        return Color.rgb(180, 187, 201);
    }

    private static int safeColor(String value) {
        try { return value == null ? Color.rgb(142, 142, 142) : Color.parseColor(value); }
        catch (IllegalArgumentException ignored) { return Color.rgb(142, 142, 142); }
    }
}
