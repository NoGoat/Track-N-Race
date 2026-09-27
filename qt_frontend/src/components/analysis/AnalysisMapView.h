#pragma once

#include "../../SessionModel.h"

#include <QColor>
#include <QElapsedTimer>
#include <QString>
#include <QWidget>

class AnalysisInputCard;
class QAction;
class QComboBox;
class QLabel;
class QSlider;
class QTimer;
class QToolBar;
class TrackMapWidget;

// Lap comparison on the circuit: both laps' cars on the track map, a floating
// card with their driver inputs at the cursor, and — for fixed laps, which have no
// playback cursor of their own — a transport to scrub or replay the pair.
class AnalysisMapView : public QWidget {
    Q_OBJECT
public:
    explicit AnalysisMapView(QWidget* parent = nullptr);

    void setLaps(const LapBlock* current, const LapBlock* comparison, bool fixedMode,
                 int trackId, bool compatibleCircuit);
    void setColors(const QColor& current, const QColor& comparison);
    void setLabels(const QString& current, const QString& comparison);
    void setMapAppearance(bool sectorColors, int opacityPercent);
    void setCurrentTime(float sessionTime);
    // Moves the cursor to a lap-relative time and holds it there.
    void focusElapsed(double seconds);
    void setReadoutVisible(bool visible);

signals:
    void cursorElapsedChanged(double seconds);

protected:
    void hideEvent(QHideEvent* event) override;

private:
    TrackMapWidget* map_ = nullptr;
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

    double localTime() const;
    double cursorElapsed() const;
    void seek(double seconds);
    void setPlaying(bool playing);
    void refresh();
    void refreshTransport();
};
