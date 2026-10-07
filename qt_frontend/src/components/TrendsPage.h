#pragma once

#include <QPointer>
#include <QSettings>
#include <QWidget>

#include <optional>

#include <tnrp/control_rows.h>
#include <tnrp/rows.h>

#include "TrendsLayout.h"
#include "../CompactSettings.h"
#include "../TrendMeasures.h"

class QComboBox;
class QFrame;
class QVBoxLayout;
class SessionModel;
class SummaryCard;
class TrendChart;
class TrendTyreChart;

// Trends tab (Electron TrendPanel.tsx): the streamed driver's current tyre
// stint — the player live, the driver selector's car in V6 playback. A strip
// of summary cards over the current stint, then the lap-time, ERS and tyre
// graphs as separate charts, one combined chart or one bar chart (Settings ▸
// Layout ▸ Trends). Each graph can show the stint or all laps.
//
// Completed laps and the stint start come from the engine's driver lap
// history push (MainWindow claims the car while the page is shown); per-lap
// ERS and fuel from the model's status history; tyre wear from its damage
// history. MainWindow feeds the latest status, tyre-set and lap rows.
class TrendsPage : public QWidget {
    Q_OBJECT

public:
    explicit TrendsPage(SessionModel* model, QWidget* parent = nullptr);

    // The streamed driver (-1 = not known yet). A change drops its lap history.
    void setDriver(int carIdx);
    int driver() const { return driver_; }
    // A pushed lap history; one for another car is ignored.
    void setLapHistory(const tnrp::DriverLapHistoryRow& history);
    // A new claim's row may still describe another car or cursor.
    void clearLapHistory();
    // Latest rows (nullptr = none yet).
    void update(const StatusRow* status, const tnrp::TyreSetsRow* tyreSets, const LapRow* lap);
    // Recharge is MGU-K plus MGU-H only where the regulations have an MGU-H.
    void setHasMguh(bool hasMguh);
    void setTyreLifeMode(bool life);

    void setPlaybackMode(bool on, float currentTime = 0.0f);
    void setCurrentTime(float t);

    // "Edit Layout" dialog and Settings read/write through these (immediate-apply).
    TrendsLayout loadLayout();
    void applyAndSaveLayout(const TrendsLayout& layout);
    TrendsChartLayout chartLayout() const;
    void setChartLayout(TrendsChartLayout layout);
    bool tyreLifeMode() const { return life_; }
    void setDensityMode(tnr::DensityMode mode);

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void buildCards();
    void buildCharts();   // (re)create the chart area for the chart layout
    void applyLayout(const TrendsLayout& layout);
    void saveLayout(const TrendsLayout& layout);
    QComboBox* makeRangeSelect(const QString& name, const char* settingsKey);
    void requestRefresh();
    void refresh();
    void refreshCards(const QVector<tnrp::SessionHistoryLap>& stintLaps, int stintStart,
                      const QHash<int, TrendLapMeasure>& measures);

    QPointer<SessionModel> model_;
    QSettings settings_{ "TrackNRace", "NativeRecorder" };
    tnr::DensityMode density_ = tnr::DensityMode::Normal;
    TrendsLayout layout_;
    TrendsChartLayout chartLayout_ = TrendsChartLayout::Separate;
    bool life_ = true;
    bool hasMguh_ = true;
    bool playback_ = false;
    float currentTime_ = 0.0f;
    bool dirty_ = false;

    int driver_ = -1;
    std::optional<tnrp::DriverLapHistoryRow> history_;
    std::optional<StatusRow> status_;
    std::optional<tnrp::TyreSetsRow> tyreSets_;
    std::optional<int> currentLap_;
    StintStatusScanner scanner_;

    QWidget* cardsBar_ = nullptr;
    QFrame* cardsDivider_ = nullptr;
    SummaryCard* cards_[TrendsLayout::StatCount] = {};
    QFrame* cardSeps_[TrendsLayout::StatCount] = {};

    QVBoxLayout* chartsLayout_ = nullptr;
    QWidget* chartsArea_ = nullptr;
    // Separate layout
    TrendChart* lapChart_ = nullptr;
    TrendChart* usedChart_ = nullptr;
    TrendChart* rechargeChart_ = nullptr;
    TrendTyreChart* tyreChart_ = nullptr;
    QWidget* ersRow_ = nullptr;
    QFrame* ersDivider_ = nullptr;      // between ERS usage and recharge
    // Combined / bar layouts
    TrendChart* singleChart_ = nullptr;
    QComboBox* lapRange_ = nullptr;
    QComboBox* usedRange_ = nullptr;
    QComboBox* rechargeRange_ = nullptr;
    QComboBox* tyreRange_ = nullptr;
    QComboBox* combinedRange_ = nullptr;
};
