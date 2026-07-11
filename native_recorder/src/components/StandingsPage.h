#pragma once

#include <QWidget>
#include <QColor>
#include <QSettings>

#include <tnrp/rows.h>
#include <tnrp/control_rows.h>

#include <unordered_map>
#include <vector>

class QLabel;
class QProgressBar;
class QTableWidget;

// Standings tab — live timing table plus the race panel (right sidebar) that
// shows the player's (or a clicked driver's) lap/energy/strategy data.
// Self-contained: owns the table/panel widgets, row-click selection, cached
// contrast colours and fastest-lap tracking. MainWindow feeds it the cached
// JSON rows via the update methods from the coalesced refresh.
class StandingsPage : public QWidget {
    Q_OBJECT

public:
    explicit StandingsPage(QWidget* parent = nullptr);

    // Cached rows fed by MainWindow; nullptr = the row hasn't been seen yet.
    void updateTimingTable(const TimingRow* timing,
                           const tnrp::ParticipantsRow* participants,
                           const AllStatusRow* allStatus);
    void updateRacePanel(const TimingRow* timing,
                         const tnrp::ParticipantsRow* participants,
                         const LapRow* playerLap,
                         const StatusRow* playerStatus,
                         const AllStatusRow* allStatus);

    // Fastest-lap tracking, fed from the row stream ("fastest_lap" /
    // "session_history_fastest"). The latter returns true when the fastest
    // holder changed — the caller marks the timing table dirty on that.
    void noteFastestLap(int carIdx);
    bool noteSessionHistoryFastest(int carIdx, int bestMs);
    void resetForNewSession();

signals:
    // A row click changed the selection; the owner re-feeds the cached rows
    // through the update methods above.
    void refreshRequested();

private:
    float contrastThreshold() const { return settings_.value("ui/contrastThreshold", 1.75f).toFloat(); }
    QWidget* buildRacePanel();

    QTableWidget*    timingTable_    = nullptr;
    int              selectedCarIdx_ = -1;   // -1 = no selection (show player)
    struct RowContrastColors {
        QColor normal;
        QColor highlighted;
        QColor fastestLap;
    };
    std::vector<int> tableRowCarIdx_;              // row index → car idx
    std::vector<RowContrastColors> rowSafeColors_; // cached contrast colors per row
    float            lastContrastThreshold_ = -1.0f;
    int              fastestLapCarIdx_ = -1;
    bool             fastestLapSet_    = false;
    std::unordered_map<int, int> sessionHistoryBest_;

    // ── Race panel (right side) ───────────────────────────────────
    QLabel*       rp_driverName = nullptr;
    QLabel*       rp_lapNum     = nullptr;
    QLabel*       rp_position   = nullptr;
    QLabel*       rp_pitStatus  = nullptr;
    QLabel*       rp_currentLap = nullptr;
    QLabel*       rp_lastLap    = nullptr;
    QLabel*       rp_s1         = nullptr;
    QLabel*       rp_s2         = nullptr;
    QProgressBar* rp_ersBar     = nullptr;
    QLabel*       rp_ersPct     = nullptr;
    QLabel*       rp_ersMode    = nullptr;
    QLabel*       rp_drs        = nullptr;
    QLabel*       rp_fuelKg     = nullptr;
    QLabel*       rp_fuelLaps   = nullptr;
    QLabel*       rp_fuelMix    = nullptr;
    QLabel*       rp_tyre       = nullptr;
    QLabel*       rp_tyreAge    = nullptr;
    QLabel*       rp_brakeBias  = nullptr;

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
