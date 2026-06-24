#pragma once

#include <QWidget>
#include <unordered_map>

#include <nlohmann/json.hpp>

class QLabel;
class QProgressBar;
class QStackedWidget;
class QScrollArea;
class QVBoxLayout;

// Race-strategy screen ported from the Electron StrategyPanel. Self-contained:
// MainWindow hands it the cached telemetry JSON rows on each refresh tick and the
// page runs all stint / undercut / wear calculations internally, then rebuilds its
// panels. Styling follows the rest of the native app — palette roles for chrome,
// hard-coded hex only for domain semantics (wear/compound/strategy colours).
class StrategyPage : public QWidget {
    Q_OBJECT
public:
    explicit StrategyPage(QWidget* parent = nullptr);

    // One call with all the JSON rows MainWindow already caches. Any of them may be
    // an empty object early in a session — the page degrades gracefully.
    void update(const nlohmann::json& lap,      const nlohmann::json& session,
                const nlohmann::json& status,   const nlohmann::json& damage,
                const nlohmann::json& timing,    const nlohmann::json& participants,
                const nlohmann::json& tyreSets,  const nlohmann::json& allStatus);

    // Clears the cross-update tracking (pit counts, latched rivals, gap trend).
    // Called internally on a detected session change and by MainWindow on SSTA.
    void resetForNewSession();

private:
    // ── Persistent widgets (built once, refilled on update) ──────────────
    QLabel*         lapValue_     = nullptr;
    QLabel*         lapTotal_     = nullptr;
    QLabel*         compoundChip_ = nullptr;
    QLabel*         wearPct_      = nullptr;
    QLabel*         wearAge_      = nullptr;
    QProgressBar*   wearBar_      = nullptr;
    QLabel*         cliffValue_   = nullptr;
    QLabel*         cliffPlus_    = nullptr;

    QStackedWidget* bodyStack_    = nullptr;   // left pane: 0 = strategy columns, 1 = non-race placeholder
    QVBoxLayout*    consLayout_   = nullptr;    // conservative column content
    QVBoxLayout*    aggLayout_    = nullptr;    // aggressive column content
    QVBoxLayout*    sidebarLayout_= nullptr;    // right sidebar sections
    QWidget*        sidebarScroll_= nullptr;    // full-height right column (hidden when non-race)
    QWidget*        sidebarSep_   = nullptr;    // divider between left pane and sidebar

    // ── Cross-update tracking (mirrors the React refs/effects) ───────────
    std::unordered_map<int, int> pitCounts_;        // car idx → completed stops
    std::unordered_map<int, int> prevDriverStatus_; // car idx → last driver_status

    int  rivalAheadIdx_  = -1;
    int  rivalBehindIdx_ = -1;
    bool rivalsLatched_  = false;
    int  prevLapNum_     = -1;

    bool   hasPrevAheadGap_  = false;
    bool   hasPrevBehindGap_ = false;
    double prevAheadGap_     = 0.0;
    double prevBehindGap_    = 0.0;
    int    playerGainingAhead_ = -1;   // -1 unknown, 0 losing, 1 gaining
    int    behindIsGaining_    = -1;

    std::string lastSessionKey_;
};
