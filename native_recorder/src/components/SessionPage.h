#pragma once

#include <QWidget>
#include <QHash>
#include <QSettings>

#include <nlohmann/json.hpp>

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

    void updateSession(const nlohmann::json& session, const nlohmann::json& timing);
    void updateEvents(const nlohmann::json& participants);   // rebuild list from the internal log
    void updateProximity(const nlohmann::json& timing, const nlohmann::json& participants);
    void updateTrackMap(const nlohmann::json& session,
                        const nlohmann::json& participants,
                        const nlohmann::json& positions);

    // Event log maintenance, fed from the race_event row stream.
    void addEvent(const nlohmann::json& eventRow);
    void clearEvents();   // SSTA / new session

    // Rendering gate pass-through for the map's 60fps animation timer.
    void setRenderingActive(bool on);

    // The Settings dialog's map setters push live values through this.
    TrackMapWidget* trackMap() const { return trackMap_; }

    // Per-section compact density (each rebuilds its part in place; MainWindow
    // re-feeds the latest session row to repaint). Driven independently by Settings.
    void setCardsCompact(bool on);
    void setWeatherCompact(bool on);
    void setHeaderCompact(bool on);

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
    bool         weatherCompact_  = false;
    bool         headerCompact_   = false;
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
    std::vector<nlohmann::json> eventLog_;

    TrackMapWidget* trackMap_   = nullptr;
    int             mapTrackId_ = -1;   // last track loaded into the map widget

    // Fullscreen map: hide every sibling of the map so it fills the session view,
    // mirroring the Electron app's maximised map. Toggled from the map's overlay
    // button; mapFsHide_ holds the widgets (header, stats, weather, sidebar and
    // their separators) that are hidden while fullscreen.
    void setMapFullscreen(bool on);
    std::vector<QWidget*> mapFsHide_;
    bool                  mapFullscreen_ = false;

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
