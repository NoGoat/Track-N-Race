package com.tracknrace.android;

import android.content.Context;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.annotation.StringRes;
import androidx.appcompat.widget.AppCompatImageView;
import androidx.core.widget.ImageViewCompat;

import com.google.android.material.color.MaterialColors;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.stream.Collectors;

final class MorePageView extends ScrollView {
    interface Navigator { void open(@StringRes int title, @StringRes int description); }

    MorePageView(Context context, Navigator navigator) {
        super(context);
        setFillViewport(true);
        LinearLayout list = Ui.vertical(context);
        list.setPadding(0, 0, 0, Ui.dimen(context, R.dimen.m2_grid_3));

        addSectionHeader(list, R.string.more_dashboard);
        addItem(list, R.string.analysis_title, R.string.analysis_summary,
            () -> navigator.open(R.string.analysis_title, R.string.analysis_placeholder), true);
        addItem(list, R.string.tyres_title, R.string.tyres_summary,
            () -> navigator.open(R.string.tyres_title, R.string.tyres_placeholder), true);
        addItem(list, R.string.input_title, R.string.input_summary,
            () -> navigator.open(R.string.input_title, R.string.input_placeholder), true);
        addItem(list, R.string.power_title, R.string.power_summary,
            () -> navigator.open(R.string.power_title, R.string.power_placeholder), true);
        addItem(list, R.string.misc_title, R.string.misc_summary,
            () -> navigator.open(R.string.misc_title, R.string.misc_placeholder), false);

        addSectionHeader(list, R.string.more_app);
        addItem(list, R.string.settings_title, R.string.settings_summary,
            () -> navigator.open(R.string.settings_title, R.string.settings_placeholder), true);
        addItem(list, R.string.about_title, R.string.about_summary, this::showLicences, false);
        addView(list, new ScrollView.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    private void addSectionHeader(LinearLayout parent, @StringRes int text) {
        Context context = getContext();
        TextView header = Ui.text(context, context.getString(text),
            R.style.TextAppearance_TrackNRace_Overline);
        header.setPadding(Ui.dimen(context, R.dimen.m2_screen_margin),
            Ui.dimen(context, R.dimen.m2_grid_3),
            Ui.dimen(context, R.dimen.m2_screen_margin),
            Ui.dimen(context, R.dimen.m2_grid_1));
        parent.addView(header);
    }

    private void addItem(LinearLayout parent, @StringRes int title, @StringRes int subtitle,
                         Runnable action, boolean dividerAfter) {
        Context context = getContext();
        LinearLayout row = Ui.horizontal(context);
        row.setMinimumHeight(Ui.dimen(context, R.dimen.m2_two_line_item_height));
        row.setPadding(Ui.dimen(context, R.dimen.m2_screen_margin),
            Ui.dimen(context, R.dimen.m2_grid_1),
            Ui.dimen(context, R.dimen.m2_grid_1),
            Ui.dimen(context, R.dimen.m2_grid_1));
        row.setClickable(true);
        row.setFocusable(true);
        Ui.applySelectableBackground(row);

        LinearLayout labels = Ui.vertical(context);
        labels.addView(Ui.text(context, context.getString(title),
            R.style.TextAppearance_TrackNRace_Subtitle1));
        TextView supporting = Ui.text(context, context.getString(subtitle),
            R.style.TextAppearance_TrackNRace_Body2);
        supporting.setMaxLines(1);
        labels.addView(supporting);
        row.addView(labels, new LinearLayout.LayoutParams(0,
            ViewGroup.LayoutParams.WRAP_CONTENT, 1));

        ImageView arrow = new AppCompatImageView(context);
        arrow.setImageResource(R.drawable.ic_chevron_right);
        ImageViewCompat.setImageTintList(arrow,
            android.content.res.ColorStateList.valueOf(MaterialColors.getColor(
                row, com.google.android.material.R.attr.colorOnSurface)));
        arrow.setAlpha(0.60f);
        arrow.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        int touchTarget = Ui.dimen(context, R.dimen.m2_touch_target);
        row.addView(arrow, new LinearLayout.LayoutParams(touchTarget, touchTarget));
        row.setOnClickListener(view -> action.run());
        parent.addView(row, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        if (dividerAfter) {
            View divider = Ui.divider(context);
            LinearLayout.LayoutParams dividerParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                Ui.dimen(context, R.dimen.m2_divider_height));
            dividerParams.setMarginStart(Ui.dimen(context, R.dimen.m2_screen_margin));
            parent.addView(divider, dividerParams);
        }
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
            R.style.TextAppearance_TrackNRace_Body2);
        int padding = Ui.dimen(getContext(), R.dimen.m2_grid_3);
        body.setPadding(padding, Ui.dimen(getContext(), R.dimen.m2_grid_1), padding,
            Ui.dimen(getContext(), R.dimen.m2_grid_1));
        ScrollView scroll = new ScrollView(getContext());
        scroll.addView(body, new ScrollView.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        new MaterialAlertDialogBuilder(getContext())
            .setTitle(R.string.about_title)
            .setView(scroll)
            .setPositiveButton(R.string.about_close, null)
            .show();
    }
}
