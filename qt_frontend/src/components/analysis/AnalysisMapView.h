#pragma once

#include "../../SessionModel.h"

#include <QColor>
#include <QElapsedTimer>
#include <QString>
#include <QWidget>

#include <memory>

namespace tnrp { struct AnalysisMapResult; }

class AnalysisInputCard;
class AnalysisMapLegend;
class QAction;
class QBoxLayout;
class QComboBox;
class QFrame;
class QLabel;
class QSlider;
class QTimer;
class QToolBar;
class TrackMapWidget;

// Lap comparison on the circuit: both laps' cars on the track map, a floating
// card with their driver inputs at the cursor, and — for fixed laps, which have no
// playback cursor of their own — a transport to scrub or replay the pair.
//
// The map can be painted with one of libtnrp's layers (tnrp/AnalysisMap.h):
// Track Dominance on one map, or ERS Usage, ERS Harvest, Driver Inputs and
// Speed on a map per lap, side by side on a wide pane and stacked on a tall
// one. Braking zones are highlighted over any of those, with both laps' detail
// on hover. Racing Line needs nothing from libtnrp: it strips the map to its
// sector boundaries and draws both laps' recorded positions.
class AnalysisMapView : public QWidget {
    Q_OBJECT
public:
    explicit AnalysisMapView(QWidget* parent = nullptr);
    ~AnalysisMapView() override;

    void setLaps(const LapBlock* current, const LapBlock* comparison, bool fixedMode,
                 int trackId, bool compatibleCircuit);
    void setColors(const QColor& current, const QColor& comparison);
    void setLabels(const QString& current, const QString& comparison);
    void setMapAppearance(bool sectorColors, int opacityPercent);
    void setAeroMode(bool slm);
    // Repaints the maps and their layers for the current theme.
    void refreshTheme();
    void setCurrentTime(float sessionTime);
    // Moves the cursor to a lap-relative time and holds it there.
    void focusElapsed(double seconds);
    void setReadoutVisible(bool visible);
    // Map zoom: every map on show follows the same view.
    void zoomIn();
    void zoomOut();
    void fitCircuit();
    bool isZoomed() const;
    bool isAtMaxZoom() const;
    double zoom() const;
    // `mode`: "none", "dominance", "racing", "inputs", "speed", "ers" or "harvest". The
    // layout follows the selection (`comparisonSelected`: a comparison lap is
    // chosen, loaded or not), so switching laps does not drop to one map
    // while the next result is on its way.
    void setLayerMode(const QString& mode, bool comparisonSelected);
    // libtnrp's results for the laps on show; null while none has arrived or
    // the layer / braking points are off.
    void setLayerResult(std::shared_ptr<const tnrp::AnalysisMapResult> result);
    void setBrakingResult(std::shared_ptr<const tnrp::AnalysisMapResult> result);

    // The head-to-head card (rich text) for zone `row` of a braking result: a
    // matched pair when it has a comparison lap, else one lap's zone. Empty
    // when there is no such zone.
    static QString brakingZoneCard(const tnrp::AnalysisMapResult& result, int row,
                                   const QString& currentLabel, const QColor& currentColor,
                                   const QString& comparisonLabel, const QColor& comparisonColor,
                                   const QPalette& palette);

signals:
    void cursorElapsedChanged(double seconds);
    void zoomChanged(double zoom);

protected:
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    TrackMapWidget* map_ = nullptr;
    TrackMapWidget* comparisonMap_ = nullptr;
    QFrame* mapDivider_ = nullptr;
    QBoxLayout* mapsLayout_ = nullptr;
    AnalysisMapLegend* legend_ = nullptr;
    AnalysisMapLegend* comparisonLegend_ = nullptr;
    QLabel* mismatch_ = nullptr;
    QWidget* mapArea_ = nullptr;
    AnalysisInputCard* inputs_ = nullptr;
    QToolBar* transport_ = nullptr;
    QAction* back_ = nullptr;
    QAction* play_ = nullptr;
    QAction* forward_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* time_ = nullptr;
    QComboBox* speed_ = nullptr;
    QTimer* timer_ = nullptr;
    QElapsedTimer clock_;

    LapBlock current_;
    LapBlock comparison_;
    bool hasCurrent_ = false;
    bool hasComparison_ = false;
    bool fixed_ = false;
    bool playing_ = false;
    bool focused_ = false;
    double cursor_ = 0;
    double total_ = 0;
    double rate_ = 1;
    float sessionTime_ = 0;
    QString signature_;
    QColor currentColor_;
    QColor comparisonColor_;
    QString currentLabel_;
    QString comparisonLabel_;

    // What the map shows now, and what was last asked for. They differ while a
    // new layer is being worked out, so the old one stays up until it arrives.
    QString layerMode_ = QStringLiteral("none");
    bool comparisonSelected_ = false;
    QString requestedLayerMode_ = QStringLiteral("none");
    bool requestedComparisonSelected_ = false;
    std::shared_ptr<const tnrp::AnalysisMapResult> layer_;
    std::shared_ptr<const tnrp::AnalysisMapResult> braking_;

    bool separateMaps() const;
    bool dark() const;
    double localTime() const;
    double cursorElapsed() const;
    void seek(double seconds);
    void setPlaying(bool playing);
    void refresh();
    void refreshTransport();
    void refreshLayers();
    void applyRequestedLayer();
    void placeOverlays();
};
