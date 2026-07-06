#pragma once

#include <QWidget>
#include <QHash>
#include <QSettings>

#include <nlohmann/json.hpp>

#include "PowerLayout.h"

class QFrame;
class QLabel;
class PowerChartsWidget;
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

    // Collapse the info cards to a single line (label · value · unit). Rebuilds the
    // card row in place; MainWindow re-feeds the latest status row to repaint it.
    void setCompactMode(bool on);

private:
    void buildCards();   // (re)populate the card row at the current density
    void saveLayout(const PowerLayout& layout);
    void applyLayout(const PowerLayout& layout);

    bool compact_ = false;   // one-line cards when true (ui/compactMode)
    QHash<QString, QLabel*> cardValue_;
    QWidget* topBar_      = nullptr;
    QWidget* cardFrames_[PowerLayout::CardCount] = {};
    QFrame*  cardDivs_[PowerLayout::CardCount - 1] = {};
    QFrame*  hdiv_        = nullptr;

    PowerChartsWidget* charts_ = nullptr;   // split / harvest / store / fuel, one plot

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
