#pragma once

#include <QWidget>
#include <QColor>
#include <QString>
#include <QSettings>
#include <unordered_map>
#include <map>
#include <vector>

#include <tnrp/rows.h>
#include <tnrp/control_rows.h>

class QLabel;
class QProgressBar;
class QStackedWidget;
class QScrollArea;
class QVBoxLayout;

// Circuit pit-lane time loss, read from the track map (assets/maps/track_<id>.json,
// same files used for map rendering). Falls back to 11.25s in-lap / 13.75s out-lap /
// 25s total when no map exists for the circuit.
struct PitLoss {
    double inlapMs  = 11250.0;   // extra time on the in-lap (last lap before the box)
    double outlapMs = 13750.0;   // extra time on the out-lap (first lap after the box)
    double totalMs  = 25000.0;   // full time loss of a stop (used for pit-worth decisions)
};

// One row of a stint's per-lap target table.
struct StrategyLapRow {
    int    lapNum = 0;
    double requiredMs = 0, actualRequiredMs = 0, actualMs = 0;
    double deltaLapMs = 0, deltaStintMs = 0, deltaTotalMs = 0;
    bool   hasActual = false;
};

// A stint captured the moment it begins, so it can be kept on screen after it ends
// (the live plan only carries the current + future stints).
struct PastStint {
    int     startLap = 0;
    double  reqBaseMs = 0;
    QString compoundName;
    QColor  color;
    bool    postPit = false;     // its out-lap carries the boxing-time penalty
    int     expectedLaps = 0;    // planned length, kept live until the stint ends then
                                 // frozen — so a stint boxed early still shows its plan
};

// Everything needed to render one stint card + its per-lap table. Cached in members
// and rebuilt only when the lap counter advances, so rows don't churn each tick.
struct DisplayStint {
    QString compoundName;
    QColor  color;
    int     stintNumber = 0;            // 1-based position in the race (oldest = 1)
    int     startLap = 0, endLap = 0, lapCount = 0;
    int     actualLaps = 0;             // laps actually completed on this set so far
    bool    isLast = false;
    QString wornText;
    bool    targetPresent = false, isEstimate = false, hasDelta = false;
    double  targetMs = 0, deltaMs = 0;
    std::vector<StrategyLapRow> rows;
};

// Race-strategy screen ported from the Electron StrategyPanel. Self-contained:
// MainWindow hands it the cached telemetry JSON rows on each refresh tick and the
// page runs all stint / undercut / wear calculations internally, then rebuilds its
// panels. Styling follows the rest of the native app — palette roles for chrome,
// hard-coded hex only for domain semantics (wear/compound/strategy colours).
class StrategyPage : public QWidget {
    Q_OBJECT
public:
    explicit StrategyPage(QWidget* parent = nullptr);

    // One call with all the typed rows MainWindow already caches. Any of them may
    // be nullptr early in a session — the page degrades gracefully.
    void update(const LapRow* lap,        const tnrp::SessionRow* session,
                const StatusRow* status,  const DamageRow* damage,
                const TimingRow* timing,  const tnrp::ParticipantsRow* participants,
                const tnrp::TyreSetsRow* tyreSets, const AllStatusRow* allStatus,
                const std::map<int, int>& lapTimesByNum);

    // Clears the cross-update tracking (pit counts, latched rivals, gap trend).
    // Called internally on a detected session change and by MainWindow on SSTA.
    void resetForNewSession();

    // Collapse the top header (lap / tyre bar / cliff) to single-line cells.
    // Rebuilds the header in place; MainWindow re-feeds update() to repaint it.
    void setCompactMode(bool on);

private:
    void buildStratHeader();   // (re)build the top header at the current density

    // ── Persistent widgets (built once, refilled on update) ──────────────
    QWidget*        stratHeader_  = nullptr;   // header container, repopulated on compact toggle
    QLabel*         lapValue_     = nullptr;
    QLabel*         lapTotal_     = nullptr;
    QLabel*         compoundChip_ = nullptr;
    QLabel*         wearPct_      = nullptr;
    QLabel*         wearAge_      = nullptr;
    QProgressBar*   wearBar_      = nullptr;
    QLabel*         cliffValue_   = nullptr;
    QLabel*         cliffPlus_    = nullptr;

    QStackedWidget* bodyStack_    = nullptr;   // left pane: 0 = strategy columns, 1 = non-race placeholder
    QScrollArea*    consScroll_   = nullptr;    // conservative column scroll area
    QScrollArea*    aggScroll_    = nullptr;    // aggressive column scroll area
    QVBoxLayout*    consHeaderLayout_ = nullptr; // conservative fixed (non-scrolling) header bar
    QVBoxLayout*    aggHeaderLayout_  = nullptr; // aggressive fixed (non-scrolling) header bar
    QVBoxLayout*    consLayout_   = nullptr;    // conservative column content
    QVBoxLayout*    aggLayout_    = nullptr;    // aggressive column content
    QVBoxLayout*    sidebarLayout_= nullptr;    // right sidebar sections
    QWidget*        sidebarScroll_= nullptr;    // full-height right column (hidden when non-race)
    QWidget*        sidebarSep_   = nullptr;    // divider between left pane and sidebar

    // ── Cross-update tracking (mirrors the React refs/effects) ───────────
    int  rivalAheadIdx_  = -1;
    int  rivalBehindIdx_ = -1;
    bool rivalsLatched_  = false;
    int  prevLapNum_     = -1;

    // Per-lap target table state (persists across the per-tick column rebuild).
    // Actual lap times come from the authoritative SessionModel laps (passed into
    // update()), so the table is correct live and under playback/seeking alike.
    std::map<int, double>   consFrozenReqMs_;      // stint true-start -> frozen Required base (ms)
    std::map<int, double>   aggFrozenReqMs_;       // stint true-start -> frozen Required base (ms)
    std::vector<PastStint>  consPast_;             // every stint that has begun (oldest → newest)
    std::vector<PastStint>  aggPast_;
    // Display stints (completed + current + future) are rebuilt only when the lap
    // counter advances (or on the first valid build), then held until the next lap.
    int                       tableLapComputed_ = -1;
    std::vector<DisplayStint> consDisplay_;
    std::vector<DisplayStint> aggDisplay_;
    // What is actually on screen per column. The stint widgets are reconciled
    // against consDisplay_/aggDisplay_ so only changed stints are rebuilt; these
    // mirror the rendered widgets one-for-one (index = layout position).
    std::vector<DisplayStint> consRendered_;
    std::vector<DisplayStint> aggRendered_;
    // Last-rendered header summary, so the fixed header bar is only rebuilt when
    // the stop count or the Monaco flag changes.
    int                       consStopsShown_ = -1;
    int                       aggStopsShown_  = -1;
    bool                      monacoShown_    = false;
    // The stint columns are heavy (QTableWidgets); rebuild them only when their
    // content changes, not every tick — avoids scrollbar flicker / clumping. Once a
    // valid strategy has shown, keep it on screen (never revert to "Waiting…").
    bool                      stratBuilt_          = false;  // any column content built
    bool                      stratShowingTimeline_ = false; // timeline vs waiting placeholder
    bool                      everStratValid_      = false;  // a valid strategy has been shown

    bool   hasPrevAheadGap_  = false;
    bool   hasPrevBehindGap_ = false;
    double prevAheadGap_     = 0.0;
    double prevBehindGap_    = 0.0;
    int    playerGainingAhead_ = -1;   // -1 unknown, 0 losing, 1 gaining
    int    behindIsGaining_    = -1;

    std::string lastSessionKey_;

    // Circuit pit-loss, loaded once per track from the map file (cached by track id).
    PitLoss pitLoss_;
    int     pitLossTrackId_ = -2;

    bool      compact_ = false;   // single-line header cells when true (ui/compact/strategySummary)
    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
