#pragma once

#include <QWidget>
#include <QHash>
#include <QSettings>

#include <tnrp/rows.h>
#include <tnrp/control_rows.h>

#include "SessionLayout.h"

#include <vector>

class QLabel;
class QListWidget;
class TrackMapWidget;

// Session tab — GP header + marshal strip, session stat cards, live track map
// with weather strip, and the proximity/events sidebar. Self-contained: owns
// its widgets, the event log and the track map; MainWindow feeds it the cached
// JSON rows via the update methods from the coalesced refresh.
class SessionPage : public QWidget {
    Q_OBJECT

public:
    explicit SessionPage(QWidget* parent = nullptr);

    SessionLayout loadLayout();
    void saveLayout(const SessionLayout& layout);
    void applyLayout(const SessionLayout& layout);
    void applyAndSaveLayout(const SessionLayout& layout);

    // Cached rows fed by MainWindow; nullptr = the row hasn't been seen yet.
    void updateSession(const tnrp::SessionRow* session, const TimingRow* timing);
    void updateEvents(const tnrp::ParticipantsRow* participants);   // rebuild list from the internal log
    void updateProximity(const TimingRow* timing, const tnrp::ParticipantsRow* participants);
    void updateTrackMap(const tnrp::SessionRow* session,
                        const tnrp::ParticipantsRow* participants,
                        const PositionsRow* positions);

    // Event log maintenance, fed from the race_event row stream.
    void addEvent(const tnrp::RaceEventRow& eventRow);
    void clearEvents();   // SSTA / new session

    // Rendering gate pass-through for the map's 60fps animation timer.
    void setRenderingActive(bool on);

    // The Settings dialog's map setters push live values through this.
    TrackMapWidget* trackMap() const { return trackMap_; }

    // Per-section compact density (each rebuilds its part in place; MainWindow
    // re-feeds the latest session row to repaint). Driven independently by Settings.
    void setCardsCompact(bool on);
    void setWeatherCompactLevel(int level);
    void setHeaderCompact(bool on) { setHeaderCompactLevel(on ? 1 : 0); }
    void setHeaderCompactLevel(int level);
    void setEventsCompact(bool on);
    void setProximityCompact(bool on);

private:
    void buildHeader();         // (re)populate the top header at the current density
    void buildSessionCards();   // (re)populate the stat-card row at the current density
    void buildWeatherStrip();   // (re)populate the bottom weather strip at the current density

    QWidget*     sp_header_       = nullptr;   // header container, repopulated on compact toggle
    QWidget*     sp_marshalStrip  = nullptr;
    QLabel*      sp_gpName        = nullptr;
    QLabel*      sp_circuitName   = nullptr;   // null in compact mode (circuit name dropped)
    QLabel*      sp_timeLeft      = nullptr;
    // Session stat cards registered by key (value labels also aliased below).
    QWidget*     spStatsRow_      = nullptr;   // stat-card row container, repopulated on compact toggle
    QWidget*     sp_weatherStrip_ = nullptr;   // weather strip container, repopulated on compact toggle
    bool         cardsCompact_    = false;     // per-section compact density (ui/compact/session*)
    int          weatherCompactLevel_ = 0;      // 0 Normal, 1 Compact 1, 2 Compact 2, 3 Compact 3
    int          headerCompactLevel_  = 0;      // 0 Normal, 1 Compact 1, 2 Compact 2
    bool         eventsCompact_   = false;
    QLabel*      sp_eventsHeader  = nullptr;
    bool         proximityCompact_ = false;
    QLabel*      sp_proxHeader    = nullptr;
    QHash<QString, QLabel*> spCardValue_;
    QLabel*      sp_statTotalLaps = nullptr;
    QLabel*      sp_statRemain    = nullptr;
    QLabel*      sp_statPitSpeed  = nullptr;
    QLabel*      sp_statPitWin    = nullptr;
    QLabel*      sp_statRejoin    = nullptr;
    QLabel*      sp_trackTemp     = nullptr;
    QLabel*      sp_airTemp       = nullptr;
    QLabel*      sp_trackLen      = nullptr;
    QLabel*      sp_timeOfDay     = nullptr;
    QLabel*      sp_weatherNow    = nullptr;
    QLabel*      sp_weatherNowIcon = nullptr;
    QLabel*      sp_fcTime[5]     = {};
    QLabel*      sp_fcIcon[5]     = {};
    QLabel*      sp_fcWeather[5]  = {};
    QLabel*      sp_fcRain[5]     = {};
    QLabel*      sp_proxPos[3]    = {};
    QLabel*      sp_proxName[3]   = {};
    QLabel*      sp_proxGap[3]    = {};
    QWidget*     sp_proxRow[3]    = {};
    QListWidget* sp_eventsList    = nullptr;
    std::vector<tnrp::RaceEventRow> eventLog_;

    TrackMapWidget* trackMap_   = nullptr;
    int             mapTrackId_ = -1;   // last track loaded into the map widget

    SessionLayout layout_;
    QWidget* sp_headerSep_ = nullptr;
    QWidget* sp_gpBlock_ = nullptr;
    QWidget* sp_zoneBlock_ = nullptr;
    QWidget* sp_tmBlock_ = nullptr;
    QWidget* sp_headerDiv1_ = nullptr;
    QWidget* sp_headerDiv2_ = nullptr;
    QWidget* sp_statsSep_ = nullptr;
    QWidget* sp_statCardFrames_[SessionLayout::StatCardCount] = {};
    QWidget* sp_statCardDivs_[SessionLayout::StatCardCount - 1] = {};
    QWidget* leftArea_ = nullptr;
    QWidget* sp_weatherSep_ = nullptr;
    QWidget* midVLine_ = nullptr;
    QWidget* rightPanel_ = nullptr;
    QWidget* sp_proxSep_ = nullptr;

    // Fullscreen map: hide every sibling of the map so it fills the session view,
    // mirroring the Electron app's maximised map. Toggled from the map's overlay
    // button; mapFsHide_ holds the widgets (header, stats, weather, sidebar and
    // their separators) that are hidden while fullscreen.
    void setMapFullscreen(bool on);
    std::vector<QWidget*> mapFsHide_;
    bool                  mapFullscreen_ = false;

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
