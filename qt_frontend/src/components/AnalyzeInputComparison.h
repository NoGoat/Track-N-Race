#pragma once

#include <QColor>
#include <QFrame>
#include <QPoint>

#include "../SessionModel.h"

class QLabel;
class QResizeEvent;
class QToolButton;
class QWidget;

class AnalyzeInputComparison : public QFrame {
    Q_OBJECT
public:
    explicit AnalyzeInputComparison(QWidget* mapArea);

    void setLaps(const LapBlock* current, const LapBlock* comparison);
    void setColors(const QColor& current, const QColor& comparison);
    void setLabels(const QString& current, const QString& comparison);
    void setElapsed(double seconds);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    class DualBar;

    static const TelSample* telemetryAt(const LapBlock* lap, double elapsed);
    static const StsSample* statusAt(const LapBlock* lap, double elapsed);
    void setCollapsed(bool collapsed);
    void refreshReadings(bool force = false);
    void updatePosition();
    void moveToPixel(const QPoint& position, bool persist);
    void savePosition();

    QWidget* mapArea_ = nullptr;
    QToolButton* dragHandle_ = nullptr;
    QToolButton* collapse_ = nullptr;
    QWidget* body_ = nullptr;
    QLabel* currentLegend_ = nullptr;
    QLabel* comparisonLegend_ = nullptr;
    DualBar* steering_ = nullptr;
    DualBar* brake_ = nullptr;
    DualBar* throttle_ = nullptr;
    DualBar* ers_ = nullptr;
    QLabel* currentSpeed_ = nullptr;
    QLabel* comparisonSpeed_ = nullptr;
    QLabel* currentGear_ = nullptr;
    QLabel* comparisonGear_ = nullptr;
    const LapBlock* current_ = nullptr;
    const LapBlock* comparison_ = nullptr;
    const TelSample* lastCurrentSample_ = nullptr;
    const TelSample* lastComparisonSample_ = nullptr;
    const StsSample* lastCurrentStatus_ = nullptr;
    const StsSample* lastComparisonStatus_ = nullptr;
    bool collapsed_ = false;
    bool dragging_ = false;
    double elapsed_ = 0.0;
    double normalizedX_ = 0.0;
    double normalizedY_ = 0.0;
    QPoint dragOffset_;
    QColor currentColor_{"#5794F2"};
    QColor comparisonColor_{"#C4162A"};
    QString currentLabel_{"Current lap"};
    QString comparisonLabel_{"Comparison lap"};
};
