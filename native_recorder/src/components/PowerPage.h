#pragma once

#include <QWidget>
#include <QHash>
#include <QSettings>

#include <nlohmann/json.hpp>

#include "PowerLayout.h"

class QFrame;
class QLabel;
class PowerChart;
class SessionModel;

// Power tab — key-driven stat cards over four PowerCharts bound to the
// SessionModel. Self-contained: owns its widgets and layout persistence;
// MainWindow feeds it the latest status row via update() and forwards
// playback state through the setters below.
class PowerPage : public QWidget {
    Q_OBJECT

public:
    explicit PowerPage(SessionModel* model, QWidget* parent = nullptr);

    // Recompute the stat cards from the latest status row (lastPlayerStatusData).
    void update(const nlohmann::json& status);

    // 4 MJ → 8 MJ harvest axis in 2026; forwarded to the harvest chart.
    void applyHarvestScale(uint16_t format);

    // "Edit Layout" dialog reads/writes through these (immediate-apply).
    PowerLayout loadLayout();
    void applyAndSaveLayout(const PowerLayout& layout);

    // Playback plumbing — forwarded to the charts.
    void setPlaybackMode(bool on, float currentTime = 0.0f);
    void setCurrentTime(float t);
    void setWindowSeconds(float secs);

private:
    void saveLayout(const PowerLayout& layout);
    void applyLayout(const PowerLayout& layout);

    QHash<QString, QLabel*> cardValue_;
    QWidget* topBar_      = nullptr;
    QWidget* cardFrames_[PowerLayout::CardCount] = {};
    QFrame*  cardDivs_[PowerLayout::CardCount - 1] = {};
    QFrame*  hdiv_        = nullptr;
    QWidget* splitContainer_ = nullptr;
    QWidget* harvContainer_ = nullptr;
    QWidget* storeContainer_ = nullptr;
    QWidget* fuelContainer_ = nullptr;
    QWidget* topChartsRow_ = nullptr;
    QWidget* bottomChartsRow_ = nullptr;
    QFrame*  vline_ = nullptr;
    QFrame*  hline1_ = nullptr;
    QFrame*  hline2_ = nullptr;
    PowerChart* splitChart_   = nullptr;
    PowerChart* harvestChart_ = nullptr;
    PowerChart* storeChart_   = nullptr;
    PowerChart* fuelChart_    = nullptr;

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
