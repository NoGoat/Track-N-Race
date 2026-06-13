#pragma once

#include <QWidget>
#include <QVector>

class TelemetryChart : public QWidget {
    Q_OBJECT

public:
    explicit TelemetryChart(QWidget* parent = nullptr);

public slots:
    void addPoint(float sessionTime, float speed, int rpm, float ers);
    void reset();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    struct Pt { float t; float speed; float rpm; float ers; };

    static constexpr float WINDOW_S  = 30.0f;
    static constexpr float MAX_SPEED = 380.0f;
    static constexpr float MAX_RPM   = 16000.0f;

    QVector<Pt> pts;

    // Map a normalised value [0,1] to a Y pixel inside the plot rect.
    static float ny(float norm, float top, float h) {
        return top + h * (1.0f - norm);
    }
};
