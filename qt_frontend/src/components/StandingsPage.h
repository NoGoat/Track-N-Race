#pragma once

#include <QSet>
#include <QWidget>
#include <QColor>
#include <QSettings>

#include <tnrp/rows.h>
#include "../CompactSettings.h"
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
                         const AllStatusRow* allStatus,
                         bool playerDrsAvailable = true,
                         const QSet<int>* allStatusDrsAvailable = nullptr);

    // Fastest-lap tracking, fed from the row stream ("fastest_lap" /
    // "session_history_fastest"). The latter returns true when the fastest
    // holder changed — the caller marks the timing table dirty on that.
    void noteFastestLap(int carIdx);
    bool noteSessionHistoryFastest(int carIdx, int bestMs);
    void resetForNewSession();
    void showLayoutEditor();
    void setTableDensity(tnr::DensityMode mode);
    void setCardDensity(int card, tnr::DensityMode mode);
    // Playback driver changes select the same timing row the Electron header
    // selects; -1 restores the ordinary player-following state.
    void selectDriver(int driverIndex);

protected:
    void resizeEvent(QResizeEvent* event) override;

signals:
    // A row click changed the selection; the owner re-feeds the cached rows
    // through the update methods above.
    void refreshRequested();

private:
    // Electron's sector display: when a car completes a lap its S1/S2 and the
    // derived S3 stay on screen for 7 s; S3 is otherwise blank (it is only
    // known once the lap closes). One tracker per car.
    struct SectorTrack {
        int lapNum = -1, s1 = 0, s2 = 0;       // previous row
        int snapLap = -1, snapS1 = 0, snapS2 = 0; // S1/S2 captured on entering sector 3
        int frozenS1 = 0, frozenS2 = 0, frozenS3 = 0;
        qint64 frozenUntilMs = 0;
        bool seen = false;
        bool frozen(qint64 now) const { return now < frozenUntilMs; }
    };
    static void trackSectors(SectorTrack& t, int lapNum, int sector, int s1, int s2,
                             int lastLapMs, qint64 now);
    void showPlaceholderRows();   // Electron's P1–P20 skeleton before timing arrives

    float contrastThreshold() const { return settings_.value("ui/contrastThreshold", 1.75f).toFloat(); }
    QWidget* buildRacePanel();
    void rebuildRacePanel();
    void applyLayout();
    void updateSidebarWidth();

    bool showTimingTower_ = true;
    bool showCards_[3] = {true, true, true};
    tnr::DensityMode tableDensity_ = tnr::DensityMode::Normal;
    tnr::DensityMode cardDensity_[3] = {tnr::DensityMode::Normal,
        tnr::DensityMode::Normal, tnr::DensityMode::Normal};
    int sidebarPct_ = 28;
    QWidget* sidebar_ = nullptr;
    QWidget* sidebarDivider_ = nullptr;
    QWidget* cards_[3] = {};
    QWidget* cardDividers_[2] = {};

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
    std::unordered_map<int, SectorTrack> tableSectors_;   // car idx → tower sectors
    SectorTrack playerSectors_;                           // race panel (player's lap row)

    // ── Race panel (right side) ───────────────────────────────────
    QLabel*       rp_driverName = nullptr;
    QLabel*       rp_lapNum     = nullptr;
    QLabel*       rp_position   = nullptr;
    QLabel*       rp_pitStatus  = nullptr;
    QLabel*       rp_currentLap = nullptr;
    QLabel*       rp_lastLap    = nullptr;
    QLabel*       rp_s1         = nullptr;
    QLabel*       rp_s2         = nullptr;
    QLabel*       rp_s3         = nullptr;
    QProgressBar* rp_ersBar     = nullptr;
    QLabel*       rp_ersPct     = nullptr;
    QLabel*       rp_ersStore   = nullptr;   // "2.31 MJ / 4.00 MJ" under the bar
    QLabel*       rp_ersMode    = nullptr;
    QLabel*       rp_ersDeployed = nullptr;
    QLabel*       rp_ersHarvested = nullptr; // Spacious only, as Electron
    QLabel*       rp_drsLabel   = nullptr;   // protocol-aware "DRS" caption
    QLabel*       rp_drs        = nullptr;
    QLabel*       rp_fuelKg     = nullptr;
    QLabel*       rp_fuelLaps   = nullptr;
    QLabel*       rp_fuelMix    = nullptr;
    QLabel*       rp_tyre       = nullptr;
    QLabel*       rp_tyreAge    = nullptr;
    QLabel*       rp_brakeBias  = nullptr;

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
