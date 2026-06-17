#include "TrackMapWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QFile>
#include <QComboBox>
#include <QToolButton>
#include <QFontMetrics>
#include "../IconUtils.h"
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <cmath>
#include <algorithm>
#include <tuple>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Styling constants (ported 1:1 from the Electron TrackMap) ──────────────
namespace {
constexpr double TRACK_PX  = 5.0;
constexpr double DOT_R     = 7.0;
constexpr double MAP_PAD   = 24.0;
constexpr double DRS_PX    = 6.0;
constexpr double DRS_OFFSET= 18.0;
constexpr double SF_HALF   = 14.0;
constexpr double JUNC_HALF = 10.0;
const char* DRS_COLOR = "#39B54A";
constexpr int    LABEL_W   = 38;
constexpr int    LABEL_H   = 16;
constexpr int    LABEL_GAP = 5;
constexpr int    LABEL_R   = 3;
constexpr int    ACCENT_W  = 3;

const char* SECTOR_DARK[3]  = { "#E8002D", "#0090D0", "#FFD700" };
const char* SECTOR_LIGHT[3] = { "#D32F2F", "#0D47A1", "#B7950B" };

// Unit perpendicular to the polyline at index i (from neighbouring points).
QPointF perpAt(const QPointF* pts, int n, int i) {
    const int lo = std::max(0, i - 1);
    const int hi = std::min(n - 1, i + 1);
    const double dx = pts[hi].x() - pts[lo].x();
    const double dy = pts[hi].y() - pts[lo].y();
    double len = std::hypot(dx, dy);
    if (len == 0.0) len = 1.0;
    return QPointF(-dy / len, dx / len);
}

// 3-letter abbreviation: last name token, upper-cased, first 3 chars.
QString abbrev(const QString& name) {
    const QStringList parts = name.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return QString();
    return parts.last().left(3).toUpper();
}

class ClearableComboBox : public QComboBox {
public:
    explicit ClearableComboBox(QWidget* parent = nullptr) : QComboBox(parent) {
        clearBtn_ = new QToolButton(this);
        QIcon clearIcon = adaptThemeIcon(
            QIcon::fromTheme("edit-clear"),
            palette().color(QPalette::WindowText),
            style()->standardIcon(QStyle::SP_LineEditClearButton)
        );
        clearBtn_->setIcon(clearIcon);
        clearBtn_->setCursor(Qt::PointingHandCursor);
        clearBtn_->setStyleSheet(
            "QToolButton {"
            "  border: none; background: transparent; font-weight: bold; font-size: 16px;"
            "  color: #888; padding: 0px; margin: 0px;"
            "}"
            "QToolButton:hover { color: palette(text); }"
        );
        connect(clearBtn_, &QToolButton::clicked, this, [this]{
            setCurrentIndex(0);
        });
        clearBtn_->hide();
    }

    void setClearVisible(bool visible) {
        clearBtn_->setVisible(visible);
    }

protected:
    void resizeEvent(QResizeEvent* e) override {
        QComboBox::resizeEvent(e);
        int arrowWidth = 24;
        int btnSize = 18;
        clearBtn_->setGeometry(width() - arrowWidth - btnSize, (height() - btnSize) / 2, btnSize, btnSize);
    }

private:
    QToolButton* clearBtn_;
};

} // namespace

TrackMapWidget::TrackMapWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(200, 200);
    animTimer_ = new QTimer(this);
    animTimer_->setInterval(16);   // ~60fps
    connect(animTimer_, &QTimer::timeout, this, [this]{ update(); });
    snapTimer_.start();

    // Overlay controls (top-right): follow-driver + zoom selectors.
    zoomCombo_ = new QComboBox(this);
    zoomCombo_->addItem("2x", 2.0);
    zoomCombo_->addItem("4x", 4.0);
    zoomCombo_->addItem("8x", 8.0);
    zoomCombo_->addItem("16x", 16.0);
    zoomCombo_->setCurrentIndex(1);   // 4x
    zoomCombo_->hide();
    connect(zoomCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        zoomLevel_ = zoomCombo_->currentData().toDouble();
    });

    driverCombo_ = new ClearableComboBox(this);
    driverCombo_->addItem("Follow driver…", -1);
    
    connect(driverCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        selectedDriverIdx_ = driverCombo_->currentData().toInt();
        zoomCombo_->setVisible(selectedDriverIdx_ >= 0);
        static_cast<ClearableComboBox*>(driverCombo_)->setClearVisible(selectedDriverIdx_ >= 0);
        positionControls();
    });

    positionControls();
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

    rawDrs_.clear();
    if (j.contains("drs_zones")) {
        for (const auto& z : j["drs_zones"]) {
            if (!z.contains("track_points")) continue;
            std::vector<QPointF> pts;
            pts.reserve(z["track_points"].size());
            for (const auto& p : z["track_points"])
                if (p.is_array() && p.size() >= 2)
                    pts.emplace_back(p[0].get<double>(), p[1].get<double>());
            if (pts.size() >= 2) rawDrs_.push_back(std::move(pts));
        }
    }

    rawHasSF_ = false;
    if (j.contains("start_finish") && j["start_finish"].is_array() &&
        j["start_finish"].size() >= 2) {
        rawHasSF_ = true;
        rawSF_ = QPointF(j["start_finish"][0].get<double>(), j["start_finish"][1].get<double>());
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

    const auto rot = [&](const QPointF& p) {
        const double dx = p.x() - cx, dy = p.y() - cy;
        return QPointF(cos * dx - sin * dy + cx, sin * dx + cos * dy + cy);
    };

    // DRS zones (rotated; bounds intentionally exclude these, as in the reference).
    prep_.drsZones.clear();
    for (const auto& raw : rawDrs_) {
        std::vector<QPointF> z; z.reserve(raw.size());
        for (const QPointF& p : raw) z.push_back(rot(p));
        prep_.drsZones.push_back(std::move(z));
    }

    // Junctions: perpendicular tick at the end of each sector except the last
    // (= start of sectors 2 and 3).
    prep_.junctions.clear();
    for (size_t si = 0; si + 1 < prep_.sectors.size(); ++si) {
        const QPolygonF& s = prep_.sectors[si];
        if (s.size() < 2) continue;
        const int last = s.size() - 1;
        const QPointF n = perpAt(s.constData(), s.size(), last);
        prep_.junctions.push_back({ s[last], n.x(), n.y() });
    }

    // Start/finish: the start of sector 1, oriented by the sector-1 polyline.
    prep_.hasSF = false;
    if (rawHasSF_ && !prep_.sectors.empty() && prep_.sectors[0].size() > 0) {
        const QPointF sf = rot(rawSF_);
        const QPolygonF& s1 = prep_.sectors[0];
        int best = 0; double bestD = 1e18;
        for (int i = 0; i < s1.size(); ++i) {
            const double d = std::hypot(s1[i].x() - sf.x(), s1[i].y() - sf.y());
            if (d < bestD) { bestD = d; best = i; }
        }
        prep_.hasSF = true; prep_.sfPt = sf; prep_.sfIdx = best;
    }
}

TrackMapWidget::Layout TrackMapWidget::buildLayout(double cw, double ch) const {
    Layout l{1.0, 0.0, 0.0};
    if (prep_.w <= 0 || prep_.h <= 0) return l;
    l.scale = std::min((cw - 2 * MAP_PAD) / prep_.w, (ch - 2 * MAP_PAD) / prep_.h);
    l.ox = (cw - prep_.w * l.scale) / 2.0 - prep_.minX * l.scale;
    l.oy = (ch - prep_.h * l.scale) / 2.0 - prep_.minY * l.scale;
    return l;
}

QPointF TrackMapWidget::projectViewBox(double worldX, double worldZ) const {
    const double vx = (worldX - transform_.minX) * transform_.scale + transform_.offX;
    const double vy = (worldZ - transform_.minZ) * transform_.scale + transform_.offZ;
    const double dx = vx - prep_.rotCx, dy = vy - prep_.rotCy;
    return QPointF(prep_.rotCos * dx - prep_.rotSin * dy + prep_.rotCx,
                   prep_.rotSin * dx + prep_.rotCos * dy + prep_.rotCy);
}

QPointF TrackMapWidget::project(double worldX, double worldZ, const Layout& l) const {
    const QPointF r = projectViewBox(worldX, worldZ);
    return QPointF(r.x() * l.scale + l.ox, r.y() * l.scale + l.oy);
}

// ── Track vector render (used both for the cached layer and live zoom) ──────
//
// effZoom is layout.scale / baseLayout.scale. Track line widths grow with zoom
// (so cars don't float off a hairline) while staying readable, exactly as in
// the Electron map: trackZoomFactor = effZoom^0.8, other elements ~ sqrt(effZoom).
void TrackMapWidget::drawTrack(QPainter& p, const Layout& l, double effZoom) const {
    const double zf  = std::sqrt(effZoom);
    const double tzf = std::pow(effZoom, 0.8);
    const auto tc = [&](const QPointF& vb) {
        return QPointF(vb.x() * l.scale + l.ox, vb.y() * l.scale + l.oy);
    };
    const char** pal = dark_ ? SECTOR_DARK : SECTOR_LIGHT;

    for (size_t si = 0; si < prep_.sectors.size(); ++si) {
        const QPolygonF& poly = prep_.sectors[si];
        if (poly.size() < 2) continue;
        QPolygonF canvasPts;
        canvasPts.reserve(poly.size());
        for (const QPointF& vb : poly) canvasPts << tc(vb);

        QColor col(pal[std::min(si, size_t(2))]);
        QPen pen(col);
        pen.setWidthF(TRACK_PX * tzf);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPolyline(canvasPts);
    }

    // Sectors are already colour-coded, so boundary ticks use the contrasting
    // track colour (white on dark / black on light), matching the Electron map.
    const QColor lineCol(dark_ ? "#ffffff" : "#000000");

    // Sector-start ticks: junctions (S2, S3) …
    for (const Junction& jc : prep_.junctions) {
        const QPointF c = tc(jc.pt);
        const double half = std::max(JUNC_HALF * zf, (TRACK_PX * tzf) / 2 + 4 * zf);
        QPen pen(lineCol); pen.setWidthF(3.0 * zf); pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(c.x() - jc.nx * half, c.y() - jc.ny * half),
                   QPointF(c.x() + jc.nx * half, c.y() + jc.ny * half));
    }

    // DRS zones: dashed green line offset perpendicular-outward from the track.
    for (const std::vector<QPointF>& zone : prep_.drsZones) {
        if (zone.size() < 2) continue;
        QPolygonF poly;
        poly.reserve((int)zone.size());
        const double offset = std::max(DRS_OFFSET * zf, (TRACK_PX * tzf) / 2 + 8 * zf);
        for (int i = 0; i < (int)zone.size(); ++i) {
            const QPointF n = perpAt(zone.data(), (int)zone.size(), i);
            const QPointF c = tc(zone[i]);
            poly << QPointF(c.x() + n.x() * offset, c.y() + n.y() * offset);
        }
        QColor drsCol(DRS_COLOR);
        QPen pen(drsCol);
        pen.setWidthF(DRS_PX * zf);
        pen.setCapStyle(Qt::FlatCap);
        pen.setJoinStyle(Qt::MiterJoin);
        pen.setDashPattern({ 3.0 / DRS_PX, 3.0 / DRS_PX });  // 3px dash / 3px gap
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(poly);
    }

    // … and the start/finish line (start of sector 1).
    if (prep_.hasSF && !prep_.sectors.empty()) {
        const QPolygonF& s1 = prep_.sectors[0];
        const QPointF n = perpAt(s1.constData(), s1.size(), prep_.sfIdx);
        const QPointF c = tc(prep_.sfPt);
        const double half = std::max(SF_HALF * zf, (TRACK_PX * tzf) / 2 + 4 * zf);
        QPen pen(lineCol); pen.setWidthF(2.5 * zf); pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(c.x() - n.x() * half, c.y() - n.y() * half),
                   QPointF(c.x() + n.x() * half, c.y() + n.y() * half));
    }
}

// ── Static circuit layer (cached pixmap, base layout) ──────────────────────

void TrackMapWidget::rebuildStaticLayer() {
    if (!loaded_ || width() <= 0 || height() <= 0) return;

    const double dpr = devicePixelRatioF();
    staticLayer_ = QPixmap(QSize(int(width() * dpr), int(height() * dpr)));
    staticLayer_.setDevicePixelRatio(dpr);
    staticLayer_.fill(Qt::transparent);
    staticLayerSize_ = size();

    QPainter p(&staticLayer_);
    p.setRenderHint(QPainter::Antialiasing, true);
    drawTrack(p, buildLayout(width(), height()), 1.0);
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
    rebuildDriverCombo();
    update();
}

void TrackMapWidget::setDark(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    rebuildStaticLayer();
    update();
}

bool TrackMapWidget::interpCar(int idx, double t, double& outX, double& outZ) const {
    const Car* cur = nullptr;
    for (const Car& c : curSnap_.cars) if (c.idx == idx) { cur = &c; break; }
    if (!cur || (cur->x == 0.0 && cur->z == 0.0)) return false;
    outX = cur->x; outZ = cur->z;
    for (const Car& pv : prevSnap_.cars)
        if (pv.idx == idx) {
            if (!(pv.x == 0.0 && pv.z == 0.0)) {
                outX = pv.x + (cur->x - pv.x) * t;
                outZ = pv.z + (cur->z - pv.z) * t;
            }
            break;
        }
    return true;
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

    // Interpolation factor between prev and cur snapshot.
    double t = 1.0;
    if (!prevSnap_.cars.empty())
        t = std::clamp(snapTimer_.elapsed() / snapIntervalMs_, 0.0, 1.0);

    // ── Camera: follow selected driver with smooth zoom, else full map ────
    const Layout base = buildLayout(width(), height());
    Layout layout = base;
    double fx, fz;
    if (selectedDriverIdx_ >= 0 && interpCar(selectedDriverIdx_, t, fx, fz)) {
        const QPointF r = projectViewBox(fx, fz);
        const double followScale = base.scale * zoomLevel_;
        if (!hasCam_) { cam_ = { base.scale, 0.0, 0.0 }; hasCam_ = true; }
        cam_.scale += (followScale - cam_.scale) * 0.12;        // ease zoom
        cam_.ox = width()  / 2.0 - r.x() * cam_.scale;          // snap-pan to centre driver
        cam_.oy = height() / 2.0 - r.y() * cam_.scale;
        layout = cam_;
    } else if (selectedDriverIdx_ < 0 && hasCam_) {
        cam_.scale += (base.scale - cam_.scale) * 0.12;          // ease back to full map
        cam_.ox    += (base.ox    - cam_.ox)    * 0.12;
        cam_.oy    += (base.oy    - cam_.oy)    * 0.12;
        const double diff = std::abs(cam_.scale - base.scale)
                          + std::abs(cam_.ox - base.ox) + std::abs(cam_.oy - base.oy);
        if (diff < 0.5) hasCam_ = false;
        else            layout = cam_;
    }
    const double effZoom = base.scale > 0 ? layout.scale / base.scale : 1.0;

    // ── Track: cached pixmap when idle, live vector render when zoomed ───
    if (!hasCam_) {
        if (staticLayer_.isNull() || staticLayerSize_ != size())
            rebuildStaticLayer();
        p.drawPixmap(0, 0, staticLayer_);
    } else {
        drawTrack(p, layout, effZoom);
    }

    const Layout& l = layout;

    QFont labelFont("monospace", 9, QFont::Bold);
    labelFont.setStyleHint(QFont::Monospace);

    struct LabelJob { QPointF c; QString text; QColor color; };
    std::vector<LabelJob> labels;

    const auto findDriver = [&](int idx) -> const nlohmann::json* {
        if (!participants_.contains("drivers")) return nullptr;
        for (const auto& d : participants_["drivers"])
            if (d.value("idx", -1) == idx) return &d;
        return nullptr;
    };

    for (const Car& car : curSnap_.cars) {
        double cx, cz;
        if (!interpCar(car.idx, t, cx, cz)) continue;      // skips idle / (0,0)
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
    positionControls();
}

void TrackMapWidget::showEvent(QShowEvent*) {
    if (animTimer_) animTimer_->start();
    positionControls();
}

void TrackMapWidget::hideEvent(QHideEvent*) {
    if (animTimer_) animTimer_->stop();
}

// ── Follow-driver controls ──────────────────────────────────────────────────

void TrackMapWidget::positionControls() {
    if (!driverCombo_) return;
    const int margin = 8, gap = 6;
    int maxNameWidth = 0;
    QFontMetrics fm = driverCombo_->fontMetrics();
    for (int i = 0; i < driverCombo_->count(); ++i) {
        maxNameWidth = std::max(maxNameWidth, fm.horizontalAdvance(driverCombo_->itemText(i)));
    }
    
    // As requested: the widest name + 32px for the clear icon 
    // (+ 32px for the native dropdown arrow and borders)
    const int dw = std::max(120, maxNameWidth + 32 + 32);
    driverCombo_->setFixedWidth(dw);
    
    int currentX = width() - margin;
    
    currentX -= dw;
    driverCombo_->move(currentX, margin);
    driverCombo_->raise();

    if (zoomCombo_->isVisible()) {
        const int zw = std::max(58, zoomCombo_->sizeHint().width());
        zoomCombo_->setFixedWidth(zw);
        currentX -= gap + zw;
        zoomCombo_->move(currentX, margin);
        zoomCombo_->raise();
    }
}

void TrackMapWidget::rebuildDriverCombo() {
    if (!driverCombo_ || !participants_.contains("drivers")) return;

    QString sig;
    std::vector<std::tuple<int, int, QString>> drivers;  // raceNum, idx, label
    for (const auto& d : participants_["drivers"]) {
        const int idx = d.value("idx", -1);
        if (idx < 0) continue;
        const QString name = QString::fromStdString(d.value("name", "")).trimmed();
        const int raceNum = d.value("race_number", 0);
        if (name.isEmpty() && raceNum <= 0) continue;
        const QString last = name.isEmpty()
            ? QString("C%1").arg(idx)
            : name.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).last().toUpper();
        const QString label = QString("%1 %2").arg(raceNum).arg(last);
        drivers.emplace_back(raceNum, idx, label);
        sig += QString::number(idx) + label + ";";
    }
    if (sig == driverSig_) return;   // unchanged — keep current selection
    driverSig_ = sig;

    std::sort(drivers.begin(), drivers.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });

    const int prevSel = selectedDriverIdx_;
    QSignalBlocker block(driverCombo_);
    driverCombo_->clear();
    driverCombo_->addItem("Follow driver…", -1);
    for (const auto& dr : drivers)
        driverCombo_->addItem(std::get<2>(dr), std::get<1>(dr));

    int restore = 0;
    if (prevSel >= 0) {
        const int found = driverCombo_->findData(prevSel);
        if (found >= 0) restore = found;
        else            selectedDriverIdx_ = -1;
    }
    driverCombo_->setCurrentIndex(restore);
    zoomCombo_->setVisible(selectedDriverIdx_ >= 0);
    positionControls();
}
