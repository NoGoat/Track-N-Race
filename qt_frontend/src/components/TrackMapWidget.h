#pragma once

#include <QWidget>
#include <QPixmap>
#include <QPolygonF>
#include <QElapsedTimer>
#include <QColor>
#include <QString>
#include <memory>
#include <vector>
#include <unordered_map>

#include <tnrp/rows.h>
#include <tnrp/control_rows.h>

namespace tnrp { struct AnalysisMapPaint; struct TrackMapData; }

class QComboBox;
class QLabel;
class QToolButton;

// Live track map: renders a circuit outline (sector-colored) with live car dots
// + driver labels. Mirrors the Electron TrackMap. The static circuit geometry is
// rasterised once per track/resize into an offscreen pixmap; each frame only blits
// that pixmap and draws ~20 cars, so per-frame cost stays trivial (60fps on CPU).
class TrackMapWidget : public QWidget {
    Q_OBJECT
public:
    enum class LabelMode { DotsAndLabels, DotsOnly, LabelsOnly };
    struct Marker { double x = 0; double z = 0; QString label; QColor color; };
    // A stretch of the lap line between two fractions of the lap from the
    // timing line (`to` below `from` crosses the line), drawn over the track.
    // Hovering it shows `tooltip` (rich text) when set.
    struct Mark {
        double from = 0;
        double to = 0;
        QColor color;
        double opacity = 1.0;
        QString tooltip;
    };

    explicit TrackMapWidget(QWidget* parent = nullptr);

    // Draws the track's map: the session's choice with Modded Maps on
    // (TrackMapService), else the bundled :/maps/track_<id>.json. Follows the
    // choice as it changes. Returns false if no map exists.
    bool setTrack(int trackId);
    // Draws this map instead of the track's (Edit Map's preview); null drops it.
    void setMapOverride(std::shared_ptr<const tnrp::TrackMapData> map);
    // Colours the track by lap position in place of the plain/sector colours;
    // an empty paint restores them.
    void setPaint(const tnrp::AnalysisMapPaint& paint);
    void setMarks(const QVector<Mark>& marks);
    void setPositions(const PositionsRow& positions);
    void setParticipants(const tnrp::ParticipantsRow& participants);
    void setDark(bool dark);
    void setLabelMode(LabelMode mode);
    void setSectorColors(bool on);   // rebuilds the static layer on change
    void setAeroMode(bool slm);      // false = DRS (F1 24/25), true = SLM (F1 26)
    void setSlmTrackStatus(int status);  // 0 = Full (dry), 1 = Partial (wet), -1 = n/a
    void setMapOpacity(double a);     // 0.0–1.0, track outline only
    void setIdleTimeout(int secs);    // 0 = disabled (never hide for inactivity)
    void setReduceAnimations(bool on);
    void setControlledMarkers(const QVector<Marker>& markers);
    void setControlledMode(bool on);
    bool hasTrack() const { return loaded_; }
    const QString& trackName() const { return trackName_; }
    const QString& circuitName() const { return circuitName_; }

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
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    // ── Track transform (world → viewBox) ─────────────────────────────────
    struct Transform { double minX, minZ, scale, offX, offZ; };

    // ── Prepared (rotated) geometry, computed once per track load ──────────
    struct Junction { QPointF pt; double nx, ny; };  // boundary point + unit perpendicular
    struct OffsetZone {
        std::vector<QPointF> points;
        int outwardSign = -1;
    };
    struct Prepared {
        std::vector<QPolygonF> sectors;          // rotated sector polylines (index 0..2)
        std::vector<OffsetZone> drsZones;        // rotated DRS overlay polylines
        std::vector<OffsetZone> slmDry;          // 2026 SLM overlay — Full-status zones
        std::vector<OffsetZone> slmWet;          // 2026 SLM overlay — Partial-status zones
        std::vector<QPointF> speedTraps;          // rotated speed-trap marker positions
        bool    hasOvertakeDetection = false;     // 2026 SLM detection marker
        QPointF overtakeDetection;
        bool    hasOvertakeActivation = false;    // 2026 SLM activation tick
        Junction overtakeActivation;
        std::vector<Junction> junctions;         // starts of sectors 2 & 3
        bool    hasSF = false;                   // start/finish (start of sector 1)
        QPointF sfPt;
        int     sfIdx = 0;                       // nearest index on sector-1 polyline
        // The rotated centerline from the start/finish line in driving order,
        // and each vertex's fraction of the lap by arc length.
        std::vector<QPointF> lapLine;
        std::vector<double> lapFraction;
        double minX, minY, w, h;                 // tight bounds of rotated geometry
        double rotCos, rotSin, rotCx, rotCy;
    };

    struct Layout { double scale, ox, oy; };

    // ── Live car position snapshot (for interpolation) ────────────────────
    struct Car { int idx; double x, z; };
    struct Snapshot { std::vector<Car> cars; };

    bool loadMap(const tnrp::TrackMapData& map);
    void reloadMap();
    void refreshMapPicker();
    QPointF lapPointAt(double fraction) const;
    void drawPaintedTrack(QPainter& p, const Layout& l, double width) const;
    std::vector<QPolygonF> drawMarks(QPainter& p, const Layout& l, double tzf) const;
    void showHoveredMark(int index, const QPointF& at);
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
    bool        trackSet_     = false;
    std::shared_ptr<const tnrp::TrackMapData> map_;        // the map drawn
    std::shared_ptr<const tnrp::TrackMapData> mapOverride_;

    // Painted layer: a colour per stretch of the lap, and highlighted stretches.
    std::vector<QColor> paintColors_;
    double      paintBinM_ = 0;
    double      paintTrackLengthM_ = 0;
    QColor      paintColorAt(double fraction) const;
    QVector<Mark> marks_;
    std::vector<QPolygonF> markPaths_;   // device points along each mark, for hover
    int         hoveredMark_ = -1;
    QLabel*     markCard_ = nullptr;

    // Modded maps: the map picker, Edit Map, and the choice's status.
    QComboBox*   mapCombo_ = nullptr;
    QToolButton* editButton_ = nullptr;
    QLabel*      mapStatus_ = nullptr;
    Transform   transform_{};
    double      viewBoxW_     = 1000.0;
    double      viewBoxH_     = 1000.0;
    double      rotationDeg_  = 0.0;
    QString     trackName_;             // defaults loaded from the map JSON
    QString     circuitName_;
    std::vector<std::vector<QPointF>> rawSectors_;  // viewBox-space (pre-rotation)
    // Overtaking-aid zones as {start,end} endpoints (viewBox-space); the polyline
    // is re-derived from the centerline at prepare time (see rebuildPrepared).
    std::vector<std::pair<QPointF, QPointF>> rawDrs_, rawSlmDry_, rawSlmWet_;
    std::vector<QPointF> rawSpeedTraps_;
    bool        rawHasOvertakeDetection_ = false;
    QPointF     rawOvertakeDetection_;
    bool        rawHasOvertakeActivation_ = false;
    QPointF     rawOvertakeActivation_;
    bool        rawHasSF_ = false;
    QPointF     rawSF_;
    Prepared    prep_{};

    QPixmap     staticLayer_;        // cached circuit, device-pixel sized
    QSize       staticLayerSize_;    // logical size the cache was built for

    tnrp::ParticipantsRow participants_;   // drivers: idx/name/livery_color/race_number
    struct DriverVisual { QString label; QColor color; };
    std::unordered_map<int, DriverVisual> driverVisuals_; // preformatted for paintEvent
    QString     participantsSig_;          // skips repeated copies from positions refreshes
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
    bool          renderingActive_ = true;  // false when the app window is hidden/minimized/occluded
    bool          reduceAnimations_ = false;
    void requestAnimationFrame();

    // ── Follow-driver camera + zoom + labels ──────────────────────────────
    QComboBox*   driverCombo_ = nullptr;   // "Follow driver…"
    QComboBox*   zoomCombo_   = nullptr;   // 2x / 4x / 8x / 16x
    QToolButton* fsButton_    = nullptr;   // maximise / restore the map
    bool         isFullscreen_ = false;    // host fullscreen state (drives the icon)
    int        selectedDriverIdx_ = -1;  // -1 = no follow
    double     zoomLevel_ = 4.0;
    LabelMode  labelMode_ = LabelMode::DotsAndLabels;
    QVector<Marker> controlledMarkers_;
    bool controlledMode_ = false;
    Layout     cam_{};                   // active follow camera (scale, ox, oy)
    bool       hasCam_ = false;          // camera animating or active
    QString    driverSig_;               // signature to detect participant changes
};
