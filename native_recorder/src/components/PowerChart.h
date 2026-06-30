#pragma once

#include "ChartView.h"

#include <QPointer>
#include <QVector>

class SessionModel;
class QTimer;

enum class PowerChartType { Split, Harvest, Store, Fuel };

class PowerChart : public ChartView {
    Q_OBJECT

public:
    explicit PowerChart(PowerChartType type, QWidget* parent = nullptr);

    void setModel(SessionModel* m);
    void setPlaybackMode(bool on);
    void setWindowSeconds(float seconds);

    // Harvest chart Y-axis caps the per-lap ERS harvest limit: 4 MJ under
    // 2024/2025 regs, 8 MJ under 2026. No-op for the other chart types.
    void applyHarvestScale(uint16_t format);

public slots:
    void setCurrentTime(float t);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void requestRefresh();
    void refresh();

    void buildSplit(float endTime);
    void buildHarvest(float endTime);
    void buildStore(float endTime);
    void buildFuel(float endTime);

    void putSeries(int id, const QVector<double>& xs, const QVector<double>& ys);

    float currentTime() const;

    PowerChartType type_;
    QPointer<SessionModel> model_;
    QTimer*   refreshTimer_ = nullptr;
    bool      dirty_        = false;

    bool      playback_     = false;
    float     currentTime_  = 0.0f;
    float     windowS_      = 30.0f;
    float     prevEndTime_  = -1.0f;
    float     lastAddedTime_= -1.0f;

    int axXId_  = -1;
    int axYId_  = -1;
    int s1Id_   = -1;
    int s2Id_   = -1;
};
