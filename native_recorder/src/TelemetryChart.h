#pragma once

#include "components/ChartView.h"

#include <QPointer>
#include <QVector>

class SessionModel;
struct LapBlock;
class QTimer;

// Viewing modes, mirroring the Electron Speed/RPM/ERS chart.
enum class ChartMode { Default, CurrentLap, PreviousLap, FastestLap, Compare };

// Speed / RPM / ERS overlay. A thin domain configuration over the generic
// ChartView, driven entirely by a SessionModel: it queries the model for the
// active mode's data and rebuilds its series on the model's change signals (and
// on the playback playhead). No per-point feeding — paint cost stays flat.
class TelemetryChart : public ChartView {
    Q_OBJECT

public:
    explicit TelemetryChart(QWidget* parent = nullptr);

    // The named series shown in the legend, in display order, with their colours,
    // so an external legend (e.g. in the toolbar) can stay in sync with the chart.
    struct LegendEntry { QString name; QColor color; };
    static QVector<LegendEntry> legendEntries();

    void setModel(SessionModel* m);
    void setPlaybackMode(bool on);
    void setWindowSeconds(float seconds);   // Default mode window

public slots:
    void setMode(ChartMode m);
    void setCompareLap(int lapNum);
    void setCurrentTime(float t);           // playback playhead

protected:
    void showEvent(QShowEvent* e) override;

private:
    void requestRefresh();                  // ~30 Hz coalesced
    void refresh();

    void buildDefault(float endTime);
    void buildOverlay(const LapBlock* ref, const LapBlock* cur, float curUpTo);
    
    void showReference(bool on);            // toggle the 3 muted reference series

    float currentTime() const;
    const LapBlock* currentLapBlock() const;
    const LapBlock* previousLapBlock() const;

    static constexpr float MAX_SPEED = 380.0f;
    static constexpr float MAX_RPM   = 16000.0f;

    QPointer<SessionModel> model_;
    QTimer*   refreshTimer_ = nullptr;
    bool      dirty_        = false;

    bool      playback_     = false;
    float     currentTime_  = 0.0f;
    ChartMode mode_         = ChartMode::Default;
    int       compareLap_   = -1;
    float     windowS_      = 30.0f;
    float     prevEndTime_  = -1.0f;
    float     lastAddedTime_= -1.0f;
    float     lastAddedStsTime_= -1.0f;

    int axXId_ = -1;
    int spId_  = -1, rpId_  = -1, erId_  = -1;   // current (full colour)
    int rSpId_ = -1, rRpId_ = -1, rErId_ = -1;   // reference (muted)
};
