package com.tracknrace.android;

import android.content.Context;
import android.graphics.Color;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.core.view.ViewCompat;
import androidx.core.widget.NestedScrollView;

import com.google.android.material.card.MaterialCardView;
import com.google.android.material.chip.Chip;
import com.google.android.material.chip.ChipGroup;
import com.google.android.material.color.MaterialColors;

import java.util.Collections;
import java.util.List;
import java.util.Locale;

final class StandingsPageView extends NestedScrollView {
    private final LinearLayout content;
    private List<TimingStore.Car> cars = Collections.emptyList();
    private int selectedCar = -1;

    StandingsPageView(Context context) {
        super(context);
        setFillViewport(true);
        setClipToPadding(false);
        content = Ui.vertical(context);
        int margin = Ui.dimen(context, R.dimen.m2_screen_margin);
        content.setPadding(margin, margin, margin, Ui.dimen(context, R.dimen.m2_grid_3));
        addView(content, new NestedScrollView.LayoutParams(
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
            content.addView(emptyState());
            return;
        }

        TextView summary = Ui.text(getContext(),
            getContext().getString(R.string.standings_summary, cars.size()),
            R.style.TextAppearance_TrackNRace_Body2);
        summary.setPadding(0, 0, 0, Ui.dimen(getContext(), R.dimen.m2_grid_1));
        content.addView(summary);
        for (int index = 0; index < cars.size(); index++) {
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            if (index < cars.size() - 1) {
                params.bottomMargin = Ui.dimen(getContext(), R.dimen.m2_grid_1);
            }
            content.addView(driverCard(cars.get(index)), params);
        }
    }

    private View emptyState() {
        Context context = getContext();
        MaterialCardView card = Ui.card(context);
        LinearLayout body = Ui.vertical(context);
        int padding = Ui.dimen(context, R.dimen.m2_grid_2);
        body.setPadding(padding, padding, padding, padding);

        TextView title = Ui.text(context, context.getString(R.string.standings_empty_title),
            R.style.TextAppearance_TrackNRace_Headline6);
        TextView message = Ui.text(context, context.getString(R.string.standings_empty_body),
            R.style.TextAppearance_TrackNRace_Body2);
        message.setPadding(0, Ui.dimen(context, R.dimen.m2_grid_1), 0, 0);
        body.addView(title);
        body.addView(message);
        card.addView(body);
        return card;
    }

    private View driverCard(TimingStore.Car car) {
        Context context = getContext();
        MaterialCardView card = Ui.card(context);
        boolean selected = car.idx == selectedCar;
        card.setCheckable(true);
        card.setChecked(selected);
        card.setSelected(selected);
        ViewCompat.setStateDescription(card, context.getString(selected
            ? R.string.standings_selected : R.string.standings_not_selected));
        if (car.fastest) {
            card.setStrokeColor(context.getColor(R.color.racing_fastest_lap));
            card.setStrokeWidth(Ui.dp(context, 1));
        }
        card.setClickable(true);
        card.setFocusable(true);
        card.setOnClickListener(view -> {
            selectedCar = car.idx;
            render();
        });

        LinearLayout body = Ui.vertical(context);
        int padding = Ui.dimen(context, R.dimen.m2_grid_2);
        body.setPadding(padding, padding, padding, padding);
        body.addView(driverHeader(car));

        View divider = Ui.divider(context);
        LinearLayout.LayoutParams dividerParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, Ui.dimen(context, R.dimen.m2_divider_height));
        dividerParams.setMargins(0, Ui.dimen(context, R.dimen.m2_grid_2), 0,
            Ui.dimen(context, R.dimen.m2_grid_2));
        body.addView(divider, dividerParams);
        body.addView(metricsRows(car));

        ChipGroup statuses = statusChips(car);
        if (statuses.getChildCount() > 0) {
            LinearLayout.LayoutParams chipParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            chipParams.topMargin = Ui.dimen(context, R.dimen.m2_grid_1);
            body.addView(statuses, chipParams);
        }
        card.addView(body);
        return card;
    }

    private View driverHeader(TimingStore.Car car) {
        Context context = getContext();
        LinearLayout row = Ui.horizontal(context);

        TextView position = Ui.coloredText(context,
            context.getString(R.string.standings_position, car.position),
            R.style.TextAppearance_TrackNRace_Headline5, positionColor(car.position));
        row.addView(position, new LinearLayout.LayoutParams(
            Ui.dimen(context, R.dimen.m2_touch_target), ViewGroup.LayoutParams.WRAP_CONTENT));

        View teamBar = new View(context);
        teamBar.setBackgroundColor(safeColor(car.driver == null ? null : car.driver.color));
        LinearLayout.LayoutParams barParams = new LinearLayout.LayoutParams(
            Ui.dimen(context, R.dimen.team_indicator_width),
            Ui.dimen(context, R.dimen.m2_touch_target));
        barParams.setMarginEnd(Ui.dimen(context, R.dimen.m2_grid_1));
        row.addView(teamBar, barParams);

        LinearLayout identity = Ui.vertical(context);
        identity.addView(Ui.text(context, TimingStore.abbreviation(car.driver, car.idx),
            R.style.TextAppearance_TrackNRace_Subtitle1));
        String name = car.driver == null
            ? context.getString(R.string.standings_car_name, car.idx) : car.driver.name;
        if (car.player) name += context.getString(R.string.standings_player_suffix);
        identity.addView(Ui.text(context, name, R.style.TextAppearance_TrackNRace_Caption));
        row.addView(identity, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));

        LinearLayout gap = Ui.vertical(context);
        gap.setGravity(Gravity.END);
        TextView gapValue = Ui.text(context, resultOrGap(car),
            R.style.TextAppearance_TrackNRace_Subtitle2);
        gapValue.setGravity(Gravity.END);
        TextView gapLabel = Ui.text(context,
            context.getString(car.position == 1
                ? R.string.standings_race_lead : R.string.standings_to_leader),
            R.style.TextAppearance_TrackNRace_Overline);
        gapLabel.setGravity(Gravity.END);
        gap.addView(gapValue);
        gap.addView(gapLabel);
        row.addView(gap);
        return row;
    }

    private View metricsRows(TimingStore.Car car) {
        Context context = getContext();
        LinearLayout rows = Ui.vertical(context);
        LinearLayout primary = Ui.horizontal(context);
        primary.addView(metric(R.string.metric_lap, String.valueOf(car.lapNum)), weighted());
        primary.addView(metric(R.string.metric_last_lap, formatLap(car.lastLapMs)), weighted());
        primary.addView(metric(R.string.metric_tyre, compoundLabel(car.actualCompound)), weighted());
        rows.addView(primary);

        LinearLayout sectors = Ui.horizontal(context);
        LinearLayout.LayoutParams sectorParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        sectorParams.topMargin = Ui.dimen(context, R.dimen.m2_grid_1);
        sectors.addView(metric(R.string.metric_sector_1, formatSector(car.s1)), weighted());
        sectors.addView(metric(R.string.metric_sector_2, formatSector(car.s2)), weighted());
        sectors.addView(metric(R.string.metric_sector_3, formatSector(car.s3)), weighted());
        rows.addView(sectors, sectorParams);
        return rows;
    }

    private View metric(int label, String value) {
        Context context = getContext();
        LinearLayout metric = Ui.vertical(context);
        metric.addView(Ui.text(context, context.getString(label),
            R.style.TextAppearance_TrackNRace_Overline));
        TextView valueView = Ui.text(context, value, R.style.TextAppearance_TrackNRace_Body2);
        valueView.setPadding(0, Ui.dp(context, 2), 0, 0);
        metric.addView(valueView);
        return metric;
    }

    private ChipGroup statusChips(TimingStore.Car car) {
        Context context = getContext();
        ChipGroup group = new ChipGroup(context);
        group.setChipSpacingHorizontal(Ui.dimen(context, R.dimen.m2_grid_1));
        group.setChipSpacingVertical(Ui.dimen(context, R.dimen.m2_grid_1));
        int errorColor = MaterialColors.getColor(this, android.R.attr.colorError);
        if (car.pitStatus > 0) {
            group.addView(chip(car.pitStatus == 1 ? "PIT" : "PIT LANE",
                context.getColor(R.color.racing_pit)));
        }
        if (car.invalid) group.addView(chip("INVALID LAP", errorColor));
        if (car.penalties > 0) {
            group.addView(chip("+" + car.penalties + " SEC",
                context.getColor(R.color.racing_penalty)));
        }
        if (car.driveThroughs > 0) {
            group.addView(chip((car.driveThroughs > 1 ? car.driveThroughs + "× " : "")
                + "DRIVE THROUGH", errorColor));
        }
        if (car.stopGos > 0) {
            group.addView(chip((car.stopGos > 1 ? car.stopGos + "× " : "")
                + "STOP/GO", errorColor));
        }
        return group;
    }

    private Chip chip(String text, int color) {
        Chip chip = new Chip(getContext());
        chip.setText(text);
        chip.setTextColor(color);
        chip.setChipBackgroundColor(android.content.res.ColorStateList.valueOf(
            Color.argb(30, Color.red(color), Color.green(color), Color.blue(color))));
        chip.setChipStrokeColor(android.content.res.ColorStateList.valueOf(color));
        chip.setChipStrokeWidth(Ui.dp(getContext(), 1));
        chip.setClickable(false);
        return chip;
    }

    private LinearLayout.LayoutParams weighted() {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
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
        return String.format(Locale.ROOT, "%d:%02d.%03d",
            ms / 60_000, (ms % 60_000) / 1000, ms % 1000);
    }

    private static String formatSector(int ms) {
        if (ms <= 0) return "—";
        return String.format(Locale.ROOT, "%d.%03d", ms / 1000, ms % 1000);
    }

    private static String compoundLabel(int actual) {
        switch (actual) {
            case 16: return "C5 · SOFT";
            case 17: return "C4 · MED";
            case 18: return "C3 · HARD";
            case 19: return "C2";
            case 20: return "C1";
            case 7: return "INTER";
            case 8: return "WET";
            default: return "—";
        }
    }

    private int positionColor(int position) {
        if (position == 1) return getContext().getColor(R.color.racing_podium_gold);
        if (position == 2) return getContext().getColor(R.color.racing_podium_silver);
        if (position == 3) return getContext().getColor(R.color.racing_podium_bronze);
        return MaterialColors.getColor(this,
            com.google.android.material.R.attr.colorOnSurface);
    }

    private int safeColor(String value) {
        int fallback = MaterialColors.getColor(this,
            com.google.android.material.R.attr.colorOnSurface);
        try { return value == null ? fallback : Color.parseColor(value); }
        catch (IllegalArgumentException ignored) { return fallback; }
    }
}
