package com.tracknrace.android;

import android.content.Context;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.annotation.StringRes;

import com.google.android.material.card.MaterialCardView;

final class PlaceholderPageView extends FrameLayout {
    PlaceholderPageView(Context context, @StringRes int description) {
        super(context);
        int margin = Ui.dimen(context, R.dimen.m2_screen_margin);
        setPadding(margin, margin, margin, margin);

        MaterialCardView card = Ui.card(context);
        LinearLayout content = Ui.vertical(context);
        content.setPadding(margin, margin, margin, margin);

        TextView title = Ui.text(context, context.getString(R.string.placeholder_title),
            R.style.TextAppearance_TrackNRace_Headline6);
        TextView body = Ui.text(context, context.getString(description),
            R.style.TextAppearance_TrackNRace_Body2);
        body.setPadding(0, Ui.dimen(context, R.dimen.m2_grid_1), 0, 0);
        content.addView(title);
        content.addView(body);
        card.addView(content);
        addView(card, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }
}
