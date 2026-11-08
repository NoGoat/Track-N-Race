#pragma once

#include <QColor>
#include <QIcon>
#include <QString>
#include <QToolButton>
#include <QVector>
#include <QWidget>

#include <array>

class QGridLayout;
class QLabel;

namespace analysis {

// "1:32.456"; an em dash for missing or non-positive times.
QString formatLapTime(int milliseconds);
// "+0.123" / "-0.045" / "0.000"; "—.---" when unknown.
QString formatDelta(double seconds);

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

// S1 / S2 / S3 / Lap time delta between the two laps, as a band of tiles above
// the graphs. Values are coloured with the Delta series' slower / faster
// colours so they read like the chart. The tiles sit in one row, or two rows
// of two when the pane is too narrow, so none is ever cut off.
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
    void resizeEvent(QResizeEvent* event) override;

private:
    QGridLayout* grid_ = nullptr;
    QVector<QWidget*> cells_;
    int columns_ = 0;
    int shownCells_ = 0;
    QVector<QWidget*> sectorCells_;
    QVector<QLabel*> values_;
    std::array<double, 4> seconds_{};
    QColor slower_;
    QColor faster_;

    void repaintValues();
    void reflow();
};
