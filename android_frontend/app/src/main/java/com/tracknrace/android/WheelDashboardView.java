package com.tracknrace.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.View;

import androidx.annotation.NonNull;

import java.util.Locale;

/** A dense, glanceable display modeled after a modern formula steering wheel. */
final class WheelDashboardView extends View {
    enum HostState { STARTING, LISTENING, RECEIVING, ERROR }

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private final Typeface regular = Typeface.create("sans-serif", Typeface.NORMAL);
    private final Typeface medium = Typeface.create("sans-serif-medium", Typeface.NORMAL);
    private final Typeface digits = Typeface.create("sans-serif-condensed", Typeface.BOLD);
    private final int background;
    private final int panel;
    private final int panelBorder;
    private final int primary;
    private final int secondary;
    private final int accent;
    private final int green;
    private final int amber;
    private final int red;
    private final int purple;

    private DashboardTelemetryStore.Snapshot data = new DashboardTelemetryStore.Snapshot();
    private HostState hostState = HostState.STARTING;
    private int port = 20777;
    private int format;

    WheelDashboardView(Context context) {
        super(context);
        background = context.getColor(R.color.dashboard_background);
        panel = context.getColor(R.color.dashboard_panel);
        panelBorder = context.getColor(R.color.dashboard_panel_border);
        primary = context.getColor(R.color.dashboard_text_primary);
        secondary = context.getColor(R.color.dashboard_text_secondary);
        accent = context.getColor(R.color.dashboard_accent);
        green = context.getColor(R.color.dashboard_green);
        amber = context.getColor(R.color.dashboard_amber);
        red = context.getColor(R.color.dashboard_red);
        purple = context.getColor(R.color.dashboard_purple);
        setLayerType(View.LAYER_TYPE_HARDWARE, null);
        setContentDescription(context.getString(R.string.dashboard_accessibility));
    }

    void setTelemetry(DashboardTelemetryStore.Snapshot snapshot) {
        data = snapshot;
        format = snapshot.activeFormat;
        postInvalidateOnAnimation();
    }

    void setHostState(HostState state, int udpPort, int activeFormat) {
        hostState = state;
        port = udpPort;
        if (activeFormat > 0) format = activeFormat;
        postInvalidateOnAnimation();
    }

    @Override protected void onDraw(@NonNull Canvas canvas) {
        super.onDraw(canvas);
        canvas.drawColor(background);

        float left = getPaddingLeft() + dp(14);
        float top = getPaddingTop() + dp(12);
        float right = getWidth() - getPaddingRight() - dp(14);
        float bottom = getHeight() - getPaddingBottom() - dp(14);
        float width = Math.max(1, right - left);
        float height = Math.max(1, bottom - top);
        boolean landscape = width > height * 1.14f;

        drawConnection(canvas, left, top);
        drawShiftLights(canvas, left, top + dp(48), width);
        if (landscape) drawLandscape(canvas, left, top + dp(78), right, bottom);
        else drawPortrait(canvas, left, top + dp(78), right, bottom);
    }

    private void drawConnection(Canvas canvas, float x, float y) {
        int statusColor;
        String label;
        switch (hostState) {
            case RECEIVING:
                statusColor = green;
                label = format > 0 ? "LIVE  •  F1 " + format : "LIVE";
                break;
            case ERROR:
                statusColor = red;
                label = "UDP ERROR";
                break;
            case LISTENING:
                statusColor = amber;
                label = "UDP " + port + "  •  WAITING";
                break;
            default:
                statusColor = amber;
                label = "STARTING";
        }
        paint.setColor(statusColor);
        canvas.drawCircle(x + dp(5), y + dp(12), dp(4), paint);
        text(canvas, label, x + dp(17), y + dp(17), dp(12), primary,
            Paint.Align.LEFT, medium);
    }

    private void drawShiftLights(Canvas canvas, float left, float y, float width) {
        int count = 15;
        float gap = dp(5);
        float cell = Math.max(dp(8), (width - gap * (count - 1)) / count);
        float lit = clamp(data.rpm / 12_500f) * count;
        for (int i = 0; i < count; i++) {
            float x = left + i * (cell + gap);
            int liveColor = i < 5 ? green : (i < 10 ? red : purple);
            paint.setColor(i < lit ? liveColor : panelBorder);
            rect.set(x, y, x + cell, y + dp(11));
            canvas.drawRoundRect(rect, dp(3), dp(3), paint);
        }
    }

    private void drawLandscape(Canvas canvas, float left, float top, float right, float bottom) {
        float width = right - left;
        float gap = dp(10);
        float side = width * 0.255f;
        float centerLeft = left + side + gap;
        float centerRight = right - side - gap;

        float tileHeight = (bottom - top - gap * 2) / 3f;
        metricTile(canvas, left, top, left + side, top + tileHeight,
            "POSITION", data.position > 0 ? "P" + data.position : "—", accent);
        metricTile(canvas, left, top + tileHeight + gap, left + side,
            top + tileHeight * 2 + gap, "LAP", lapLabel(), primary);
        metricTile(canvas, left, top + tileHeight * 2 + gap * 2, left + side, bottom,
            data.lapInvalid ? "CURRENT • INVALID" : "CURRENT LAP",
            formatTime(data.currentLapMs), data.lapInvalid ? red : primary);

        drawCenter(canvas, centerLeft, top, centerRight, bottom);

        metricTile(canvas, right - side, top, right, top + tileHeight,
            "ERS", data.ersPercent + "%", data.ersPercent < 20 ? amber : green);
        metricTile(canvas, right - side, top + tileHeight + gap, right,
            top + tileHeight * 2 + gap, "FUEL", fuelLabel(), primary);
        metricTile(canvas, right - side, top + tileHeight * 2 + gap * 2, right, bottom,
            "LAST LAP", formatTime(data.lastLapMs), primary);
    }

    private void drawPortrait(Canvas canvas, float left, float top, float right, float bottom) {
        float width = right - left;
        float gap = dp(9);
        float centerBottom = top + (bottom - top) * .49f;
        drawCenter(canvas, left, top, right, centerBottom);

        float gridTop = centerBottom + gap;
        float tileWidth = (width - gap) / 2f;
        float tileHeight = (bottom - gridTop - gap * 2) / 3f;
        metricTile(canvas, left, gridTop, left + tileWidth, gridTop + tileHeight,
            "POSITION", data.position > 0 ? "P" + data.position : "—", accent);
        metricTile(canvas, left + tileWidth + gap, gridTop, right, gridTop + tileHeight,
            "LAP", lapLabel(), primary);
        metricTile(canvas, left, gridTop + tileHeight + gap, left + tileWidth,
            gridTop + tileHeight * 2 + gap, "ERS", data.ersPercent + "%",
            data.ersPercent < 20 ? amber : green);
        metricTile(canvas, left + tileWidth + gap, gridTop + tileHeight + gap, right,
            gridTop + tileHeight * 2 + gap, "FUEL", fuelLabel(), primary);
        metricTile(canvas, left, gridTop + tileHeight * 2 + gap * 2, left + tileWidth, bottom,
            data.lapInvalid ? "CURRENT • INVALID" : "CURRENT LAP",
            formatTime(data.currentLapMs), data.lapInvalid ? red : primary);
        metricTile(canvas, left + tileWidth + gap, gridTop + tileHeight * 2 + gap * 2,
            right, bottom, "LAST LAP", formatTime(data.lastLapMs), primary);
    }

    private void drawCenter(Canvas canvas, float left, float top, float right, float bottom) {
        panel(canvas, left, top, right, bottom);
        float width = right - left;
        float height = bottom - top;
        float cx = (left + right) / 2f;

        String aeroLabel = "slm".equals(data.aeroMode) ? "SLM" : "DRS";
        boolean aeroActive = "slm".equals(data.aeroMode) ? data.slm > 0 : data.drs > 0;
        pill(canvas, aeroLabel, left + dp(12), top + dp(12), aeroActive ? green : secondary);
        pill(canvas, tyreLabel(), right - dp(12), top + dp(12), tyreColor());

        float gearSize = Math.min(height * .47f, width * .31f);
        text(canvas, gearLabel(), cx, top + height * .48f, gearSize, primary,
            Paint.Align.CENTER, digits);
        text(canvas, Integer.toString(data.speedKph), cx, top + height * .72f,
            Math.min(height * .17f, width * .12f), primary, Paint.Align.CENTER, digits);
        text(canvas, "KM/H  •  " + data.rpm + " RPM", cx, top + height * .82f,
            Math.min(dp(13), height * .065f), secondary, Paint.Align.CENTER, medium);

        float barLeft = left + dp(14);
        float barRight = right - dp(14);
        float barY = bottom - dp(17);
        float half = (barRight - barLeft - dp(10)) / 2f;
        progress(canvas, barLeft, barY, barLeft + half, clamp(data.brake), red, "BRK");
        progress(canvas, barLeft + half + dp(10), barY, barRight,
            clamp(data.throttle), green, "THR");
    }

    private void metricTile(Canvas canvas, float left, float top, float right, float bottom,
                            String label, String value, int valueColor) {
        panel(canvas, left, top, right, bottom);
        text(canvas, label, left + dp(11), top + dp(19), dp(10), secondary,
            Paint.Align.LEFT, medium);
        float size = Math.min(dp(25), (bottom - top) * .34f);
        text(canvas, value, left + dp(11), bottom - dp(12), size, valueColor,
            Paint.Align.LEFT, digits);
    }

    private void panel(Canvas canvas, float left, float top, float right, float bottom) {
        rect.set(left, top, right, bottom);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(panel);
        canvas.drawRoundRect(rect, dp(7), dp(7), paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1));
        paint.setColor(panelBorder);
        canvas.drawRoundRect(rect, dp(7), dp(7), paint);
        paint.setStyle(Paint.Style.FILL);
    }

    private void pill(Canvas canvas, String label, float anchorX, float y, int color) {
        paint.setTypeface(medium);
        paint.setTextSize(dp(11));
        float textWidth = paint.measureText(label);
        boolean leftAligned = anchorX < getWidth() / 2f;
        float x1 = leftAligned ? anchorX : anchorX - textWidth - dp(18);
        rect.set(x1, y, x1 + textWidth + dp(18), y + dp(25));
        paint.setColor(Color.argb(42, Color.red(color), Color.green(color), Color.blue(color)));
        canvas.drawRoundRect(rect, dp(5), dp(5), paint);
        text(canvas, label, rect.centerX(), y + dp(17), dp(11), color,
            Paint.Align.CENTER, medium);
    }

    private void progress(Canvas canvas, float left, float centerY, float right,
                          float fraction, int color, String label) {
        text(canvas, label, left, centerY - dp(8), dp(9), secondary,
            Paint.Align.LEFT, medium);
        rect.set(left, centerY, right, centerY + dp(7));
        paint.setColor(panelBorder);
        canvas.drawRoundRect(rect, dp(3), dp(3), paint);
        rect.right = left + (right - left) * fraction;
        paint.setColor(color);
        canvas.drawRoundRect(rect, dp(3), dp(3), paint);
    }

    private void text(Canvas canvas, String value, float x, float baseline, float size,
                      int color, Paint.Align align, Typeface typeface) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(color);
        paint.setTextAlign(align);
        paint.setTypeface(typeface == null ? regular : typeface);
        paint.setTextSize(size);
        canvas.drawText(value, x, baseline, paint);
    }

    private String gearLabel() {
        if (data.gear < 0) return "R";
        if (data.gear == 0) return "N";
        return Integer.toString(data.gear);
    }

    private String lapLabel() {
        if (data.lapNumber <= 0) return "—";
        return data.totalLaps > 0
            ? data.lapNumber + " / " + data.totalLaps : Integer.toString(data.lapNumber);
    }

    private String fuelLabel() {
        return data.fuelLaps > 0
            ? String.format(Locale.ROOT, "%.1f LAPS", data.fuelLaps) : "—";
    }

    private static String formatTime(int milliseconds) {
        if (milliseconds <= 0) return "—";
        int minutes = milliseconds / 60_000;
        int seconds = (milliseconds / 1_000) % 60;
        int millis = milliseconds % 1_000;
        return String.format(Locale.ROOT, "%d:%02d.%03d", minutes, seconds, millis);
    }

    private String tyreLabel() {
        switch (data.tyreCompound) {
            case 16: return "SOFT " + data.tyreAgeLaps + "L";
            case 17: return "MED " + data.tyreAgeLaps + "L";
            case 18: return "HARD " + data.tyreAgeLaps + "L";
            case 7: return "INTER " + data.tyreAgeLaps + "L";
            case 8: return "WET " + data.tyreAgeLaps + "L";
            default: return data.tyreAgeLaps > 0 ? "TYRE " + data.tyreAgeLaps + "L" : "TYRE —";
        }
    }

    private int tyreColor() {
        switch (data.tyreCompound) {
            case 16: return red;
            case 17: return amber;
            case 18: return primary;
            case 7: return green;
            case 8: return accent;
            default: return secondary;
        }
    }

    private float dp(float value) {
        return value * getResources().getDisplayMetrics().density;
    }

    private static float clamp(float value) {
        return Math.max(0f, Math.min(1f, value));
    }
}
