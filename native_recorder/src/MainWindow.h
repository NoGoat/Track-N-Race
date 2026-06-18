#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QProgressBar>
#include <QScrollArea>
#include <QListWidget>
#include <QSlider>
#include <QComboBox>
#include <QFrame>
#include <QUdpSocket>
#include <QSettings>

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <limits>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include "components/OverviewLayout.h"
#include "components/InputLayout.h"
#include "components/PowerLayout.h"

class TelemetryChart;
class GearChart;
class InputsChart;
class SteeringChart;
class TnrdPlayer;
class TrackMapWidget;
class SessionModel;
class QComboBox;
class QTimer;
class QToolBar;
class QAction;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Overview "Edit Layout" dialog reads/writes through these — the dialog
    // itself doesn't get access to the underlying QFrame* card pointers.
    OverviewLayout loadOverviewLayout();
    void applyAndSaveOverviewLayout(const OverviewLayout& layout);

    InputLayout loadInputLayout();
    void applyAndSaveInputLayout(const InputLayout& layout);

    PowerLayout loadPowerLayout();
    void applyAndSavePowerLayout(const PowerLayout& layout);

    // Settings dialog reads/writes through these — it owns its own widgets
    // and persists immediately on every change, same immediate-apply pattern
    // as the Overview layout dialog above.
    QString currentOutputDirectory() const { return outputDirectory; }
    void    setOutputDirectory(const QString& dir);
    bool    autoRecordEnabled() const { return wantRecord; }
    void    setAutoRecord(bool checked);
    QString currentTheme() const { return settings.value("theme", "system").toString(); }
    void    setTheme(const QString& theme);
    QString currentStyleName() const { return settings.value("style", "system").toString(); }
    void    setStyleName(const QString& name);
    bool    toolbarLabelsEnabled() const { return settings.value("ui/toolbarShowLabels", false).toBool(); }
    void    setToolbarLabels(bool checked);
    float   contrastThreshold() const { return settings.value("ui/contrastThreshold", 1.75f).toFloat(); }
    void    setContrastThreshold(float val);
    int     trackMapLabelMode() const { return settings.value("ui/trackMapLabelMode", 0).toInt(); }
    void    setTrackMapLabelMode(int mode);

signals:
    void telemetryUpdated(float speed, int rpm, int gear,
                          float throttle, float brake, float steering, bool drs, int engineTemp);
    void statusUpdated(float ersPct, int ersMode, float fuelKg,
                       float fuelLaps, int tyreCompound, int tyreAgeLaps);
    void damageUpdated(int tyreFl, int tyreFr, int tyreRl, int tyreRr,
                       int brakeFl, int brakeFr, int brakeRl, int brakeRr,
                       int wingFl, int wingFr, int wingRear,
                       int floor, int sidepod, int diffuser, int gearbox, int engine);
    void lapUpdated(int position, int lapNum);

private slots:
    void onDatagramReady();

private:
    // ── Overview tab ──────────────────────────────────────────────
    QLabel*         cardSpeed    = nullptr;
    QLabel*         cardRpm      = nullptr;
    QLabel*         cardGear     = nullptr;
    QLabel*         cardThrottle = nullptr;
    QLabel*         cardBrake    = nullptr;
    QLabel*         cardDrs      = nullptr;
    QLabel*         cardErs      = nullptr;
    QLabel*         cardPos      = nullptr;
    TelemetryChart* chart        = nullptr;
    SessionModel*   model_       = nullptr;
    QComboBox*      ov_lapCombo_ = nullptr;   // compare-lap selector (Overview)
    QPushButton*    ov_compareBtn_ = nullptr; // enabled only while a file is loaded
    QPushButton*    ov_defaultBtn_ = nullptr; // re-selected when a recording closes
    QWidget*        ov_modeBar_  = nullptr;   // chart-mode row; hidden together with the chart
    QFrame*         ov_statsFrame_ = nullptr; // stats row container; hidden if all stat cards are hidden
    QFrame*         ov_sep1_     = nullptr;   // separator below the stats row
    QFrame*         ov_sep2_     = nullptr;   // separator above the damage rows
    QFrame*         ov_dmgFrame_ = nullptr;   // damage rows container; hidden if both rows are hidden
    QFrame*         ov_dmgRowA_  = nullptr;   // tyre/brake damage row
    QFrame*         ov_dmgRowB_  = nullptr;   // wing/body damage row
    QFrame*         ov_dmgHdiv_  = nullptr;   // separator between the two damage rows
    QFrame*         ov_statCardFrame_[OverviewLayout::StatCardCount] = {};
    QFrame*         ov_dmgCardFrame_[OverviewLayout::DmgCardCount]   = {};
    QLabel*         dmgTyreFl   = nullptr; QLabel* dmgTyreFr  = nullptr;
    QLabel*         dmgTyreRl   = nullptr; QLabel* dmgTyreRr  = nullptr;
    QLabel*         dmgBrakeFl  = nullptr; QLabel* dmgBrakeFr = nullptr;
    QLabel*         dmgBrakeRl  = nullptr; QLabel* dmgBrakeRr = nullptr;
    QLabel*         dmgWingFl   = nullptr; QLabel* dmgWingFr  = nullptr;
    QLabel*         dmgWingRear = nullptr; QLabel* dmgFloor   = nullptr;
    QLabel*         dmgSidepod  = nullptr; QLabel* dmgDiffuser = nullptr;
    QLabel*         dmgGearbox  = nullptr; QLabel* dmgEngine   = nullptr;

    // ── Standings page ────────────────────────────────────────────
    QTableWidget*    timingTable         = nullptr;
    int              selectedCarIdx      = -1;   // -1 = no selection (show player)
    std::vector<int> tableRowCarIdx;             // row index → car idx
    std::vector<QColor> rowSafeColors;           // cached contrast colors per row
    float            lastContrastThreshold = -1.0f;
    nlohmann::json   lastTimingData;
    nlohmann::json   lastParticipantsData;
    nlohmann::json   lastAllStatusData;
    nlohmann::json   lastPlayerLapData;
    nlohmann::json   lastPlayerStatusData;

    // ── Race panel (right side of Standings) ──────────────────────
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

    // ── Tyres page ───────────────────────────────────────────────
    QLabel*       tp_surfaceTemp[4] = {};  // FL, FR, RL, RR
    QLabel*       tp_innerTemp[4]   = {};
    QLabel*       tp_brakeTemp[4]   = {};
    QLabel*       tp_wearLabel[4]   = {};  // "34%" text next to "Wear" label
    QProgressBar* tp_wear[4]        = {};  // colored fill bar
    QLabel*       tp_blisters[4]    = {};
    QTableWidget* tp_drySetsTable   = nullptr;
    QTableWidget* tp_wetSetsTable   = nullptr;
    nlohmann::json lastPlayerTelemetryData;
    nlohmann::json lastPlayerDamageData;
    nlohmann::json lastTyreSetsData;

    // ── Session page ──────────────────────────────────────────────
    QWidget*     sp_marshalStrip  = nullptr;
    QLabel*      sp_gpName        = nullptr;
    QLabel*      sp_circuitName   = nullptr;
    QLabel*      sp_sessionType   = nullptr;
    QLabel*      sp_timeLeft      = nullptr;
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
    QLabel*      sp_fcTime[5]     = {};
    QLabel*      sp_fcWeather[5]  = {};
    QLabel*      sp_fcRain[5]     = {};
    QLabel*      sp_proxPos[3]    = {};
    QLabel*      sp_proxName[3]   = {};
    QLabel*      sp_proxGap[3]    = {};
    QWidget*     sp_proxRow[3]    = {};
    QListWidget* sp_eventsList    = nullptr;
    nlohmann::json              lastSessionData;
    std::vector<nlohmann::json> sessionEventLog;

    // ── Track map page ────────────────────────────────────────────
    TrackMapWidget* trackMap_      = nullptr;
    nlohmann::json  lastPositionsData;
    int             mapTrackId_    = -1;   // last track loaded into the map widget

    // ── Input tab ──────────────────────────────────────────────
    QWidget*      ip_topRow_         = nullptr;
    QWidget*      ip_gearContainer_  = nullptr;
    QWidget*      ip_inputsContainer_= nullptr;
    QFrame*       ip_vdiv_           = nullptr;
    QWidget*      ip_steeringContainer_= nullptr;
    QFrame*       ip_hdiv_           = nullptr;
    GearChart*    gearChart_         = nullptr;
    InputsChart*  inputsChart_       = nullptr;
    SteeringChart* steeringChart_    = nullptr;

    // ── Power page ─────────────────────────────────────────────
    QLabel* pp_totalPowerVal = nullptr;
    QLabel* pp_iceVal        = nullptr;
    QLabel* pp_mgukVal       = nullptr;
    QLabel* pp_splitVal      = nullptr;
    QLabel* pp_ersStoreVal   = nullptr;
    QLabel* pp_ersPctVal     = nullptr;
    QLabel* pp_fuelVal       = nullptr;
    QWidget* pp_topBar_      = nullptr;
    QWidget* pp_cardFrames_[PowerLayout::CardCount] = {};
    QFrame*  pp_cardDivs_[PowerLayout::CardCount - 1] = {};
    QFrame*  pp_hdiv_        = nullptr;
    QWidget* pp_splitContainer_ = nullptr;
    QWidget* pp_harvContainer_ = nullptr;
    QWidget* pp_storeContainer_ = nullptr;
    QWidget* pp_fuelContainer_ = nullptr;
    QWidget* pp_topChartsRow_ = nullptr;
    QWidget* pp_bottomChartsRow_ = nullptr;
    QFrame*  pp_vline_ = nullptr;
    QFrame*  pp_hline1_ = nullptr;
    QFrame*  pp_hline2_ = nullptr;
    class PowerChart* pp_splitChart  = nullptr;
    class PowerChart* pp_harvestChart= nullptr;
    class PowerChart* pp_storeChart  = nullptr;
    class PowerChart* pp_fuelChart   = nullptr;

    // ── Dialogs ───────────────────────────────────────────────────
    QToolBar* toolbar_ = nullptr;   // referenced by the Settings dialog's label toggle
    // Theme-icon actions kept so their (tinted) icons can be rebuilt when the
    // palette changes at runtime — see changeEvent()/refreshThemedIcons().
    QAction*  openAct_       = nullptr;
    QAction*  editLayoutAct_ = nullptr;
    QAction*  settingsAct_   = nullptr;

    // ── Playback ──────────────────────────────────────────────────
    TnrdPlayer*  player_         = nullptr;
    bool         inPlayback_     = false;
    bool         seekerUpdating_ = false;
    bool         pbLastPlaying_  = false;   // only swap the play/pause icon on change
    QWidget*     pb_bar_         = nullptr;
    QFrame*      pb_sep_         = nullptr;
    QPushButton* pb_seekBackBtn_ = nullptr;
    QPushButton* pb_playBtn_     = nullptr;
    QPushButton* pb_seekFwdBtn_  = nullptr;
    QSlider*     pb_slider_      = nullptr;
    QLabel*      pb_timeLabel_   = nullptr;
    QComboBox*   pb_speedCombo_  = nullptr;
    QComboBox*   pb_lapCombo_    = nullptr;
    QWidget*     container_      = nullptr;
    QWidget*     loadingOverlay_ = nullptr;

    // ── UDP / network ─────────────────────────────────────────────
    QUdpSocket* udpSocket = nullptr;

    // ── Persistence ───────────────────────────────────────────────
    QSettings settings{ "TrackNRace", "NativeRecorder" };

    // ── Recording state ───────────────────────────────────────────
    bool    wantRecord        = false;
    QString outputDirectory;
    gzFile  activeGzip        = nullptr;
    int     currentTrackId    = -1;
    int     currentSessionType = -1;
    QString activeGzipPath;
    float   lastSessionTime   = -1.0f;

    struct BufferEntry { std::string line; float sessionTime; };
    std::vector<BufferEntry> rollingBuffer;
    static constexpr float BUFFER_WINDOW_S = 30.0f;

    std::unordered_map<int, uint32_t>            lastFrameId;
    std::unordered_map<int, uint64_t>            lastSlowMs;
    std::unordered_map<std::string, std::string> dedupeCache;

    void resizeEvent(QResizeEvent* e) override;
    // Rebuilds the toolbar's tinted theme icons when the application palette
    // changes (e.g. Light↔Dark), so they re-tint to the new foreground colour.
    void changeEvent(QEvent* e) override;
    void refreshThemedIcons();
    // Forces the toolbar to the app's window colour only when the app's chosen
    // light/dark differs from the OS scheme (otherwise the native Breeze "tools
    // area" colour, taken from the OS scheme, would clash). When they match — or
    // the theme is "system" — the native unified header look is left alone.
    void updateToolbarColorScheme();

    // ── Builders ──────────────────────────────────────────────────
    QWidget* buildOverviewTab();
    QWidget* buildStandingsPage();
    QWidget* buildRacePanel();
    QWidget* buildSessionPage();
    QWidget* buildTyresPage();
    QWidget* buildInputPage();
    QWidget* buildPowerPage();
    void     saveOverviewLayout(const OverviewLayout& layout);
    void     applyOverviewLayout(const OverviewLayout& layout);
    void     saveInputLayout(const InputLayout& layout);
    void     applyInputLayout(const InputLayout& layout);
    void     savePowerLayout(const PowerLayout& layout);
    void     applyPowerLayout(const PowerLayout& layout);
    void     updateTimingTable();
    void     updateRacePanel();
    void     updateSessionPage();
    void     updateSessionEvents();
    void     updateProximityWidget();
    void     updateTyresPage();
    void     updateTyreSetsTable();
    void     updateTrackMapPage();
    void     updatePowerPage();

    // ── Coalesced panel refresh ───────────────────────────────────
    // Packets can arrive in bursts (especially fast playback); rebuilding a
    // panel per packet locks the UI. Each packet only marks its panel dirty and
    // the heavy rebuild runs once per refresh tick.
    QTimer* uiRefreshTimer_ = nullptr;
    int  currentPage_    = 0;   // visible stack index; updaters skip hidden pages
    bool dirtyTiming_    = false;
    bool dirtyRacePanel_ = false;
    bool dirtyTyres_     = false;
    bool dirtyTyreSets_  = false;
    bool dirtySession_   = false;
    bool dirtyEvents_    = false;
    bool dirtyProximity_ = false;
    bool dirtyTrackMap_  = false;
    bool dirtyPower_     = false;
    void scheduleUiRefresh();
    void flushUiRefresh();

    // ── Recording helpers ─────────────────────────────────────────
    void startNewStream(int trackId, int sessionType, int format);
    void closeActiveStream();
    void flushBufferToDisk(const std::vector<BufferEntry>& entries);
    void flushOldBufferEntries();
    void truncateTimeline(float newSessionTime);
    void processPacket(const uint8_t* data, int length);
    void recordRow(const nlohmann::json& row, float sessionTime);
    void emitLiveData(const nlohmann::json& row);
    void ingestForModel(const nlohmann::json& row, float sessionTime);
    bool isDuplicate(const std::string& type, const nlohmann::json& row);

    gzFile gzOpenPath(const QString& path, const char* mode);

    static std::string getISOTimestamp();
    static std::string getFilenameTimestamp();
    static std::string sanitizeName(const std::string& name);
};
