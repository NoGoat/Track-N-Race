package com.tracknrace.android;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.google.android.material.card.MaterialCardView;

final class Ui {
    private Ui() {}

    static int dp(Context context, int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    static TextView text(Context context, String value, float size, int color) {
        TextView view = new TextView(context);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setFontFeatureSettings("tnum");
        return view;
    }

    static TextView title(Context context, String value, float size) {
        TextView view = text(context, value, size, context.getColor(R.color.tnr_on_surface));
        view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return view;
    }

    static MaterialCardView card(Context context) {
        MaterialCardView card = new MaterialCardView(context);
        card.setCardBackgroundColor(context.getColor(R.color.tnr_surface));
        card.setRadius(dp(context, 12));
        card.setCardElevation(dp(context, 1));
        card.setStrokeColor(context.getColor(R.color.tnr_divider));
        card.setStrokeWidth(dp(context, 1));
        return card;
    }

    static View divider(Context context) {
        View divider = new View(context);
        divider.setBackgroundColor(context.getColor(R.color.tnr_divider));
        divider.setLayoutParams(new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(context, 1)));
        return divider;
    }

    static ColorStateList navigationColors(Context context) {
        int[][] states = new int[][] {
            new int[] { android.R.attr.state_checked }, new int[] {}
        };
        return new ColorStateList(states, new int[] {
            context.getColor(R.color.tnr_secondary), context.getColor(R.color.tnr_on_surface_muted)
        });
    }

    static LinearLayout vertical(Context context) {
        LinearLayout layout = new LinearLayout(context);
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }

    static LinearLayout horizontal(Context context) {
        LinearLayout layout = new LinearLayout(context);
        layout.setOrientation(LinearLayout.HORIZONTAL);
        layout.setGravity(Gravity.CENTER_VERTICAL);
        return layout;
    }
}
