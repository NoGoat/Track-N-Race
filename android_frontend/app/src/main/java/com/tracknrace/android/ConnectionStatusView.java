package com.tracknrace.android;

import android.content.Context;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.annotation.ColorRes;
import androidx.core.view.ViewCompat;

import com.google.android.material.color.MaterialColors;

final class ConnectionStatusView extends LinearLayout {
    enum State {
        STARTING(R.color.connection_starting),
        LISTENING(R.color.connection_listening),
        RECEIVING(R.color.connection_receiving),
        ERROR(R.color.connection_error);

        final int color;
        State(@ColorRes int color) { this.color = color; }
    }

    private final View indicator;
    private final TextView label;

    ConnectionStatusView(Context context) {
        super(context);
        setOrientation(HORIZONTAL);
        setGravity(Gravity.CENTER_VERTICAL);
        setMinimumHeight(Ui.dimen(context, R.dimen.m2_connection_status_height));
        setPadding(Ui.dimen(context, R.dimen.m2_screen_margin), 0,
            Ui.dimen(context, R.dimen.m2_screen_margin), 0);
        setBackgroundColor(MaterialColors.getColor(this,
            com.google.android.material.R.attr.colorSurface));
        ViewCompat.setElevation(this, Ui.dimen(context, R.dimen.m2_card_elevation));

        indicator = new View(context);
        int size = Ui.dimen(context, R.dimen.connection_indicator_size);
        LayoutParams indicatorParams = new LayoutParams(size, size);
        indicatorParams.setMarginEnd(Ui.dimen(context, R.dimen.m2_grid_1));
        addView(indicator, indicatorParams);

        label = Ui.text(context, "", R.style.TextAppearance_TrackNRace_Caption);
        label.setSingleLine(true);
        addView(label, new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
    }

    void setStatus(State state, String text) {
        GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(getContext().getColor(state.color));
        indicator.setBackground(dot);
        label.setText(text);
        setContentDescription(text);
    }
}
