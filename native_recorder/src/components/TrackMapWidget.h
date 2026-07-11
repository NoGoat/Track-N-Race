#pragma once

#include <QWidget>
#include <QPixmap>
#include <QPolygonF>
#include <QElapsedTimer>
#include <vector>
#include <unordered_map>

#include <tnrp/rows.h>
#include <tnrp/control_rows.h>

class QTimer;
class QComboBox;
class QToolButton;

// Live track map: renders a circuit outline (sector-colored) with live car dots
// + driver labels. Mirrors the Electron TrackMap. The static circuit geometry is
// rasterised once per track/resize into an offscreen pixmap; each frame only blits
// that pixmap and draws ~20 cars, so per-frame cost stays trivial (60fps on CPU).
class TrackMapWidget : public QWidget {
    Q_OBJECT
public:
    enum class LabelMode { DotsAndLabels, DotsOnly, LabelsOnly };

    explicit TrackMapWidget(QWidget* parent = nullptr);

    // Loads :/maps/track_<id>.json and rebuilds the prepared geometry + static layer.
    // No-op if the same track is already loaded. Returns false if no map exists.
    bool setTrack(int trackId);
    void setPositions(const PositionsRow& positions);
    void setParticipants(const tnrp::ParticipantsRow& participants);
    void setDark(bool dark);
    void setLabelMode(LabelMode mode);
    void setSectorColors(bool on);   // rebuilds the static layer on change
    void setAeroMode(bool slm);      // false = DRS (F1 24/25), true = SLM (F1 26)
    void setSlmTrackStatus(int status);  // 0 = Full (dry), 1 = Partial (wet), -1 = n/a
    void setMapOpacity(double a);     // 0.0–1.0, track outline only
    void setIdleTimeout(int secs);    // 0 = disabled (never hide for inactivity)
    bool hasTrack() const { return loaded_; }

    // External render gate (driven by MainWindow when the app window is
    // hidden/minimized/occluded). The 60fps animation timer runs only when the
    // widget is both shown and rendering is active.
    void setRenderingActive(bool on);

    // Reflects the host's fullscreen state on the overlay toggle button (swaps the
    // maximise ↔ restore icon + label). The actual layout change is the host's job.
    void setFullscreenState(bool on);

    // Show/hide the fullscreen button's text label, mirroring the app's global
    // "Show button labels in toolbar" setting (icon-only vs text-beside-icon).
    void setShowLabels(bool on);

signals:
    // The overlay fullscreen button was clicked; the host (SessionPage) maximises
    // the map to fill the session view, or restores it.
    void fullscreenToggled();

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
        std::vector<std::vector<QPointF>> drsZones;  // rotated DRS overlay polylines
        std::vector<std::vector<QPointF>> slmDry;    // 2026 SLM overlay — Full-status zones
        std::vector<std::vector<QPointF>> slmWet;    // 2026 SLM overlay — Partial-status zones
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
    // Overtaking-aid zones as {start,end} endpoints (viewBox-space); the polyline
    // is re-derived from the centerline at prepare time (see rebuildPrepared).
    std::vector<std::pair<QPointF, QPointF>> rawDrs_, rawSlmDry_, rawSlmWet_;
    bool        rawHasSF_ = false;
    QPointF     rawSF_;
    Prepared    prep_{};

    QPixmap     staticLayer_;        // cached circuit, device-pixel sized
    QSize       staticLayerSize_;    // logical size the cache was built for

    tnrp::ParticipantsRow participants_;   // drivers: idx/name/livery_color/race_number
    bool        dark_ = true;
    bool        sectorColors_ = true;   // colored sectors vs plain white/black lines
    bool        aeroSlm_       = false; // false = DRS overlay, true = SLM overlay (2026)
    int         slmTrackStatus_ = -1;   // 0 = Full (draw slm_dry), 1 = Partial (draw slm_wet)
    double      mapOpacity_   = 1.0;    // track-outline opacity (drivers stay full)

    // ── Idle-driver hiding ─────────────────────────────────────────────────
    int         idleTimeoutSec_ = 0;    // 0 = disabled
    int         playerIdx_ = -1;        // player car: never hidden for inactivity
    struct PosTrack { double x, z; qint64 lastMovedMs; };
    std::unordered_map<int, PosTrack> lastPos_;   // per-car last-moved tracking
    QElapsedTimer idleClock_;           // monotonic clock for inactivity timing

    // Interpolated motion: lerp between previous and current snapshot.
    Snapshot      prevSnap_, curSnap_;
    QElapsedTimer snapTimer_;        // time since curSnap_ arrived
    double        snapIntervalMs_ = 50.0;   // measured gap between snapshots
    QTimer*       animTimer_ = nullptr;     // ~60fps redraw while visible
    bool          renderingActive_ = true;  // false when the app window is hidden/minimized/occluded

    // ── Follow-driver camera + zoom + labels ──────────────────────────────
    QComboBox*   driverCombo_ = nullptr;   // "Follow driver…"
    QComboBox*   zoomCombo_   = nullptr;   // 2x / 4x / 8x / 16x
    QToolButton* fsButton_    = nullptr;   // maximise / restore the map
    bool         isFullscreen_ = false;    // host fullscreen state (drives the icon)
    int        selectedDriverIdx_ = -1;  // -1 = no follow
    double     zoomLevel_ = 4.0;
    LabelMode  labelMode_ = LabelMode::DotsAndLabels;
    Layout     cam_{};                   // active follow camera (scale, ox, oy)
    bool       hasCam_ = false;          // camera animating or active
    QString    driverSig_;               // signature to detect participant changes
};
