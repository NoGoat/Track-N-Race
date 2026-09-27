#pragma once

#include <QColor>
#include <QIcon>
#include <QString>
#include <QToolButton>
#include <QVector>
#include <QWidget>

#include <array>

class QLabel;

namespace analysis {

// "1:32.456"; an em dash for missing or non-positive times.
QString formatLapTime(int milliseconds);
// "+0.123" / "-0.045" / "0.000"; "—.---" when unknown.
QString formatDelta(double seconds);

// Filled circle with a darker rim, sized for combo boxes and labels. Used for
// tyre compounds and for identifying a lap's colour next to its name.
QIcon dotIcon(const QColor& fill, const QWidget* context, int size = 12);

} // namespace analysis

// Shows its colour as the button face and opens a colour dialog on click.
class AnalysisColorButton : public QToolButton {
    Q_OBJECT
public:
    explicit AnalysisColorButton(const QString& dialogTitle, QWidget* parent = nullptr);

    QColor color() const { return color_; }
    void setColor(const QColor& color);

signals:
    void colorPicked(const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString dialogTitle_;
    QColor color_;
};

// S1 / S2 / S3 / Lap time delta between the two laps. Values are coloured with
// the Delta series' slower / faster colours so they read like the chart.
class AnalysisDeltaReadout : public QWidget {
    Q_OBJECT
public:
    explicit AnalysisDeltaReadout(QWidget* parent = nullptr);

    void setColors(const QColor& slower, const QColor& faster);
    void setSectorsVisible(bool visible);
    // NaN entries show as unknown.
    void setValues(const std::array<double, 4>& seconds);

protected:
    void changeEvent(QEvent* event) override;

private:
    QVector<QWidget*> sectorCells_;
    QVector<QLabel*> values_;
    std::array<double, 4> seconds_{};
    QColor slower_;
    QColor faster_;

    void repaintValues();
};
