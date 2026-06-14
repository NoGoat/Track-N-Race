#include "TrackMapWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QFile>
#include <QFontMetrics>
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QRegularExpression>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Styling constants (ported 1:1 from the Electron TrackMap) ──────────────
namespace {
constexpr double TRACK_PX  = 5.0;
constexpr double DOT_R     = 7.0;
constexpr double MAP_PAD   = 24.0;
constexpr int    LABEL_W   = 38;
constexpr int    LABEL_H   = 16;
constexpr int    LABEL_GAP = 5;
constexpr int    LABEL_R   = 3;
constexpr int    ACCENT_W  = 3;

const char* SECTOR_DARK[3]  = { "#E8002D", "#0090D0", "#FFD700" };
const char* SECTOR_LIGHT[3] = { "#D32F2F", "#0D47A1", "#B7950B" };

// 3-letter abbreviation: last name token, upper-cased, first 3 chars.
QString abbrev(const QString& name) {
    const QStringList parts = name.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return QString();
    return parts.last().left(3).toUpper();
}
} // namespace

TrackMapWidget::TrackMapWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setMinimumSize(200, 200);
    animTimer_ = new QTimer(this);
    animTimer_->setInterval(16);   // ~60fps
    connect(animTimer_, &QTimer::timeout, this, [this]{ update(); });
    snapTimer_.start();
}

// ── Track loading ──────────────────────────────────────────────────────────

bool TrackMapWidget::setTrack(int trackId) {
    if (trackId == trackId_ && loaded_) return true;

    QFile f(QString(":/maps/track_%1.json").arg(trackId));
    if (!f.open(QIODevice::ReadOnly)) {
        loaded_ = false; trackId_ = trackId;
        update();
        return false;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(bytes.constData(), bytes.constData() + bytes.size());
    } catch (...) {
        loaded_ = false; trackId_ = trackId;
        update();
        return false;
    }

    transform_.minX  = j["transform"].value("min_x", 0.0);
    transform_.minZ  = j["transform"].value("min_z", 0.0);
    transform_.scale = j["transform"].value("scale", 1.0);
    transform_.offX  = j["transform"].value("off_x", 0.0);
    transform_.offZ  = j["transform"].value("off_z", 0.0);
    viewBoxW_    = j["view_box"].value("width", 1000.0);
    viewBoxH_    = j["view_box"].value("height", 1000.0);
    rotationDeg_ = j.value("rotation_deg", 0.0);

    rawSectors_.clear();
    rawSectors_.resize(3);
    for (const auto& sector : j["sectors"]) {
        int idx = sector.value("index", (int)rawSectors_.size());  // 1-based
        std::vector<QPointF> pts;
        pts.reserve(sector["points"].size());
        for (const auto& p : sector["points"])
            if (p.is_array() && p.size() >= 2)
                pts.emplace_back(p[0].get<double>(), p[1].get<double>());
        if (idx >= 1 && idx <= 3) rawSectors_[idx - 1] = std::move(pts);
        else                      rawSectors_.push_back(std::move(pts));
    }

    trackId_ = trackId;
    loaded_  = true;
    rebuildPrepared();
    rebuildStaticLayer();
    update();
    return true;
}

void TrackMapWidget::rebuildPrepared() {
    const double rad = rotationDeg_ * M_PI / 180.0;
    const double cos = std::cos(rad), sin = std::sin(rad);
    const double cx = viewBoxW_ / 2.0, cy = viewBoxH_ / 2.0;
    prep_.rotCos = cos; prep_.rotSin = sin; prep_.rotCx = cx; prep_.rotCy = cy;

    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    prep_.sectors.clear();
    prep_.sectors.reserve(rawSectors_.size());
    for (const auto& raw : rawSectors_) {
        QPolygonF poly;
        poly.reserve((int)raw.size());
        for (const QPointF& p : raw) {
            const double dx = p.x() - cx, dy = p.y() - cy;
            const double rx = cos * dx - sin * dy + cx;
            const double ry = sin * dx + cos * dy + cy;
            poly << QPointF(rx, ry);
            minX = std::min(minX, rx); maxX = std::max(maxX, rx);
            minY = std::min(minY, ry); maxY = std::max(maxY, ry);
        }
        prep_.sectors.push_back(std::move(poly));
    }
    prep_.minX = minX; prep_.minY = minY;
    prep_.w = maxX - minX; prep_.h = maxY - minY;
}

TrackMapWidget::Layout TrackMapWidget::buildLayout(double cw, double ch) const {
    Layout l{1.0, 0.0, 0.0};
    if (prep_.w <= 0 || prep_.h <= 0) return l;
    l.scale = std::min((cw - 2 * MAP_PAD) / prep_.w, (ch - 2 * MAP_PAD) / prep_.h);
    l.ox = (cw - prep_.w * l.scale) / 2.0 - prep_.minX * l.scale;
    l.oy = (ch - prep_.h * l.scale) / 2.0 - prep_.minY * l.scale;
    return l;
}

QPointF TrackMapWidget::project(double worldX, double worldZ, const Layout& l) const {
    // world → viewBox
    double vx = (worldX - transform_.minX) * transform_.scale + transform_.offX;
    double vy = (worldZ - transform_.minZ) * transform_.scale + transform_.offZ;
    // rotate around viewBox centre
    const double dx = vx - prep_.rotCx, dy = vy - prep_.rotCy;
    const double rx = prep_.rotCos * dx - prep_.rotSin * dy + prep_.rotCx;
    const double ry = prep_.rotSin * dx + prep_.rotCos * dy + prep_.rotCy;
    // viewBox → canvas (logical px)
    return QPointF(rx * l.scale + l.ox, ry * l.scale + l.oy);
}

// ── Static circuit layer (cached pixmap) ───────────────────────────────────

void TrackMapWidget::rebuildStaticLayer() {
    if (!loaded_ || width() <= 0 || height() <= 0) return;

    const double dpr = devicePixelRatioF();
    staticLayer_ = QPixmap(QSize(int(width() * dpr), int(height() * dpr)));
    staticLayer_.setDevicePixelRatio(dpr);
    staticLayer_.fill(Qt::transparent);
    staticLayerSize_ = size();

    QPainter p(&staticLayer_);
    p.setRenderHint(QPainter::Antialiasing, true);

    const Layout l = buildLayout(width(), height());
    const char** pal = dark_ ? SECTOR_DARK : SECTOR_LIGHT;

    for (size_t si = 0; si < prep_.sectors.size(); ++si) {
        const QPolygonF& poly = prep_.sectors[si];
        if (poly.size() < 2) continue;
        QPolygonF canvasPts;
        canvasPts.reserve(poly.size());
        for (const QPointF& vb : poly)
            canvasPts << QPointF(vb.x() * l.scale + l.ox, vb.y() * l.scale + l.oy);

        QColor col(pal[std::min(si, size_t(2))]);
        QPen pen(col);
        pen.setWidthF(TRACK_PX);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPolyline(canvasPts);
    }
}

// ── Live data setters ───────────────────────────────────────────────────────

void TrackMapWidget::setPositions(const nlohmann::json& positions) {
    if (!positions.contains("cars")) return;

    Snapshot snap;
    for (const auto& c : positions["cars"]) {
        Car car;
        car.idx = c.value("idx", -1);
        car.x   = c.value("x", 0.0);
        car.z   = c.value("z", 0.0);
        if (car.idx >= 0) snap.cars.push_back(car);
    }

    // Measure the gap since the previous snapshot so we can interpolate.
    const qint64 dt = snapTimer_.restart();
    if (dt > 0) snapIntervalMs_ = std::clamp<double>((double)dt, 16.0, 500.0);

    prevSnap_ = std::move(curSnap_);
    curSnap_  = std::move(snap);
    update();
}

void TrackMapWidget::setParticipants(const nlohmann::json& participants) {
    participants_ = participants;
    update();
}

void TrackMapWidget::setDark(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    rebuildStaticLayer();
    update();
}

// ── Painting ────────────────────────────────────────────────────────────────

void TrackMapWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (!loaded_) {
        p.setPen(QColor(dark_ ? "#6e7177" : "#9aa0aa"));
        p.drawText(rect(), Qt::AlignCenter,
                   trackId_ < 0 ? "Waiting for session…" : "No map for this track");
        return;
    }

    // Static circuit layer (rebuild lazily if the size drifted).
    if (staticLayer_.isNull() || staticLayerSize_ != size())
        rebuildStaticLayer();
    p.drawPixmap(0, 0, staticLayer_);

    const Layout l = buildLayout(width(), height());

    // Interpolation factor between prev and cur snapshot.
    double t = 1.0;
    if (!prevSnap_.cars.empty())
        t = std::clamp(snapTimer_.elapsed() / snapIntervalMs_, 0.0, 1.0);

    QFont labelFont("monospace", 9, QFont::Bold);
    labelFont.setStyleHint(QFont::Monospace);

    struct LabelJob { QPointF c; QString text; QColor color; };
    std::vector<LabelJob> labels;

    const auto findPrev = [&](int idx) -> const Car* {
        for (const Car& c : prevSnap_.cars) if (c.idx == idx) return &c;
        return nullptr;
    };
    const auto findDriver = [&](int idx) -> const nlohmann::json* {
        if (!participants_.contains("drivers")) return nullptr;
        for (const auto& d : participants_["drivers"])
            if (d.value("idx", -1) == idx) return &d;
        return nullptr;
    };

    for (const Car& car : curSnap_.cars) {
        double cx = car.x, cz = car.z;
        if (cx == 0.0 && cz == 0.0) continue;              // idle / not on track
        if (const Car* pv = findPrev(car.idx)) {           // lerp from previous
            if (!(pv->x == 0.0 && pv->z == 0.0)) {
                cx = pv->x + (car.x - pv->x) * t;
                cz = pv->z + (car.z - pv->z) * t;
            }
        }
        const nlohmann::json* d = findDriver(car.idx);
        if (!d) continue;

        const QPointF pt = project(cx, cz, l);
        const QColor livery(QString::fromStdString(d->value("livery_color", "#8e8e8e")));

        // Dot
        p.setPen(Qt::NoPen);
        p.setBrush(livery);
        p.drawEllipse(pt, DOT_R, DOT_R);

        // Label text: driver abbreviation, else race number
        QString name = QString::fromStdString(d->value("name", ""));
        QString text = name.trimmed().isEmpty()
            ? QString::number(d->value("race_number", 0))
            : abbrev(name);
        labels.push_back({ pt, text, livery });
    }

    // Labels drawn after dots so they sit on top.
    p.setFont(labelFont);
    for (const LabelJob& job : labels) {
        const double bx = job.c.x() - LABEL_W / 2.0;
        const double by = job.c.y() - DOT_R - LABEL_GAP - LABEL_H;
        const QRectF box(bx, by, LABEL_W, LABEL_H);

        QPainterPath bg;
        bg.addRoundedRect(box, LABEL_R, LABEL_R);
        p.setPen(Qt::NoPen);
        p.setBrush(dark_ ? QColor(10, 15, 30, 235) : QColor(255, 255, 255, 245));
        p.drawPath(bg);
        if (!dark_) {
            p.setPen(QColor(0, 0, 0, 38));
            p.setBrush(Qt::NoBrush);
            p.drawPath(bg);
        }

        // Left livery accent bar
        QPainterPath accent;
        accent.addRoundedRect(QRectF(bx, by, ACCENT_W, LABEL_H), LABEL_R, LABEL_R);
        p.setPen(Qt::NoPen);
        p.setBrush(job.color);
        p.drawPath(accent);
        p.fillRect(QRectF(bx + ACCENT_W - LABEL_R, by, LABEL_R, LABEL_H), job.color);

        // Text
        p.setPen(dark_ ? QColor("#ffffff") : QColor("#111827"));
        p.drawText(box.adjusted(ACCENT_W, 0, 0, 0), Qt::AlignCenter, job.text);
    }
}

void TrackMapWidget::resizeEvent(QResizeEvent*) {
    if (loaded_) rebuildStaticLayer();
}

void TrackMapWidget::showEvent(QShowEvent*) {
    if (animTimer_) animTimer_->start();
}

void TrackMapWidget::hideEvent(QHideEvent*) {
    if (animTimer_) animTimer_->stop();
}
