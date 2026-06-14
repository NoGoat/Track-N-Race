#pragma once

#include <QWidget>
#include <QPixmap>
#include <QPolygonF>
#include <QElapsedTimer>
#include <vector>
#include <nlohmann/json.hpp>

class QTimer;

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
    struct Prepared {
        std::vector<QPolygonF> sectors;     // rotated sector polylines (index 0..2)
        double minX, minY, w, h;            // tight bounds of rotated geometry
        double rotCos, rotSin, rotCx, rotCy;
    };

    struct Layout { double scale, ox, oy; };

    // ── Live car position snapshot (for interpolation) ────────────────────
    struct Car { int idx; double x, z; };
    struct Snapshot { std::vector<Car> cars; };

    void rebuildPrepared();
    void rebuildStaticLayer();
    Layout buildLayout(double cw, double ch) const;
    QPointF project(double worldX, double worldZ, const Layout& l) const;  // world → device px

    // Track data
    bool        loaded_       = false;
    int         trackId_      = -1;
    Transform   transform_{};
    double      viewBoxW_     = 1000.0;
    double      viewBoxH_     = 1000.0;
    double      rotationDeg_  = 0.0;
    std::vector<std::vector<QPointF>> rawSectors_;  // viewBox-space (pre-rotation)
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
};
