package com.tracknrace.android;

import android.content.Context;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.google.android.material.card.MaterialCardView;

final class PlaceholderPageView extends FrameLayout {
    PlaceholderPageView(Context context, String title, String description) {
        super(context);
        setPadding(Ui.dp(context, 16), Ui.dp(context, 16), Ui.dp(context, 16), Ui.dp(context, 16));
        MaterialCardView card = Ui.card(context);
        LinearLayout body = Ui.vertical(context);
        body.setGravity(Gravity.CENTER);
        body.setPadding(Ui.dp(context, 28), Ui.dp(context, 40), Ui.dp(context, 28), Ui.dp(context, 40));
        TextView titleView = Ui.title(context, title, 22);
        titleView.setGravity(Gravity.CENTER);
        TextView descriptionView = Ui.text(context, description, 14, context.getColor(R.color.tnr_on_surface_muted));
        descriptionView.setGravity(Gravity.CENTER);
        descriptionView.setPadding(0, Ui.dp(context, 10), 0, 0);
        body.addView(titleView);
        body.addView(descriptionView);
        card.addView(body);
        addView(card, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP));
    }
}
