#pragma once

#include "../../SessionModel.h"

#include <QColor>
#include <QPointF>
#include <QString>
#include <QWidget>

#include <array>

// "Data Comparison": a small card floating over the map with both laps' driver
// inputs at the map cursor — steering, brake, throttle and ERS as paired lanes
// in one gauge, speed and gear as values in each lap's colour.
//
// The card is drawn as one widget so it stays compact and crisp at any scale.
// Drag it by its header (or focus it and use the arrow keys, Shift for fine
// steps); its position is kept relative to the map and persisted. Return or
// the header button collapses it to the header.
class AnalysisInputCard : public QWidget {
    Q_OBJECT
public:
    // `area` is the widget the card floats over; the card becomes its child.
    explicit AnalysisInputCard(QWidget* area);

    void setIdentity(const QString& currentLabel, const QColor& currentColor,
                     const QString& comparisonLabel, const QColor& comparisonColor);
    void setReadings(const LapBlock* current, const LapBlock* comparison, double elapsed);

    QSize sizeHint() const override;

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum Channel { Steering, Brake, Throttle, Ers, Speed, Gear, ChannelCount };
    using Readings = std::array<double, ChannelCount>;

    QWidget* area_ = nullptr;
    std::array<Readings, 2> readings_{};
    std::array<QColor, 2> colors_;
    std::array<QString, 2> labels_;
    QPointF position_;          // 0..1 across the free space of the area
    bool collapsed_ = false;
    bool dragging_ = false;
    bool buttonHovered_ = false;
    QPoint dragOffset_;
    QPointF dragStart_;

    QRect toggleRect() const;
    QRect rowRect(int row) const;   // full row including its label column
    QRect fieldRect(int row) const; // gauge / values part of a row
    QRectF laneRect(int row, int lap) const;
    QString readingText(Channel channel, int lap, bool spoken) const;

    void setCollapsed(bool collapsed);
    void place();
    void moveTo(const QPoint& topLeft);
    void savePosition() const;
};
