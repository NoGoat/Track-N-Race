#include "TrackMapWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
const char* DRS_COLOR     = "#39B54A";
const char* SLM_DRY_COLOR = "#FF9500";   // SLM Normal grip  (slm_dry / Full status)   — orange
const char* SLM_WET_COLOR = "#22D3EE";   // SLM Reduced grip (slm_wet / Partial status) — cyan
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

// ── Overtaking-aid zone reconstruction (ported from the Electron TrackMap) ──
// DRS/SLM zones persist only their {start,end} endpoints; the polyline is the
// slice of the track centerline between them, following the driving direction
// and wrapping past the start/finish line when needed.

// Concatenate the sector polylines into one closed loop, dropping consecutive
// duplicate vertices (including the shared start/finish closing vertex).
std::vector<QPointF> buildCenterline(const std::vector<std::vector<QPointF>>& sectors) {
    std::vector<QPointF> pts;
    for (const auto& s : sectors)
        for (const QPointF& p : s)
            if (pts.empty() || pts.back().x() != p.x() || pts.back().y() != p.y())
                pts.push_back(p);
    if (pts.size() > 1 &&
        pts.front().x() == pts.back().x() && pts.front().y() == pts.back().y())
        pts.pop_back();
    return pts;
}

int nearestIdx(const std::vector<QPointF>& pts, const QPointF& p) {
    int best = 0; double bestD = 1e300;
    for (int i = 0; i < (int)pts.size(); ++i) {
        const double dx = pts[i].x() - p.x(), dy = pts[i].y() - p.y();
        const double d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// Centerline slice from nearest(start) to nearest(end), forward with wraparound.
std::vector<QPointF> sliceZone(const std::vector<QPointF>& cl,
                               const QPointF& start, const QPointF& end) {
    const int N = (int)cl.size();
    if (N == 0) return {};
    const int si = nearestIdx(cl, start), ei = nearestIdx(cl, end);
    std::vector<QPointF> out;
    int i = si;
    for (int n = 0; n < N; ++n) {
        out.push_back(cl[i]);
        if (i == ei) break;
        i = (i + 1) % N;
    }
    return out;
}

constexpr double SEAM_ANGLE_DEG   = 6.0;   // turn angle that flags the start/finish seam kink
constexpr int    SEAM_SMOOTH_SPAN = 6;     // neighbours each side of a flagged vertex to relax
constexpr int    SEAM_SMOOTH_PASSES = 20;

double turnAngleDeg(const QPointF& a, const QPointF& b, const QPointF& c) {
    const double d1x = b.x() - a.x(), d1y = b.y() - a.y();
    const double d2x = c.x() - b.x(), d2y = c.y() - b.y();
    double l1 = std::hypot(d1x, d1y); if (l1 == 0.0) l1 = 1.0;
    double l2 = std::hypot(d2x, d2y); if (l2 == 0.0) l2 = 1.0;
    double dot = (d1x * d2x + d1y * d2y) / (l1 * l2);
    dot = std::max(-1.0, std::min(1.0, dot));
    return std::acos(dot) * 180.0 / M_PI;
}

// Relax the isolated tangent kink where a zone crosses the start/finish seam.
// A cosine taper (full strength at the flagged spike vertex, fading to zero at
// the window edge) relaxes the kink while blending smoothly into the untouched
// track. Endpoints, and any vertex outside a flagged window, never move.
std::vector<QPointF> smoothSeam(const std::vector<QPointF>& pts) {
    const int n = (int)pts.size();
    if (n < 5) return pts;
    std::vector<double> weight(n, 0.0);
    bool any = false;
    for (int i = 1; i < n - 1; ++i) {
        if (turnAngleDeg(pts[i - 1], pts[i], pts[i + 1]) > SEAM_ANGLE_DEG) {
            for (int j = i - SEAM_SMOOTH_SPAN; j <= i + SEAM_SMOOTH_SPAN; ++j) {
                if (j > 0 && j < n - 1) {
                    const double taper = 0.5 * (1 + std::cos(
                        M_PI * std::abs(j - i) / (SEAM_SMOOTH_SPAN + 1)));
                    if (taper > weight[j]) { weight[j] = taper; any = true; }
                }
            }
        }
    }
    if (!any) return pts;
    std::vector<QPointF> out = pts;
    for (int pass = 0; pass < SEAM_SMOOTH_PASSES; ++pass) {
        std::vector<QPointF> next = out;
        for (int i = 1; i < n - 1; ++i) {
            const double w = weight[i];
            if (w > 0.0) {
                const double mx = (out[i - 1].x() + out[i + 1].x()) / 2.0;
                const double my = (out[i - 1].y() + out[i + 1].y()) / 2.0;
                next[i].setX(out[i].x() + w * (mx - out[i].x()));
                next[i].setY(out[i].y() + w * (my - out[i].y()));
            }
        }
        out = std::move(next);
    }
    return out;
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
            QIcon::fromTheme("window-close"),
            palette().color(QPalette::WindowText),
            style()->standardIcon(QStyle::SP_DialogCloseButton)
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
    idleClock_.start();

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

    // Fullscreen toggle (rightmost overlay control). The host maximises/restores
    // the map; we just relay the click and reflect the state on the icon. Left
    // unstyled so it takes the native OS/Breeze tool-button appearance — only the
    // icon is set.
    fsButton_ = new QToolButton(this);
    connect(fsButton_, &QToolButton::clicked, this, [this]{ emit fullscreenToggled(); });
    setFullscreenState(false);   // initial (maximise) icon

    positionControls();
}

void TrackMapWidget::setFullscreenState(bool on) {
    isFullscreen_ = on;
    if (!fsButton_) return;
    const QColor tint = palette().color(QPalette::WindowText);
    // Prefer the bundled Breeze glyphs, falling back to the style's title-bar icons.
    fsButton_->setIcon(on
        ? adaptThemeIcon(QIcon::fromTheme("window-restore-symbolic"),
                         tint, style()->standardIcon(QStyle::SP_TitleBarNormalButton))
        : adaptThemeIcon(QIcon::fromTheme("window-maximize-symbolic"),
                         tint, style()->standardIcon(QStyle::SP_TitleBarMaxButton)));
    fsButton_->setText(on ? "Restore Map" : "Enlarge Map");
    fsButton_->setToolTip(on ? "Restore Map" : "Enlarge Map");
    positionControls();   // the label change alters the button width
}

void TrackMapWidget::setShowLabels(bool on) {
    if (!fsButton_) return;
    fsButton_->setToolButtonStyle(on ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    positionControls();   // icon-only vs text-beside-icon changes the button width
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
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();

    if (!doc.isObject()) {
        loaded_ = false; trackId_ = trackId;
        update();
        return false;
    }
    const QJsonObject j = doc.object();

    const QJsonObject tf = j.value("transform").toObject();
    transform_.minX  = tf.value("min_x").toDouble(0.0);
    transform_.minZ  = tf.value("min_z").toDouble(0.0);
    transform_.scale = tf.value("scale").toDouble(1.0);
    transform_.offX  = tf.value("off_x").toDouble(0.0);
    transform_.offZ  = tf.value("off_z").toDouble(0.0);
    const QJsonObject vb = j.value("view_box").toObject();
    viewBoxW_    = vb.value("width").toDouble(1000.0);
    viewBoxH_    = vb.value("height").toDouble(1000.0);
    rotationDeg_ = j.value("rotation_deg").toDouble(0.0);

    rawSectors_.clear();
    rawSectors_.resize(3);
    for (const QJsonValue& sv : j.value("sectors").toArray()) {
        const QJsonObject sector = sv.toObject();
        int idx = sector.value("index").toInt((int)rawSectors_.size());  // 1-based
        const QJsonArray points = sector.value("points").toArray();
        std::vector<QPointF> pts;
        pts.reserve((size_t)points.size());
        for (const QJsonValue& pv : points) {
            const QJsonArray p = pv.toArray();
            if (p.size() >= 2)
                pts.emplace_back(p[0].toDouble(), p[1].toDouble());
        }
        if (idx >= 1 && idx <= 3) rawSectors_[idx - 1] = std::move(pts);
        else                      rawSectors_.push_back(std::move(pts));
    }

    // Overtaking-aid zones store only {start,end}; the polyline is re-derived from
    // the centerline in rebuildPrepared(). DRS (F1 24/25) plus the 2026 SLM dry-
    // and wet-weather zone sets.
    const auto parseZones = [](const QJsonArray& arr) {
        std::vector<std::pair<QPointF, QPointF>> zones;
        for (const QJsonValue& zv : arr) {
            const QJsonObject z = zv.toObject();
            const QJsonArray start = z.value("start").toArray();
            const QJsonArray end   = z.value("end").toArray();
            if (start.size() >= 2 && end.size() >= 2) {
                zones.emplace_back(
                    QPointF(start[0].toDouble(), start[1].toDouble()),
                    QPointF(end[0].toDouble(),   end[1].toDouble()));
            }
        }
        return zones;
    };
    rawDrs_    = parseZones(j.value("drs_zones").toArray());
    rawSlmDry_ = parseZones(j.value("slm_dry").toArray());
    rawSlmWet_ = parseZones(j.value("slm_wet").toArray());

    rawHasSF_ = false;
    {
        const QJsonArray sf = j.value("start_finish").toArray();
        if (sf.size() >= 2) {
            rawHasSF_ = true;
            rawSF_ = QPointF(sf[0].toDouble(), sf[1].toDouble());
        }
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

    // Overtaking-aid overlays: re-derive each zone's polyline as the centerline
    // slice between its {start,end}, relax the start/finish seam, then rotate.
    // Bounds intentionally exclude these, as in the reference.
    const std::vector<QPointF> centerline = buildCenterline(rawSectors_);
    const auto reconstruct = [&](const std::vector<std::pair<QPointF, QPointF>>& zones) {
        std::vector<std::vector<QPointF>> out;
        out.reserve(zones.size());
        for (const auto& z : zones) {
            std::vector<QPointF> slice = smoothSeam(sliceZone(centerline, z.first, z.second));
            std::vector<QPointF> rotated; rotated.reserve(slice.size());
            for (const QPointF& p : slice) rotated.push_back(rot(p));
            if (rotated.size() >= 2) out.push_back(std::move(rotated));
        }
        return out;
    };
    prep_.drsZones = reconstruct(rawDrs_);
    prep_.slmDry   = reconstruct(rawSlmDry_);
    prep_.slmWet   = reconstruct(rawSlmWet_);

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
    // When sector colors are off, the track is drawn as plain lines using the
    // palette text colour (white on dark themes / black on light), keeping it in
    // step with the active palette rather than hard-coded white/black.
    const QColor lineCol = palette().color(QPalette::Text);

    for (size_t si = 0; si < prep_.sectors.size(); ++si) {
        const QPolygonF& poly = prep_.sectors[si];
        if (poly.size() < 2) continue;
        QPolygonF canvasPts;
        canvasPts.reserve(poly.size());
        for (const QPointF& vb : poly) canvasPts << tc(vb);

        QColor col = sectorColors_ ? QColor(pal[std::min(si, size_t(2))]) : lineCol;
        QPen pen(col);
        pen.setWidthF(TRACK_PX * tzf);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPolyline(canvasPts);
    }

    // Sector-start ticks: junctions (S2, S3). When sectors are colour-coded the
    // boundary ticks use the contrasting track colour; when sectors are plain the
    // ticks carry the sector colour instead (Electron inverts these).
    for (size_t ji = 0; ji < prep_.junctions.size(); ++ji) {
        const Junction& jc = prep_.junctions[ji];
        const QPointF c = tc(jc.pt);
        const double half = std::max(JUNC_HALF * zf, (TRACK_PX * tzf) / 2 + 4 * zf);
        const QColor jcol = sectorColors_ ? lineCol : QColor(pal[std::min(ji + 1, size_t(2))]);
        QPen pen(jcol); pen.setWidthF(3.0 * zf); pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(c.x() - jc.nx * half, c.y() - jc.ny * half),
                   QPointF(c.x() + jc.nx * half, c.y() + jc.ny * half));
    }

    // Overtaking-aid overlay: a dashed line running alongside the track, offset
    // perpendicular toward the *outside* of the circuit (the inward normal points
    // at the enclosed interior for a consistently-wound loop, so we negate it).
    // DRS zones (F1 24/25, green) or the 2026 SLM overlay: slm_wet on a Partial
    // track status (cyan), otherwise slm_dry (orange).
    const auto drawOffsetZones = [&](const std::vector<std::vector<QPointF>>& zones,
                                     const QColor& color) {
        const double offset = std::max(DRS_OFFSET * zf, (TRACK_PX * tzf) / 2 + 8 * zf);
        QPen pen(color);
        pen.setWidthF(DRS_PX * zf);
        pen.setCapStyle(Qt::FlatCap);
        pen.setJoinStyle(Qt::MiterJoin);
        pen.setDashPattern({ 3.0 / DRS_PX, 3.0 / DRS_PX });  // 3px dash / 3px gap
        for (const std::vector<QPointF>& zone : zones) {
            if (zone.size() < 2) continue;
            QPolygonF poly;
            poly.reserve((int)zone.size());
            for (int i = 0; i < (int)zone.size(); ++i) {
                const QPointF n = perpAt(zone.data(), (int)zone.size(), i);
                const QPointF c = tc(zone[i]);
                poly << QPointF(c.x() - n.x() * offset, c.y() - n.y() * offset);
            }
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(poly);
        }
    };
    if (aeroSlm_) {
        const bool partial = (slmTrackStatus_ == 1);
        drawOffsetZones(partial ? prep_.slmWet : prep_.slmDry,
                        QColor(partial ? SLM_WET_COLOR : SLM_DRY_COLOR));
    } else {
        drawOffsetZones(prep_.drsZones, QColor(DRS_COLOR));
    }

    // … and the start/finish line (start of sector 1).
    if (prep_.hasSF && !prep_.sectors.empty()) {
        const QPolygonF& s1 = prep_.sectors[0];
        const QPointF n = perpAt(s1.constData(), s1.size(), prep_.sfIdx);
        const QPointF c = tc(prep_.sfPt);
        const double half = std::max(SF_HALF * zf, (TRACK_PX * tzf) / 2 + 4 * zf);
        // Plain track → red start/finish; colour-coded track → contrasting line.
        const QColor sfCol = sectorColors_ ? lineCol : QColor(SECTOR_DARK[0]);
        QPen pen(sfCol); pen.setWidthF(2.5 * zf); pen.setCapStyle(Qt::RoundCap);
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

void TrackMapWidget::setPositions(const PositionsRow& positions) {
    playerIdx_ = positions.player_idx;

    Snapshot snap;
    const qint64 now = idleClock_.elapsed();
    const qint64 timeoutMs = (qint64)idleTimeoutSec_ * 1000;
    for (const PositionCar& c : positions.cars) {
        Car car;
        car.idx = c.idx;
        car.x   = c.x;
        car.z   = c.z;
        if (car.idx < 0) continue;

        // Hide drivers that haven't moved for longer than the timeout. The player
        // car is always shown. Hiding is done by zeroing coords so the existing
        // (0,0) skip in interpCar()/paintEvent() drops the dot + label.
        if (idleTimeoutSec_ > 0 && car.idx != playerIdx_) {
            auto it = lastPos_.find(car.idx);
            if (it == lastPos_.end()) {
                lastPos_[car.idx] = { car.x, car.z, now };
            } else if (car.x != it->second.x || car.z != it->second.z) {
                it->second = { car.x, car.z, now };
            } else if (now - it->second.lastMovedMs > timeoutMs) {
                car.x = 0.0; car.z = 0.0;
            }
        }
        snap.cars.push_back(car);
    }

    // Measure the gap since the previous snapshot so we can interpolate.
    const qint64 dt = snapTimer_.restart();
    if (dt > 0) snapIntervalMs_ = std::clamp<double>((double)dt, 16.0, 500.0);

    prevSnap_ = std::move(curSnap_);
    curSnap_  = std::move(snap);
    update();
}

void TrackMapWidget::setParticipants(const tnrp::ParticipantsRow& participants) {
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

void TrackMapWidget::setLabelMode(LabelMode mode) {
    if (labelMode_ == mode) return;
    labelMode_ = mode;
    update();
}

void TrackMapWidget::setSectorColors(bool on) {
    if (sectorColors_ == on) return;
    sectorColors_ = on;
    rebuildStaticLayer();   // colors are baked into the cached circuit
    update();
}

void TrackMapWidget::setAeroMode(bool slm) {
    if (aeroSlm_ == slm) return;
    aeroSlm_ = slm;
    rebuildStaticLayer();   // the overlay is baked into the cached circuit
    update();
}

void TrackMapWidget::setSlmTrackStatus(int status) {
    if (slmTrackStatus_ == status) return;
    slmTrackStatus_ = status;
    if (aeroSlm_) {         // only the SLM overlay depends on the track status
        rebuildStaticLayer();
        update();
    }
}

void TrackMapWidget::setMapOpacity(double a) {
    a = std::clamp(a, 0.0, 1.0);
    if (mapOpacity_ == a) return;
    mapOpacity_ = a;
    update();   // applied at blit time, no layer rebuild needed
}

void TrackMapWidget::setIdleTimeout(int secs) {
    if (idleTimeoutSec_ == secs) return;
    idleTimeoutSec_ = secs;
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
    // Opacity dims only the track outline; driver dots/labels stay full strength.
    p.setOpacity(mapOpacity_);
    if (!hasCam_) {
        if (staticLayer_.isNull() || staticLayerSize_ != size())
            rebuildStaticLayer();
        p.drawPixmap(0, 0, staticLayer_);
    } else {
        drawTrack(p, layout, effZoom);
    }
    p.setOpacity(1.0);

    const Layout& l = layout;

    QFont labelFont("monospace", 9, QFont::Bold);
    labelFont.setStyleHint(QFont::Monospace);

    struct LabelJob { QPointF c; QString text; QColor color; };
    std::vector<LabelJob> labels;

    const auto findDriver = [&](int idx) -> const tnrp::Driver* {
        for (const tnrp::Driver& d : participants_.drivers)
            if (d.idx == idx) return &d;
        return nullptr;
    };

    for (const Car& car : curSnap_.cars) {
        double cx, cz;
        if (!interpCar(car.idx, t, cx, cz)) continue;      // skips idle / (0,0)
        const tnrp::Driver* d = findDriver(car.idx);
        if (!d) continue;

        const QPointF pt = project(cx, cz, l);
        const QColor livery(d->livery_color.empty()
            ? QStringLiteral("#8e8e8e") : QString::fromStdString(d->livery_color));

        // Dot
        if (labelMode_ != LabelMode::LabelsOnly) {
            p.setPen(Qt::NoPen);
            p.setBrush(livery);
            p.drawEllipse(pt, DOT_R, DOT_R);
        }

        // Label text: driver abbreviation, else race number
        if (labelMode_ != LabelMode::DotsOnly) {
            QString name = QString::fromStdString(d->name);
            QString text = name.trimmed().isEmpty()
                ? QString::number(d->race_number)
                : abbrev(name);
            labels.push_back({ pt, text, livery });
        }
    }

    // Labels drawn after dots so they sit on top.
    p.setFont(labelFont);
    for (const LabelJob& job : labels) {
        const double bx = job.c.x() - LABEL_W / 2.0;
        const double by = labelMode_ == LabelMode::LabelsOnly 
            ? job.c.y() - LABEL_H / 2.0 
            : job.c.y() - DOT_R - LABEL_GAP - LABEL_H;
        const QRectF box(bx, by, LABEL_W, LABEL_H);

        QPainterPath bg;
        bg.addRoundedRect(box, LABEL_R, LABEL_R);
        
        p.setPen(Qt::NoPen);
        p.setBrush(job.color);
        p.drawPath(bg);

        // Text
        double luminance = (0.299 * job.color.red() + 0.587 * job.color.green() + 0.114 * job.color.blue());
        p.setPen(luminance > 140 ? Qt::black : Qt::white);
        p.drawText(box, Qt::AlignCenter, job.text);
    }
}

void TrackMapWidget::resizeEvent(QResizeEvent*) {
    if (loaded_) rebuildStaticLayer();
    positionControls();
}

void TrackMapWidget::showEvent(QShowEvent*) {
    if (animTimer_ && renderingActive_) animTimer_->start();
    positionControls();
}

void TrackMapWidget::hideEvent(QHideEvent*) {
    if (animTimer_) animTimer_->stop();
}

void TrackMapWidget::setRenderingActive(bool on) {
    renderingActive_ = on;
    if (!animTimer_) return;
    if (on) { if (isVisible()) animTimer_->start(); }
    else    animTimer_->stop();
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

    const int ctrlH = driverCombo_->sizeHint().height();
    int currentX = width() - margin;

    // Fullscreen toggle: rightmost, aligned with the combos. Square when icon-only,
    // wide enough for the label when it's shown.
    if (fsButton_) {
        const int fw = (fsButton_->toolButtonStyle() == Qt::ToolButtonIconOnly)
            ? ctrlH
            : std::max(ctrlH, fsButton_->sizeHint().width());
        fsButton_->resize(fw, ctrlH);
        currentX -= fw;
        fsButton_->move(currentX, margin);
        fsButton_->raise();
        currentX -= gap;
    }

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
    if (!driverCombo_ || participants_.drivers.empty()) return;

    QString sig;
    std::vector<std::tuple<int, int, QString>> drivers;  // raceNum, idx, label
    for (const tnrp::Driver& d : participants_.drivers) {
        const int idx = d.idx;
        if (idx < 0) continue;
        const QString name = QString::fromStdString(d.name).trimmed();
        const int raceNum = d.race_number;
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
