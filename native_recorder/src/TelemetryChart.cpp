#include "TelemetryChart.h"

#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QFontMetrics>
#include <cmath>

TelemetryChart::TelemetryChart(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(120);
}

void TelemetryChart::addPoint(float sessionTime, float speed, int rpm, float ers) {
    while (!pts.isEmpty() && (sessionTime - pts.first().t) > MAX_WINDOW_S)
        pts.removeFirst();

    pts.append({ sessionTime, speed, (float)rpm, ers });
    update();
}

void TelemetryChart::setWindowSeconds(float seconds) {
    windowS = seconds;
    update();
}

void TelemetryChart::reset() {
    pts.clear();
    update();
}

void TelemetryChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QPalette pal = palette();
    const QColor bgColor   = pal.color(QPalette::Window);
    const QColor gridColor = pal.color(QPalette::Mid);
    const QColor textColor = pal.color(QPalette::Text);

    // Margins for axes
    const float lm = 46.0f; // left  (RPM axis)
    const float rm = 46.0f; // right (Speed axis)
    const float tm = 8.0f;
    const float bm = 20.0f; // bottom (time labels)

    const float pw = width()  - lm - rm;
    const float ph = height() - tm - bm;
    const QRectF plot(lm, tm, pw, ph);

    // Background
    p.fillRect(rect(), bgColor);

    if (pw <= 0 || ph <= 0) return;

    // ── Grid (5 horizontal lines) ─────────────────────────────────
    p.setPen(QPen(gridColor, 1, Qt::DotLine));
    for (int i = 0; i <= 4; ++i) {
        float y = plot.top() + i * ph / 4.0f;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    // ── Y-axis labels ────────────────────────────────────────────
    QFont af; af.setPointSize(7); p.setFont(af);

    // Left axis: RPM (red), displayed as "Xk"
    const QColor rpmColor("#C4162A");
    p.setPen(rpmColor);
    for (int i = 0; i <= 4; ++i) {
        float norm = 1.0f - i / 4.0f;
        float val  = norm * MAX_RPM / 1000.0f;
        float y    = plot.top() + i * ph / 4.0f;
        QString lbl = QString::number(val, 'f', val < 1.0f ? 1 : 0) + "k";
        p.drawText(QRectF(0, y - 8, lm - 4, 16), Qt::AlignRight | Qt::AlignVCenter, lbl);
    }

    // Right axis: Speed (green) kph
    const QColor speedColor("#37872D");
    p.setPen(speedColor);
    for (int i = 0; i <= 4; ++i) {
        float norm = 1.0f - i / 4.0f;
        int   val  = (int)(norm * MAX_SPEED);
        float y    = plot.top() + i * ph / 4.0f;
        p.drawText(QRectF(plot.right() + 4, y - 8, rm - 4, 16),
                   Qt::AlignLeft | Qt::AlignVCenter, QString::number(val));
    }

    // ── X-axis time labels ───────────────────────────────────────
    if (!pts.isEmpty()) {
        float tMax = pts.last().t;
        float tMin = tMax - windowS;
        p.setPen(textColor);
        int steps = 6;
        for (int i = 0; i <= steps; ++i) {
            float t  = tMin + i * windowS / steps;
            float x  = plot.left() + (t - tMin) / windowS * pw;
            int secs = (int)std::fabs(t);
            QString lbl = QString("%1:%2")
                .arg(secs / 60, 2, 10, QChar('0'))
                .arg(secs % 60, 2, 10, QChar('0'));
            p.drawText(QRectF(x - 20, plot.bottom() + 2, 40, bm - 2),
                       Qt::AlignHCenter | Qt::AlignTop, lbl);
        }
    }

    if (pts.size() < 2) return;

    float tMax = pts.last().t;
    float tMin = tMax - windowS;

    auto xPx = [&](float t) { return plot.left() + ((t - tMin) / windowS) * pw; };

    // ── Series ───────────────────────────────────────────────────
    // ERS (yellow) — drawn first so it's behind speed/rpm
    const QColor ersColor("#FADE2A");
    {
        QPainterPath path;
        bool first = true;
        for (const auto& pt : pts) {
            float x = xPx(pt.t);
            float y = ny(pt.ers / 100.0f, plot.top(), ph);
            if (first) { path.moveTo(x, y); first = false; }
            else path.lineTo(x, y);
        }
        p.setPen(QPen(ersColor, 1.5f));
        p.drawPath(path);
    }

    // RPM (red)
    {
        QPainterPath path;
        bool first = true;
        for (const auto& pt : pts) {
            float x = xPx(pt.t);
            float y = ny(pt.rpm / MAX_RPM, plot.top(), ph);
            if (first) { path.moveTo(x, y); first = false; }
            else path.lineTo(x, y);
        }
        p.setPen(QPen(rpmColor, 1.5f));
        p.drawPath(path);
    }

    // Speed (green)
    {
        QPainterPath path;
        bool first = true;
        for (const auto& pt : pts) {
            float x = xPx(pt.t);
            float y = ny(pt.speed / MAX_SPEED, plot.top(), ph);
            if (first) { path.moveTo(x, y); first = false; }
            else path.lineTo(x, y);
        }
        p.setPen(QPen(speedColor, 1.5f));
        p.drawPath(path);
    }

    // ── Legend (top-right corner) ────────────────────────────────
    QFont lf; lf.setPointSize(7); p.setFont(lf);
    const float lx = plot.right() - 140;
    const float ly = plot.top() + 4;

    auto drawLegendItem = [&](float x, float y, const QColor& c, const QString& text) {
        p.fillRect(QRectF(x, y + 2, 10, 8), c);
        p.setPen(textColor);
        p.drawText(QRectF(x + 13, y, 60, 14), Qt::AlignLeft | Qt::AlignVCenter, text);
    };
    drawLegendItem(lx,       ly, speedColor, "Speed");
    drawLegendItem(lx + 55,  ly, rpmColor,   "RPM");
    drawLegendItem(lx + 100, ly, ersColor,   "ERS");
}
