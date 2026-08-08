package com.tracknrace.android;

import android.content.Context;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.annotation.ColorInt;
import androidx.annotation.DimenRes;
import androidx.annotation.StyleRes;
import androidx.core.widget.TextViewCompat;

import com.google.android.material.card.MaterialCardView;
import com.google.android.material.textview.MaterialTextView;

final class Ui {
    static final long MOTION_DURATION_SHORT = 150L;

    private Ui() {}

    static int dp(Context context, int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    static int dimen(Context context, @DimenRes int resource) {
        return context.getResources().getDimensionPixelSize(resource);
    }

    static TextView text(Context context, String value, @StyleRes int appearance) {
        MaterialTextView view = new MaterialTextView(context);
        view.setText(value);
        TextViewCompat.setTextAppearance(view, appearance);
        view.setFontFeatureSettings("tnum");
        return view;
    }

    static TextView coloredText(Context context, String value, @StyleRes int appearance,
                                @ColorInt int color) {
        TextView view = text(context, value, appearance);
        view.setTextColor(color);
        return view;
    }

    static MaterialCardView card(Context context) {
        return new MaterialCardView(context);
    }

    static View divider(Context context) {
        View divider = new View(context);
        divider.setBackgroundResource(R.color.m2_divider);
        divider.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        divider.setLayoutParams(new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dimen(context, R.dimen.m2_divider_height)));
        return divider;
    }

    static void applySelectableBackground(View view) {
        TypedValue value = new TypedValue();
        if (view.getContext().getTheme().resolveAttribute(
                android.R.attr.selectableItemBackground, value, true)) {
            view.setBackgroundResource(value.resourceId);
        }
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
