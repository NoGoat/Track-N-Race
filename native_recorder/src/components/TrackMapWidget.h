#pragma once

#include <QWidget>
#include <QPixmap>
#include <QPolygonF>
#include <QElapsedTimer>
#include <vector>
#include <nlohmann/json.hpp>

class QTimer;
class QComboBox;

// Live track map: renders a circuit outline (sector-colored) with live car dots
// + driver labels. Mirrors the Electron TrackMap. The static circuit geometry is
// rasterised once per track/resize into an offscreen pixmap; each frame only blits
// that pixmap and draws ~20 cars, so per-frame cost stays trivial (60fps on CPU).
class TrackMapWidget : public QWidget {
    Q_OBJECT
public:
    explicit TrackMapWidget(QWidget* parent = nullptr);

    // Loads :/maps/track_<id>.json and rebuilds the prepared geometry + static layer.
    // No-op if the same track is already loaded. Returns false if no map exists.
    bool setTrack(int trackId);
    void setPositions(const nlohmann::json& positions);   // {cars:[{idx,x,z}], ...}
    void setParticipants(const nlohmann::json& participants);
    void setDark(bool dark);
    bool hasTrack() const { return loaded_; }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    // ── Track transform (world → viewBox) ─────────────────────────────────
    struct Transform { double minX, minZ, scale, offX, offZ; };

    // ── Prepared (rotated) geometry, computed once per track load ──────────
    struct Junction { QPointF pt; double nx, ny; };  // boundary point + unit perpendicular
    struct Prepared {
        std::vector<QPolygonF> sectors;          // rotated sector polylines (index 0..2)
        std::vector<std::vector<QPointF>> drsZones;  // rotated DRS track points
        std::vector<Junction> junctions;         // starts of sectors 2 & 3
        bool    hasSF = false;                   // start/finish (start of sector 1)
        QPointF sfPt;
        int     sfIdx = 0;                       // nearest index on sector-1 polyline
        double minX, minY, w, h;                 // tight bounds of rotated geometry
        double rotCos, rotSin, rotCx, rotCy;
    };

    struct Layout { double scale, ox, oy; };

    // ── Live car position snapshot (for interpolation) ────────────────────
    struct Car { int idx; double x, z; };
    struct Snapshot { std::vector<Car> cars; };

    void rebuildPrepared();
    void rebuildStaticLayer();
    void drawTrack(QPainter& p, const Layout& l, double effZoom) const;   // vector track render
    Layout buildLayout(double cw, double ch) const;
    QPointF projectViewBox(double worldX, double worldZ) const;           // world → rotated viewBox
    QPointF project(double worldX, double worldZ, const Layout& l) const; // world → device px
    bool interpCar(int idx, double t, double& outX, double& outZ) const;  // interpolated world pos
    void rebuildDriverCombo();
    void positionControls();

    // Track data
    bool        loaded_       = false;
    int         trackId_      = -1;
    Transform   transform_{};
    double      viewBoxW_     = 1000.0;
    double      viewBoxH_     = 1000.0;
    double      rotationDeg_  = 0.0;
    std::vector<std::vector<QPointF>> rawSectors_;  // viewBox-space (pre-rotation)
    std::vector<std::vector<QPointF>> rawDrs_;       // viewBox-space DRS track points
    bool        rawHasSF_ = false;
    QPointF     rawSF_;
    Prepared    prep_{};

    QPixmap     staticLayer_;        // cached circuit, device-pixel sized
    QSize       staticLayerSize_;    // logical size the cache was built for

    nlohmann::json participants_;    // {drivers:[{idx,name,livery_color,race_number}]}
    bool        dark_ = true;

    // Interpolated motion: lerp between previous and current snapshot.
    Snapshot      prevSnap_, curSnap_;
    QElapsedTimer snapTimer_;        // time since curSnap_ arrived
    double        snapIntervalMs_ = 50.0;   // measured gap between snapshots
    QTimer*       animTimer_ = nullptr;     // ~60fps redraw while visible

    // ── Follow-driver camera + zoom ───────────────────────────────────────
    QComboBox* driverCombo_ = nullptr;   // "Follow driver…"
    QComboBox* zoomCombo_   = nullptr;   // 2x / 4x / 8x / 16x
    int        selectedDriverIdx_ = -1;  // -1 = no follow
    double     zoomLevel_ = 4.0;
    Layout     cam_{};                   // active follow camera (scale, ox, oy)
    bool       hasCam_ = false;          // camera animating or active
    QString    driverSig_;               // signature to detect participant changes
};
