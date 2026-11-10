#include "TrackMapWidget.h"
#include "ClearableComboBox.h"
#include "EditMapDialog.h"
#include "../PresentationScheduler.h"
#include "../TrackMapService.h"

#include <QPainter>
#include <QPainterPath>
#include <QComboBox>
#include <QLabel>
#include <QMouseEvent>
#include <QToolButton>
#include <QFontMetrics>
#include "../IconUtils.h"
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <cmath>
#include <algorithm>
#include <tuple>
#include <utility>

#include <tnrp/AnalysisMapPaint.h>
#include <tnrp/TrackMaps.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Styling constants (ported 1:1 from the Electron TrackMap) ──────────────
namespace {
constexpr double TRACK_PX  = 5.0;
constexpr double RACING_LINE_PX = 1.5;
constexpr double RACING_MARK_PX = 9.0;
constexpr double RACING_MARK_ALPHA = 0.4;
constexpr double DOT_R     = 7.0;
constexpr double MAP_PAD   = 24.0;  // visible padding outside every rendered map element
constexpr double DRS_PX    = 6.0;
constexpr double DRS_OFFSET= 18.0;
constexpr double TRAP_R    = 9.0;
// Reserve room for the outward DRS/SLM overlay plus half its stroke so MAP_PAD
// remains visible beyond the dashes rather than only beyond the centerline.
constexpr double MAP_FIT_PAD = MAP_PAD + DRS_OFFSET + DRS_PX / 2.0;
constexpr double SF_HALF   = 14.0;
constexpr double JUNC_HALF = 10.0;
const char* DRS_COLOR_DARK      = "#39B54A";
const char* DRS_COLOR_LIGHT     = "#237A32";
const char* SLM_DRY_COLOR_DARK  = "#46E396"; // SLM Normal grip  (slm_dry / Full status)   — bright green
const char* SLM_DRY_COLOR_LIGHT = "#16804A";
const char* SLM_WET_COLOR_DARK  = "#22D3EE"; // SLM Reduced grip (slm_wet / Partial status) — cyan
const char* SLM_WET_COLOR_LIGHT = "#087F9C";
const char* TRAP_COLOR_DARK     = "#8881DE";
const char* TRAP_COLOR_LIGHT    = "#5B54B4";
const char* OVERTAKE_COLOR_DARK  = "#95E7F0";
const char* OVERTAKE_COLOR_LIGHT = "#397F88";
constexpr int    LABEL_W   = 38;
constexpr int    LABEL_H   = 16;
constexpr int    LABEL_GAP = 5;
constexpr int    LABEL_R   = 3;
constexpr int    ACCENT_W  = 3;
constexpr double MARK_HOVER_PX = 8.0;   // how close the pointer must be to a mark
// A caret mark, in pixels at 1x zoom: the gap from the track's edge to its tip,
// its length, and half its base.
constexpr double CARET_GAP_PX = 1.6;
constexpr double CARET_LENGTH_PX = 7.0;
constexpr double CARET_HALF_BASE_PX = 3.8;

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

// Even-odd containment works for ordinary circuits and for separate lobes of
// a self-intersecting circuit such as Suzuka.
bool pointInPolygon(const QPointF& point, const std::vector<QPointF>& polygon) {
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const QPointF& pi = polygon[i];
        const QPointF& pj = polygon[j];
        if ((pi.y() > point.y()) != (pj.y() > point.y()) &&
            point.x() < (pj.x() - pi.x()) * (point.y() - pi.y()) /
                            (pj.y() - pi.y()) + pi.x()) {
            inside = !inside;
        }
    }
    return inside;
}

// Pick which local normal points outside the closed circuit. Voting per zone
// lets a figure-eight's two lobes choose opposite sides while keeping each
// individual overlay stable instead of flipping at boundary-precision noise.
int findOutwardSign(const std::vector<QPointF>& zone,
                    const std::vector<QPointF>& centerline) {
    constexpr double PROBE_DISTANCE = 2.0;
    constexpr size_t MAX_PROBES = 128;
    const size_t step = std::max(size_t(1), zone.size() / MAX_PROBES);
    int positiveVotes = 0;
    int negativeVotes = 0;

    for (size_t i = 0; i < zone.size(); i += step) {
        const QPointF n = perpAt(zone.data(), (int)zone.size(), (int)i);
        const QPointF& p = zone[i];
        const bool positiveInside = pointInPolygon(p + n * PROBE_DISTANCE, centerline);
        const bool negativeInside = pointInPolygon(p - n * PROBE_DISTANCE, centerline);
        if (positiveInside == negativeInside) continue;
        if (positiveInside) ++negativeVotes;
        else                ++positiveVotes;
    }

    if (positiveVotes != negativeVotes)
        return positiveVotes > negativeVotes ? 1 : -1;

    // Degenerate/ambiguous fallback: positive signed area is counter-clockwise,
    // whose outside is the right side of travel.
    double twiceArea = 0.0;
    for (size_t i = 0, j = centerline.size() - 1; i < centerline.size(); j = i)
        twiceArea += centerline[j].x() * centerline[i].y()
                   - centerline[i].x() * centerline[j].y();
    return twiceArea >= 0.0 ? -1 : 1;
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

} // namespace

TrackMapWidget::TrackMapWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(200, 200);
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

    // Modded maps (top-left): any saved or built-in map, kept for this track,
    // and Edit Map for the one in use. Bottom-left: the choice's status.
    mapCombo_ = new QComboBox(this);
    mapCombo_->setAccessibleName(QStringLiteral("Map"));
    mapCombo_->setToolTip(QStringLiteral("Map"));
    mapCombo_->hide();
    connect(mapCombo_, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        TrackMapService::instance().pin(mapCombo_->property("trackKey").toString(),
                                        mapCombo_->currentData().toString());
    });
    editButton_ = new QToolButton(this);
    editButton_->setIcon(adaptThemeIcon(QIcon::fromTheme(QStringLiteral("document-edit")),
                                        palette().color(QPalette::WindowText),
                                        style()->standardIcon(QStyle::SP_FileDialogDetailedView)));
    editButton_->setToolTip(QStringLiteral("Edit Map"));
    editButton_->setAccessibleName(QStringLiteral("Edit Map"));
    editButton_->hide();
    connect(editButton_, &QToolButton::clicked, this, [this] {
        const QString id = editButton_->property("mapId").toString();
        if (id.isEmpty()) return;
        EditMapDialog dialog(id, this);
        dialog.exec();
    });
    mapStatus_ = new QLabel(this);
    mapStatus_->setAttribute(Qt::WA_TransparentForMouseEvents);
    mapStatus_->setAutoFillBackground(true);
    mapStatus_->setContentsMargins(6, 2, 6, 2);
    mapStatus_->hide();

    // The hovered mark's card.
    markCard_ = new QLabel(this);
    markCard_->setAttribute(Qt::WA_TransparentForMouseEvents);
    markCard_->setTextFormat(Qt::RichText);
    markCard_->setFrameShape(QFrame::StyledPanel);
    markCard_->setAutoFillBackground(true);
    markCard_->setBackgroundRole(QPalette::ToolTipBase);
    markCard_->setForegroundRole(QPalette::ToolTipText);
    markCard_->setContentsMargins(8, 6, 8, 6);
    markCard_->hide();
    setMouseTracking(true);

    TrackMapService& service = TrackMapService::instance();
    connect(&service, &TrackMapService::sessionChanged, this, &TrackMapWidget::reloadMap);
    connect(&service, &TrackMapService::catalogChanged, this, &TrackMapWidget::reloadMap);
    connect(&service, &TrackMapService::settingsChanged, this, &TrackMapWidget::reloadMap);

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
    // Changes to the session's choice and the saved maps arrive as signals.
    if (trackSet_ && trackId == trackId_) return loaded_;
    trackSet_ = true;
    trackId_ = trackId;
    // Another circuit starts from the whole lap.
    if (view_.zoom > 1.0 && moveView(View{})) emit viewChanged(view_);
    reloadMap();
    return loaded_;
}

void TrackMapWidget::setMapOverride(std::shared_ptr<const tnrp::TrackMapData> map) {
    mapOverride_ = std::move(map);
    reloadMap();
}

void TrackMapWidget::reloadMap() {
    std::shared_ptr<const tnrp::TrackMapData> map = mapOverride_
        ? mapOverride_ : TrackMapService::instance().mapFor(trackId_);
    if (map != map_ || (map && !loaded_)) {
        map_ = map;
        if (!map_ || !loadMap(*map_)) {
            loaded_ = false;
            trackName_.clear();
            circuitName_.clear();
            markPaths_.clear();
            showHoveredMark(-1, {});
            update();
        }
    }
    refreshMapPicker();
}

bool TrackMapWidget::loadMap(const tnrp::TrackMapData& map) {
    trackName_   = QString::fromStdString(map.track_name);
    circuitName_ = QString::fromStdString(map.circuit_name);

    transform_.minX  = map.transform.min_x;
    transform_.minZ  = map.transform.min_z;
    transform_.scale = map.transform.scale;
    transform_.offX  = map.transform.off_x;
    transform_.offZ  = map.transform.off_z;
    viewBoxW_    = map.view_box.width > 0 ? map.view_box.width : 1000.0;
    viewBoxH_    = map.view_box.height > 0 ? map.view_box.height : 1000.0;
    rotationDeg_ = map.rotation_deg;

    const auto point = [](const tnrp::MapPoint& p) { return QPointF(p[0], p[1]); };
    rawSectors_.clear();
    rawSectors_.resize(3);
    for (const tnrp::TrackMapSector& sector : map.sectors) {
        std::vector<QPointF> pts;
        pts.reserve(sector.points.size());
        for (const tnrp::MapPoint& p : sector.points) pts.push_back(point(p));
        if (sector.index >= 1 && sector.index <= 3) rawSectors_[sector.index - 1] = std::move(pts);
        else                                         rawSectors_.push_back(std::move(pts));
    }
    if (buildCenterline(rawSectors_).size() < 2) return false;

    // Overtaking-aid zones store only {start,end}; the polyline is re-derived from
    // the centerline in rebuildPrepared(). DRS (F1 24/25) plus the 2026 SLM dry-
    // and wet-weather zone sets.
    const auto zones = [&](const std::vector<tnrp::TrackMapZone>& in) {
        std::vector<std::pair<QPointF, QPointF>> out;
        out.reserve(in.size());
        for (const tnrp::TrackMapZone& zone : in) out.emplace_back(point(zone.start), point(zone.end));
        return out;
    };
    rawDrs_    = zones(map.drs_zones);
    rawSlmDry_ = zones(map.slm_dry.value_or(std::vector<tnrp::TrackMapZone>{}));
    rawSlmWet_ = zones(map.slm_wet.value_or(std::vector<tnrp::TrackMapZone>{}));

    rawSpeedTraps_.clear();
    for (const tnrp::MapPoint& trap : map.speed_traps) rawSpeedTraps_.push_back(point(trap));

    const auto optionalPoint = [&](const std::optional<tnrp::MapPoint>& in, bool& present, QPointF& out) {
        present = in.has_value();
        if (present) out = point(*in);
    };
    optionalPoint(map.overtake_detection_point, rawHasOvertakeDetection_, rawOvertakeDetection_);
    optionalPoint(map.overtake_activation_point, rawHasOvertakeActivation_, rawOvertakeActivation_);
    optionalPoint(map.start_finish, rawHasSF_, rawSF_);

    loaded_ = true;
    rebuildPrepared();
    rebuildStaticLayer();
    update();
    return true;
}

// ── Modded maps: picker, Edit Map, status ──────────────────────────────────

void TrackMapWidget::refreshMapPicker() {
    if (!mapCombo_) return;
    TrackMapService& service = TrackMapService::instance();
    const tnrp::TrackMapResolution* resolution =
        mapOverride_ ? nullptr : service.resolutionFor(trackId_);
    mapCombo_->setVisible(resolution);
    const QString editable = resolution ? service.editableMapId(trackId_) : QString();
    editButton_->setVisible(!editable.isEmpty() && loaded_);
    editButton_->setProperty("mapId", editable);
    mapStatus_->setVisible(resolution && (resolution->generating || resolution->mismatch));
    if (resolution) {
        mapStatus_->setText(resolution->generating ? QStringLiteral("Building map…")
                                                   : QStringLiteral("Map doesn't fit"));
        mapStatus_->adjustSize();
        const QString trackKey = QString::fromStdString(resolution->track_key);
        const QString pinned = service.pinned(trackKey);
        QSignalBlocker block(mapCombo_);
        mapCombo_->clear();
        mapCombo_->addItem(QStringLiteral("Auto"), QString());
        for (const tnrp::TrackMapEntry& entry : service.entries()) {
            QString label = QString::fromStdString(entry.name);
            if (entry.source == "generated") label += QStringLiteral(" · Generated");
            else if (entry.source == "imported") label += QStringLiteral(" · Imported");
            mapCombo_->addItem(label, QString::fromStdString(entry.id));
        }
        const int index = mapCombo_->findData(pinned);
        mapCombo_->setCurrentIndex(index >= 0 ? index : 0);
        mapCombo_->setProperty("trackKey", trackKey);
    }
    positionControls();
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
    std::vector<QPointF> rotatedCenterline;
    rotatedCenterline.reserve(centerline.size());
    for (const QPointF& p : centerline) rotatedCenterline.push_back(rot(p));
    const auto reconstruct = [&](const std::vector<std::pair<QPointF, QPointF>>& zones) {
        std::vector<OffsetZone> out;
        out.reserve(zones.size());
        for (const auto& z : zones) {
            std::vector<QPointF> slice = smoothSeam(sliceZone(centerline, z.first, z.second));
            std::vector<QPointF> rotated; rotated.reserve(slice.size());
            for (const QPointF& p : slice) rotated.push_back(rot(p));
            if (rotated.size() >= 2) {
                const int outwardSign = findOutwardSign(rotated, rotatedCenterline);
                out.push_back({ std::move(rotated), outwardSign });
            }
        }
        return out;
    };
    prep_.drsZones = reconstruct(rawDrs_);
    prep_.slmDry   = reconstruct(rawSlmDry_);
    prep_.slmWet   = reconstruct(rawSlmWet_);
    prep_.speedTraps.clear();
    prep_.speedTraps.reserve(rawSpeedTraps_.size());
    for (const QPointF& trap : rawSpeedTraps_)
        prep_.speedTraps.push_back(rot(trap));

    prep_.hasOvertakeDetection = rawHasOvertakeDetection_;
    if (rawHasOvertakeDetection_)
        prep_.overtakeDetection = rot(rawOvertakeDetection_);

    prep_.hasOvertakeActivation = false;
    if (rawHasOvertakeActivation_ && rotatedCenterline.size() > 1) {
        const QPointF activation = rot(rawOvertakeActivation_);
        const int activationIdx = nearestIdx(rotatedCenterline, activation);
        const QPointF n = perpAt(rotatedCenterline.data(),
                                 (int)rotatedCenterline.size(),
                                 activationIdx);
        prep_.hasOvertakeActivation = true;
        prep_.overtakeActivation = { activation, n.x(), n.y() };
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

    // The lap line: the centerline from the start/finish line, each vertex's
    // fraction of the lap by arc length. Painted layers and marks draw on it.
    const size_t lapStart = rawHasSF_ && !rotatedCenterline.empty()
        ? size_t(nearestIdx(rotatedCenterline, rot(rawSF_))) : 0;
    prep_.lapLine.clear();
    prep_.lapLine.reserve(rotatedCenterline.size());
    prep_.lapLine.insert(prep_.lapLine.end(), rotatedCenterline.begin() + lapStart, rotatedCenterline.end());
    prep_.lapLine.insert(prep_.lapLine.end(), rotatedCenterline.begin(), rotatedCenterline.begin() + lapStart);
    prep_.lapFraction.assign(prep_.lapLine.size(), 0.0);
    double arc = 0.0;
    for (size_t i = 1; i < prep_.lapLine.size(); ++i) {
        const QPointF d = prep_.lapLine[i] - prep_.lapLine[i - 1];
        arc += std::hypot(d.x(), d.y());
        prep_.lapFraction[i] = arc;
    }
    if (prep_.lapLine.size() > 1) {
        const QPointF d = prep_.lapLine.front() - prep_.lapLine.back();
        const double total = arc + std::hypot(d.x(), d.y());
        if (total > 0) for (double& f : prep_.lapFraction) f /= total;
    }
}

TrackMapWidget::Layout TrackMapWidget::buildLayout(double cw, double ch) const {
    Layout l{1.0, 0.0, 0.0};
    if (prep_.w <= 0 || prep_.h <= 0) return l;
    l.scale = std::min((cw - 2 * MAP_FIT_PAD) / prep_.w,
                       (ch - 2 * MAP_FIT_PAD) / prep_.h);
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

    // Racing Line leaves only the sector boundaries and start/finish of the map.
    const bool racing = racingLinesOn_;
    const bool painted = !paintColors_.empty() || racing;
    // With the track in sector colours the boundary ticks are plain; otherwise,
    // including a painted track that hides the sector colours, the ticks carry them.
    const bool sectorTrack = sectorColors_ && !painted;
    if (racing) drawRacingLines(p, l);
    else if (painted) drawPaintedTrack(p, l, TRACK_PX * tzf);
    if (!painted) for (size_t si = 0; si < prep_.sectors.size(); ++si) {
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
        const QColor jcol = sectorTrack ? lineCol : QColor(pal[std::min(ji + 1, size_t(2))]);
        QPen pen(jcol); pen.setWidthF(3.0 * zf); pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(c.x() - jc.nx * half, c.y() - jc.ny * half),
                   QPointF(c.x() + jc.nx * half, c.y() + jc.ny * half));
    }

    // Overtaking-aid overlay: a dashed line running alongside the track, offset
    // toward the outside selected once during map preparation.
    // DRS zones (F1 24/25, green) or the 2026 SLM overlay: slm_wet on a Partial
    // track status (cyan), otherwise slm_dry (green).
    const QColor drsColor(dark_ ? DRS_COLOR_DARK : DRS_COLOR_LIGHT);
    const QColor slmDryColor(dark_ ? SLM_DRY_COLOR_DARK : SLM_DRY_COLOR_LIGHT);
    const QColor slmWetColor(dark_ ? SLM_WET_COLOR_DARK : SLM_WET_COLOR_LIGHT);
    const QColor trapColor(dark_ ? TRAP_COLOR_DARK : TRAP_COLOR_LIGHT);
    const QColor overtakeColor(dark_ ? OVERTAKE_COLOR_DARK : OVERTAKE_COLOR_LIGHT);
    const auto drawOffsetZones = [&](const std::vector<OffsetZone>& zones,
                                     const QColor& color) {
        const double offset = std::max(DRS_OFFSET * zf, (TRACK_PX * tzf) / 2 + 8 * zf);
        QPen pen(color);
        pen.setWidthF(DRS_PX * zf);
        pen.setCapStyle(Qt::FlatCap);
        pen.setJoinStyle(Qt::MiterJoin);
        pen.setDashPattern({ 3.0 / DRS_PX, 3.0 / DRS_PX });  // 3px dash / 3px gap
        for (const OffsetZone& zone : zones) {
            if (zone.points.size() < 2) continue;
            QPolygonF poly;
            poly.reserve((int)zone.points.size());
            for (int i = 0; i < (int)zone.points.size(); ++i) {
                const QPointF n = perpAt(zone.points.data(), (int)zone.points.size(), i);
                const QPointF c = tc(zone.points[i]);
                poly << QPointF(c.x() + n.x() * offset * zone.outwardSign,
                                c.y() + n.y() * offset * zone.outwardSign);
            }
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(poly);
        }
    };
    if (aeroSlm_ && !racing) {
        const bool partial = (slmTrackStatus_ == 1);
        drawOffsetZones(partial ? prep_.slmWet : prep_.slmDry,
                        partial ? slmWetColor : slmDryColor);

        // Detection is a filled dot matching the speed-trap size; activation is
        // a perpendicular tick. Both belong only to the active 2026 SLM overlay.
        // A painted track leaves the overtake markers out, as it does speed traps.
        if (prep_.hasOvertakeDetection && !painted) {
            const double markerR = std::max(TRAP_R * zf, (TRACK_PX * tzf) / 2 + 4 * zf);
            p.setPen(Qt::NoPen);
            p.setBrush(overtakeColor);
            p.drawEllipse(tc(prep_.overtakeDetection), markerR, markerR);
        }
        if (prep_.hasOvertakeActivation && !painted) {
            const Junction& jc = prep_.overtakeActivation;
            const QPointF c = tc(jc.pt);
            const double half = std::max(JUNC_HALF * zf, (TRACK_PX * tzf) / 2 + 4 * zf);
            QPen pen(overtakeColor);
            pen.setWidthF(3.0 * zf);
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawLine(QPointF(c.x() - jc.nx * half, c.y() - jc.ny * half),
                       QPointF(c.x() + jc.nx * half, c.y() + jc.ny * half));
        }
    } else if (!racing) {
        drawOffsetZones(prep_.drsZones, drsColor);
    }

    // Speed traps are simple filled dots, matching the Electron map.
    const double trapR = std::max(TRAP_R * zf, (TRACK_PX * tzf) / 2 + 4 * zf);
    p.setPen(Qt::NoPen);
    p.setBrush(trapColor);
    if (!painted) for (const QPointF& trap : prep_.speedTraps)
        p.drawEllipse(tc(trap), trapR, trapR);

    // … and the start/finish line (start of sector 1).
    if (prep_.hasSF && !prep_.sectors.empty()) {
        const QPolygonF& s1 = prep_.sectors[0];
        const QPointF n = perpAt(s1.constData(), s1.size(), prep_.sfIdx);
        const QPointF c = tc(prep_.sfPt);
        const double half = std::max(SF_HALF * zf, (TRACK_PX * tzf) / 2 + 4 * zf);
        // Plain track → red start/finish; colour-coded track → contrasting line.
        const QColor sfCol = sectorTrack ? lineCol : QColor(SECTOR_DARK[0]);
        QPen pen(sfCol); pen.setWidthF(2.5 * zf); pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(c.x() - n.x() * half, c.y() - n.y() * half),
                   QPointF(c.x() + n.x() * half, c.y() + n.y() * half));
    }
}

// ── Painted layer and marks ────────────────────────────────────────────────

void TrackMapWidget::setPaint(const tnrp::AnalysisMapPaint& paint) {
    std::vector<QColor> colors;
    colors.reserve(paint.colors.size());
    for (const std::string& color : paint.colors) colors.emplace_back(QString::fromStdString(color));
    if (colors == paintColors_ && paint.binM == paintBinM_ && paint.trackLengthM == paintTrackLengthM_)
        return;
    paintColors_ = std::move(colors);
    paintBinM_ = paint.binM;
    paintTrackLengthM_ = paint.trackLengthM;
    rebuildStaticLayer();
    update();
}

QColor TrackMapWidget::paintColorAt(double fraction) const {
    if (paintColors_.empty()) return {};
    const double n = double(paintColors_.size());
    // A bin length of 0 spreads the colours evenly over the lap.
    const double bin = paintBinM_ > 0 && paintTrackLengthM_ > 0
        ? std::floor(fraction * paintTrackLengthM_ / paintBinM_) : std::floor(fraction * n);
    return paintColors_[size_t(std::clamp(bin, 0.0, n - 1))];
}

// The lap line in runs of one colour, each segment coloured at its midpoint.
void TrackMapWidget::drawPaintedTrack(QPainter& p, const Layout& l, double width) const {
    const auto& pts = prep_.lapLine;
    const auto& fractions = prep_.lapFraction;
    const size_t n = pts.size();
    if (n < 2) return;
    const auto tc = [&](const QPointF& vb) { return QPointF(vb.x() * l.scale + l.ox, vb.y() * l.scale + l.oy); };
    QPen pen;
    pen.setWidthF(width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setBrush(Qt::NoBrush);
    QPolygonF run;
    QColor runColor;
    const auto flush = [&] {
        if (run.size() < 2 || !runColor.isValid()) return;
        pen.setColor(runColor);
        p.setPen(pen);
        p.drawPolyline(run);
    };
    for (size_t i = 0; i < n; ++i) {
        const size_t next = (i + 1) % n;
        const QColor color = paintColorAt((fractions[i] + (next == 0 ? 1.0 : fractions[next])) / 2.0);
        if (run.isEmpty() || color != runColor) {
            flush();
            run.clear();
            runColor = color;
            run << tc(pts[i]);
        }
        run << tc(pts[next]);
    }
    flush();
}

void TrackMapWidget::setRacingLines(bool on, const QVector<RacingLine>& lines) {
    if (!on && !racingLinesOn_) return;
    racingLinesOn_ = on;
    racingLines_ = lines;
    rebuildStaticLayer();
    update();
}

void TrackMapWidget::setViewEnabled(bool on) {
    viewEnabled_ = on;
    refreshViewCursor();
}

// Keeps a zoomed view inside what the whole-circuit layout shows.
void TrackMapWidget::clampView(View& view) const {
    view.zoom = std::clamp(view.zoom, 1.0, kMaxViewZoom);
    if (!loaded_) return;
    const Layout base = buildLayout(width(), height());
    if (!(base.scale > 0)) return;
    const double room = (1.0 - 1.0 / view.zoom) / (2.0 * base.scale);
    const double midX = (width() / 2.0 - base.ox) / base.scale;
    const double midY = (height() / 2.0 - base.oy) / base.scale;
    view.cx = std::clamp(view.cx, midX - width() * room, midX + width() * room);
    view.cy = std::clamp(view.cy, midY - height() * room, midY + height() * room);
}

bool TrackMapWidget::moveView(const View& view) {
    View next = view;
    clampView(next);
    if (next.zoom == view_.zoom && next.cx == view_.cx && next.cy == view_.cy) return false;
    view_ = next;
    showHoveredMark(-1, {});
    refreshViewCursor();
    update();
    return true;
}

void TrackMapWidget::setView(const View& view) {
    moveView(view);
}

void TrackMapWidget::zoomBy(double factor) {
    View next = view_;
    clampView(next);
    next.zoom *= factor;
    if (moveView(next)) emit viewChanged(view_);
}

void TrackMapWidget::fitView() {
    View next = view_;
    next.zoom = 1.0;
    if (moveView(next)) emit viewChanged(view_);
}

void TrackMapWidget::refreshViewCursor() {
    if (!viewEnabled_ || view_.zoom <= 1.0) unsetCursor();
    else setCursor(dragging_ ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
}

void TrackMapWidget::wheelEvent(QWheelEvent* event) {
    if (!viewEnabled_ || !loaded_) {
        QWidget::wheelEvent(event);
        return;
    }
    View next = view_;
    clampView(next);
    const Layout base = buildLayout(width(), height());
    const QPointF at = event->position() - QPointF(width() / 2.0, height() / 2.0);
    const double before = base.scale * next.zoom;
    next.zoom = std::clamp(next.zoom * std::exp(event->angleDelta().y() * 0.0015), 1.0, kMaxViewZoom);
    const double after = base.scale * next.zoom;
    next.cx += at.x() / before - at.x() / after;
    next.cy += at.y() / before - at.y() / after;
    if (moveView(next)) emit viewChanged(view_);
    event->accept();
}

void TrackMapWidget::mousePressEvent(QMouseEvent* event) {
    if (!viewEnabled_ || view_.zoom <= 1.0 || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    dragStart_ = event->position();
    dragView_ = view_;
    showHoveredMark(-1, {});
    refreshViewCursor();
}

void TrackMapWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (!dragging_ || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    dragging_ = false;
    refreshViewCursor();
}

void TrackMapWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!viewEnabled_ || event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    dragging_ = false;
    fitView();
    refreshViewCursor();
}

// Each lap's path through every recorded position, over its braking zones:
// translucent bands along the path, under the lines so both stay readable.
void TrackMapWidget::drawRacingLines(QPainter& p, const Layout& l) const {
    p.setBrush(Qt::NoBrush);
    QPen band;
    band.setWidthF(RACING_MARK_PX);
    band.setCapStyle(Qt::FlatCap);
    band.setJoinStyle(Qt::RoundJoin);
    for (const Mark& mark : marks_) {
        if (mark.world.size() < 2) continue;
        QPolygonF path;
        path.reserve(mark.world.size());
        for (const QPointF& world : mark.world) path << project(world.x(), world.y(), l);
        band.setColor(mark.color);
        p.save();
        p.setOpacity(p.opacity() * mark.opacity * RACING_MARK_ALPHA);
        p.setPen(band);
        p.drawPolyline(path);
        p.restore();
    }

    QPen pen;
    pen.setWidthF(RACING_LINE_PX);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    for (const RacingLine& line : racingLines_) {
        QPolygonF path;
        path.reserve(line.world.size());
        for (const QPointF& world : line.world) path << project(world.x(), world.y(), l);
        pen.setColor(line.color);
        p.setPen(pen);
        p.drawPolyline(path);
    }
}

// The lap line at a fraction of the lap.
QPointF TrackMapWidget::lapPointAt(double fraction) const {
    const auto& pts = prep_.lapLine;
    const auto& fractions = prep_.lapFraction;
    const size_t n = pts.size();
    if (n == 0) return {};
    const double f = std::fmod(std::fmod(fraction, 1.0) + 1.0, 1.0);
    size_t lo = size_t(std::upper_bound(fractions.begin(), fractions.end(), f) - fractions.begin());
    lo = lo > 0 ? lo - 1 : 0;
    const size_t next = (lo + 1) % n;
    const double end = next == 0 ? 1.0 : fractions[next];
    const double span = end - fractions[lo];
    const double t = span > 0 ? (f - fractions[lo]) / span : 0.0;
    return pts[lo] + (pts[next] - pts[lo]) * t;
}

void TrackMapWidget::setMarks(const QVector<Mark>& marks) {
    marks_ = marks;
    showHoveredMark(-1, {});
    if (racingLinesOn_) rebuildStaticLayer();   // their bands are under the lines
    update();
}

// A caret on the inside of the bend at a stretch's middle, pointing at the
// track. The inside comes from the drawn line, over twice the stretch's length
// either side. Returns the caret's centre, for hover.
QPointF TrackMapWidget::drawCaret(QPainter& p, const Layout& l, double tzf, const Mark& mark, double to) const {
    const auto at = [&](double fraction) {
        const QPointF vb = lapPointAt(fraction);
        return QPointF(vb.x() * l.scale + l.ox, vb.y() * l.scale + l.oy);
    };
    const double middle = (mark.from + to) / 2.0;
    const double reach = (to - mark.from) * 2.0;
    const QPointF centre = at(middle);
    const QPointF before = at(middle - reach);
    const QPointF after = at(middle + reach);
    const QPointF chord = at(to) - at(mark.from);
    const double length = std::hypot(chord.x(), chord.y());
    const QPointF tangent = length > 0 ? chord / length : QPointF(1.0, 0.0);
    // Device y runs down, so a positive cross product is a clockwise bend,
    // whose inside is on the right of the direction of travel.
    const QPointF in = centre - before;
    const QPointF out = after - centre;
    const double inward = in.x() * out.y() - in.y() * out.x() >= 0 ? 1.0 : -1.0;
    const QPointF normal(-tangent.y() * inward, tangent.x() * inward);
    const double tip = TRACK_PX * tzf / 2.0 + CARET_GAP_PX * tzf;
    const double base = tip + CARET_LENGTH_PX * tzf;
    const QPointF baseCentre = centre + normal * base;
    const QPointF halfBase = tangent * (CARET_HALF_BASE_PX * tzf);
    p.save();
    p.setOpacity(p.opacity() * mark.opacity);
    p.setPen(Qt::NoPen);
    p.setBrush(mark.color);
    p.drawPolygon(QPolygonF{centre + normal * tip, baseCentre + halfBase, baseCentre - halfBase});
    p.restore();
    return centre + normal * ((tip + base) / 2.0);
}

// Each mark over the track: its caret, or its stretch of the lap line, a little
// wider than the track, with square ends cut where it starts and stops, edged
// in the background colour so it stays distinct from the track under it.
// Returns the device points along each mark, for hover.
std::vector<QPolygonF> TrackMapWidget::drawMarks(QPainter& p, const Layout& l, double tzf) const {
    std::vector<QPolygonF> paths;
    if (prep_.lapLine.size() < 2 || marks_.isEmpty()) return paths;
    if (racingLinesOn_) {   // drawRacingLines paints them; these are for hover
        paths.reserve(size_t(marks_.size()));
        for (const Mark& mark : marks_) {
            QPolygonF path;
            path.reserve(mark.world.size());
            for (const QPointF& world : mark.world) path << project(world.x(), world.y(), l);
            paths.push_back(std::move(path));
        }
        return paths;
    }
    const double width = TRACK_PX * tzf + 3;
    const auto tc = [&](const QPointF& vb) { return QPointF(vb.x() * l.scale + l.ox, vb.y() * l.scale + l.oy); };
    const QColor edge = palette().color(QPalette::Window);
    paths.reserve(size_t(marks_.size()));
    for (const Mark& mark : marks_) {
        const double from = mark.from;
        const double to = mark.to >= from ? mark.to : mark.to + 1.0;
        QPolygonF path;
        path << tc(lapPointAt(from));
        for (int lap = 0; lap <= 1; ++lap)
            for (double vertex : prep_.lapFraction) {
                const double f = vertex + lap;
                if (f > from && f < to) path << tc(lapPointAt(f));
            }
        path << tc(lapPointAt(to));
        if (mark.caret) {
            path << drawCaret(p, l, tzf, mark, to);
            paths.push_back(std::move(path));
            continue;
        }
        QPen pen(edge);
        pen.setCapStyle(Qt::FlatCap);
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setWidthF(width + 3);
        p.setBrush(Qt::NoBrush);
        p.setPen(pen);
        p.drawPolyline(path);
        p.save();
        p.setOpacity(p.opacity() * mark.opacity);
        pen.setColor(mark.color);
        pen.setWidthF(width);
        p.setPen(pen);
        p.drawPolyline(path);
        p.restore();
        paths.push_back(std::move(path));
    }
    return paths;
}

void TrackMapWidget::mouseMoveEvent(QMouseEvent* event) {
    QWidget::mouseMoveEvent(event);
    if (dragging_) {
        const double scale = buildLayout(width(), height()).scale * dragView_.zoom;
        const QPointF moved = event->position() - dragStart_;
        View next = dragView_;
        next.cx -= moved.x() / scale;
        next.cy -= moved.y() / scale;
        if (moveView(next)) emit viewChanged(view_);
        return;
    }
    const QPointF pointer = event->position();
    // The nearest highlighted stretch, by its closest point to the pointer.
    int best = -1;
    double bestDistance = MARK_HOVER_PX;
    QPointF bestPoint = pointer;
    for (size_t index = 0; index < markPaths_.size() && index < size_t(marks_.size()); ++index) {
        if (marks_[int(index)].tooltip.isEmpty()) continue;
        // Along each segment, not just at its ends: zoomed in they are far apart.
        const QPolygonF& path = markPaths_[index];
        for (int k = 0; k < path.size(); ++k) {
            const QPointF a = path[k];
            const QPointF d = path[std::min(k + 1, int(path.size()) - 1)] - a;
            const double lengthSq = d.x() * d.x() + d.y() * d.y();
            const double t = lengthSq > 0
                ? std::clamp(QPointF::dotProduct(pointer - a, d) / lengthSq, 0.0, 1.0) : 0.0;
            const QPointF point = a + d * t;
            const double distance = std::hypot(point.x() - pointer.x(), point.y() - pointer.y());
            if (distance <= bestDistance) { best = int(index); bestDistance = distance; bestPoint = point; }
        }
    }
    if (best != hoveredMark_) showHoveredMark(best, bestPoint);
}

void TrackMapWidget::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    showHoveredMark(-1, {});
}

void TrackMapWidget::showHoveredMark(int index, const QPointF& at) {
    hoveredMark_ = index;
    if (!markCard_) return;
    if (index < 0 || index >= marks_.size() || marks_[index].tooltip.isEmpty()) {
        markCard_->hide();
        return;
    }
    markCard_->setText(marks_[index].tooltip);
    markCard_->adjustSize();
    const QSize card = markCard_->size();
    const int x = at.x() + 12 + card.width() > width() ? int(at.x()) - 12 - card.width() : int(at.x()) + 12;
    const int y = std::min(int(at.y()) + 12, std::max(0, height() - card.height()));
    markCard_->move(std::max(0, x), y);
    markCard_->raise();
    markCard_->show();
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
    requestAnimationFrame();
}

void TrackMapWidget::requestAnimationFrame() {
    if (!renderingActive_ || !isVisible()) return;
    PresentationScheduler::instance().request(this, [this] {
        if (!renderingActive_ || !isVisible()) return;
        update();
        // Continue only while a snapshot is actually being interpolated. A
        // visible but stationary map no longer repaints forever at 60 Hz.
        if (!reduceAnimations_ && !prevSnap_.cars.empty() && snapTimer_.elapsed() < snapIntervalMs_)
            requestAnimationFrame();
    }, PresentationScheduler::Policy::Animation);
}

void TrackMapWidget::setReduceAnimations(bool on) {
    if (reduceAnimations_ == on) return;
    reduceAnimations_ = on;
    if (on) PresentationScheduler::instance().cancel(this);
    update();
}

void TrackMapWidget::setParticipants(const tnrp::ParticipantsRow& participants) {
    QString signature;
    signature.reserve((int)participants.drivers.size() * 32);
    for (const tnrp::Driver& driver : participants.drivers) {
        signature += QString::number(driver.idx) + QLatin1Char('|')
            + QString::number(driver.race_number) + QLatin1Char('|')
            + QString::fromStdString(driver.name) + QLatin1Char('|')
            + QString::fromStdString(driver.livery_color) + QLatin1Char(';');
    }
    if (signature == participantsSig_) return;
    participantsSig_ = std::move(signature);
    participants_ = participants;
    driverVisuals_.clear();
    driverVisuals_.reserve(participants_.drivers.size());
    for (const tnrp::Driver& driver : participants_.drivers) {
        if (driver.idx < 0) continue;
        const QString name = QString::fromStdString(driver.name);
        DriverVisual visual;
        visual.label = name.trimmed().isEmpty()
            ? QString::number(driver.race_number) : abbrev(name);
        visual.color = QColor(driver.livery_color.empty()
            ? QStringLiteral("#8e8e8e") : QString::fromStdString(driver.livery_color));
        driverVisuals_.emplace(driver.idx, std::move(visual));
    }
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

void TrackMapWidget::setControlledMarkers(const QVector<Marker>& markers) {
    controlledMarkers_ = markers;
    update();
}

void TrackMapWidget::setControlledMode(bool on) {
    if (controlledMode_ == on) return;
    controlledMode_ = on;
    driverCombo_->setVisible(!on);
    zoomCombo_->setVisible(!on && selectedDriverIdx_ >= 0);
    fsButton_->setVisible(!on);
    selectedDriverIdx_ = -1;
    hasCam_ = false;
    update();
}

bool TrackMapWidget::interpCar(int idx, double t, double& outX, double& outZ) const {
    const Car* cur = nullptr;
    for (const Car& c : curSnap_.cars) if (c.idx == idx) { cur = &c; break; }
    if (!cur || !std::isfinite(cur->x) || !std::isfinite(cur->z) ||
        (cur->x == 0.0 && cur->z == 0.0)) return false;
    outX = cur->x; outZ = cur->z;
    for (const Car& pv : prevSnap_.cars)
        if (pv.idx == idx) {
            if (std::isfinite(pv.x) && std::isfinite(pv.z) &&
                !(pv.x == 0.0 && pv.z == 0.0)) {
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
                   trackId_ < 0 && !mapOverride_ ? "Waiting for session…" : "No map for this track");
        markPaths_.clear();
        return;
    }

    // Interpolation factor between prev and cur snapshot.
    double t = 1.0;
    if (!reduceAnimations_ && !prevSnap_.cars.empty())
        t = std::clamp(snapTimer_.elapsed() / snapIntervalMs_, 0.0, 1.0);

    // ── Camera: follow selected driver with smooth zoom, else full map ────
    const Layout base = buildLayout(width(), height());
    Layout layout = base;
    double fx, fz;
    if (selectedDriverIdx_ >= 0 && interpCar(selectedDriverIdx_, t, fx, fz)) {
        const QPointF r = projectViewBox(fx, fz);
        const double followScale = base.scale * zoomLevel_;
        if (!hasCam_) { cam_ = { base.scale, 0.0, 0.0 }; hasCam_ = true; }
        cam_.scale = reduceAnimations_ ? followScale
                                       : cam_.scale + (followScale - cam_.scale) * 0.12;
        cam_.ox = width()  / 2.0 - r.x() * cam_.scale;          // snap-pan to centre driver
        cam_.oy = height() / 2.0 - r.y() * cam_.scale;
        layout = cam_;
    } else if (selectedDriverIdx_ < 0 && hasCam_) {
        if (reduceAnimations_) {
            cam_ = base;
            hasCam_ = false;
        } else {
            cam_.scale += (base.scale - cam_.scale) * 0.12;
            cam_.ox    += (base.ox    - cam_.ox)    * 0.12;
            cam_.oy    += (base.oy    - cam_.oy)    * 0.12;
        }
        const double diff = std::abs(cam_.scale - base.scale)
                          + std::abs(cam_.ox - base.ox) + std::abs(cam_.oy - base.oy);
        if (diff < 0.5) hasCam_ = false;
        else            layout = cam_;
    }
    if (viewEnabled_) clampView(view_);   // the size or the map may have changed
    const bool viewZoomed = viewEnabled_ && !hasCam_ && view_.zoom > 1.0;
    if (viewZoomed) {
        const double scale = base.scale * view_.zoom;
        layout = { scale, width() / 2.0 - view_.cx * scale, height() / 2.0 - view_.cy * scale };
    }
    const double effZoom = base.scale > 0 ? layout.scale / base.scale : 1.0;

    // ── Track: cached pixmap when idle, live vector render when zoomed ───
    // Opacity dims only the track outline; driver dots/labels stay full strength.
    p.setOpacity(paintColors_.empty() && !racingLinesOn_ ? mapOpacity_ : 1.0);
    if (!hasCam_ && !viewZoomed) {
        if (staticLayer_.isNull() || staticLayerSize_ != size())
            rebuildStaticLayer();
        p.drawPixmap(0, 0, staticLayer_);
    } else {
        drawTrack(p, layout, effZoom);
    }
    p.setOpacity(1.0);
    {
        std::vector<QPolygonF> paths = drawMarks(p, layout, std::pow(effZoom, 0.8));
        if (!hasCam_) markPaths_ = std::move(paths);
    }

    const Layout& l = layout;

    QFont labelFont("monospace", 9, QFont::Bold);
    labelFont.setStyleHint(QFont::Monospace);

    struct LabelJob { QPointF c; QString text; QColor color; };
    std::vector<LabelJob> labels;
    labels.reserve(controlledMode_ ? controlledMarkers_.size() : curSnap_.cars.size());

    if (controlledMode_) for (const Marker& marker : controlledMarkers_) {
        if (!std::isfinite(marker.x) || !std::isfinite(marker.z)) continue;
        const QPointF pt = project(marker.x, marker.z, l);
        p.setPen(Qt::NoPen);p.setBrush(marker.color);p.drawEllipse(pt,DOT_R,DOT_R);
        labels.push_back({pt,marker.label,marker.color});
    }

    if (!controlledMode_) for (const Car& car : curSnap_.cars) {
        double cx, cz;
        if (!interpCar(car.idx, t, cx, cz)) continue;      // skips idle / (0,0)
        const auto visual = driverVisuals_.find(car.idx);
        if (visual == driverVisuals_.end()) continue;

        const QPointF pt = project(cx, cz, l);
        const QColor& livery = visual->second.color;

        // Dot
        if (labelMode_ != LabelMode::LabelsOnly) {
            p.setPen(Qt::NoPen);
            p.setBrush(livery);
            p.drawEllipse(pt, DOT_R, DOT_R);
        }

        // Label text: driver abbreviation, else race number
        if (labelMode_ != LabelMode::DotsOnly) {
            labels.push_back({ pt, visual->second.label, livery });
        }
    }

    // Labels drawn after dots so they sit on top.
    p.setFont(labelFont);
    const QFontMetricsF labelMetrics(labelFont, this);
    for (const LabelJob& job : labels) {
        // Electron: at least LABEL_W, else wide enough for the text plus 6px a
        // side, so longer names ("Current", custom lap labels) are not clipped.
        constexpr double kTextPadX = 6.0;
        const double w = std::max<double>(LABEL_W,
            std::ceil(labelMetrics.horizontalAdvance(job.text) + kTextPadX * 2));
        const double bx = job.c.x() - w / 2.0;
        const double by = labelMode_ == LabelMode::LabelsOnly
            ? job.c.y() - LABEL_H / 2.0
            : job.c.y() - DOT_R - LABEL_GAP - LABEL_H;
        const QRectF box(bx, by, w, LABEL_H);

        p.setPen(Qt::NoPen);
        p.setBrush(job.color);
        p.drawRoundedRect(box, LABEL_R, LABEL_R);

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
    requestAnimationFrame();
    positionControls();
}

void TrackMapWidget::hideEvent(QHideEvent*) {
    PresentationScheduler::instance().cancel(this);
}

void TrackMapWidget::setRenderingActive(bool on) {
    renderingActive_ = on;
    if (on) requestAnimationFrame();
    else PresentationScheduler::instance().cancel(this);
}

// ── Follow-driver controls ──────────────────────────────────────────────────

void TrackMapWidget::positionControls() {
    if (!driverCombo_) return;
    const int margin = 8, gap = 6;
    if (mapCombo_) {
        const int ctrlH = mapCombo_->sizeHint().height();
        mapCombo_->setFixedWidth(176);
        mapCombo_->move(margin, margin);
        mapCombo_->raise();
        editButton_->resize(ctrlH, ctrlH);
        editButton_->move(margin + (mapCombo_->isVisible() ? mapCombo_->width() + gap : 0), margin);
        editButton_->raise();
        mapStatus_->move(margin, height() - margin - mapStatus_->height());
        mapStatus_->raise();
    }
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
