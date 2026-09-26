#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QVector>
#include <QWidget>

#include "../SessionModel.h"

class QComboBox;
class QHideEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QSlider;
class QTimer;
class AnalyzeInputComparison;
class TrackMapWidget;

class AnalyzeMapComparison : public QWidget {
    Q_OBJECT
public:
    explicit AnalyzeMapComparison(QWidget* parent = nullptr);
    void setLaps(const LapBlock* current, const LapBlock* comparison, bool fixedMode,
                 int trackId, bool compatibleCircuit);
    void setColors(const QColor& current, const QColor& comparison);
    void setLabels(const QString& current, const QString& comparison);
    void setMapAppearance(bool sectorColors, int opacityPercent);
    void setCurrentTime(float sessionTime);
    void focusElapsed(double seconds);

signals:
    void cursorElapsedChanged(double seconds);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    static bool markerAt(const QVector<LapPositionSample>& points, float target,
                         double& x, double& z);
    void setLocalTime(double seconds);
    double localTime() const;
    double cursorElapsed() const;
    void refreshMarkers();
    void refreshInputComparison();
    void refreshTransport();

    QWidget* mapArea_ = nullptr;
    TrackMapWidget* map_ = nullptr;
    AnalyzeInputComparison* inputComparison_ = nullptr;
    QWidget* transport_ = nullptr;
    QPushButton* back_ = nullptr;
    QPushButton* play_ = nullptr;
    QPushButton* forward_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* time_ = nullptr;
    QComboBox* speed_ = nullptr;
    QTimer* timer_ = nullptr;
    LapBlock current_;
    LapBlock comparison_;
    bool hasCurrent_ = false;
    bool hasComparison_ = false;
    bool fixed_ = false;
    bool playing_ = false;
    double cursor_ = 0;
    double total_ = 0;
    double speedValue_ = 1;
    float globalTime_ = 0;
    bool focused_ = false;
    QColor currentColor_{"#5794F2"};
    QColor comparisonColor_{"#C4162A"};
    QString currentLabel_{"Current"};
    QString comparisonLabel_{"Compare"};
    QString signature_;
    QElapsedTimer clock_;
};
