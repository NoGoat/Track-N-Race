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
#include "components/MiscLayout.h"

class TelemetryChart;
class GearChart;
class InputsChart;
class SteeringChart;
class GForceChart;
class RideHeightChart;
class TyreCardsWidget;
class TyreChartsWidget;
class TnrdPlayer;
class TrackMapWidget;
class SessionModel;
class Toast;
class QComboBox;
class QTimer;
class QToolBar;
class QAction;
class QButtonGroup;
class QToolButton;
class QMenu;
struct ToastSpec;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Overview "Edit Layout" dialog reads/writes through these — the dialog
    // itself doesn't get access to the underlying QFrame* card pointers.
    OverviewLayout loadOverviewLayout();
    void applyAndSaveOverviewLayout(const OverviewLayout& layout);
    OverviewLayout::TyreView currentTyreView();
    void setTyreView(OverviewLayout::TyreView v);

    InputLayout loadInputLayout();
    void applyAndSaveInputLayout(const InputLayout& layout);

    PowerLayout loadPowerLayout();
    void applyAndSavePowerLayout(const PowerLayout& layout);

    MiscLayout loadMiscLayout();
    void applyAndSaveMiscLayout(const MiscLayout& layout);

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
    bool    trackMapSectorColors() const { return settings.value("ui/trackMapSectorColors", true).toBool(); }
    void    setTrackMapSectorColors(bool on);
    int     trackMapOpacity() const { return settings.value("ui/trackMapOpacity", 100).toInt(); }
    void    setTrackMapOpacity(int pct);
    int     trackMapIdleTimeout() const { return settings.value("ui/trackMapIdleTimeout", 0).toInt(); }
    void    setTrackMapIdleTimeout(int secs);
    bool    toastsEnabled() const { return settings.value("ui/toastsEnabled", true).toBool(); }
    void    setToastsEnabled(bool on) { settings.setValue("ui/toastsEnabled", on); }
    int     toastDurationSecs() const { return settings.value("ui/bannerDuration", 3).toInt(); }
    void    setToastDurationSecs(int s) { settings.setValue("ui/bannerDuration", s); }

signals:
    void telemetryUpdated(float speed, int rpm, int gear,
                          float throttle, float brake, float steering, bool drs, int engineTemp);
    void statusUpdated(float ersPct, int ersMode, float fuelKg,
                       float fuelLaps, int tyreCompound, int tyreAgeLaps,
                       int fuelMix, int visualCompound);
    void damageUpdated(int tyreFl, int tyreFr, int tyreRl, int tyreRr,
                       int brakeFl, int brakeFr, int brakeRl, int brakeRr,
                       int wingFl, int wingFr, int wingRear,
                       int floor, int sidepod, int diffuser, int gearbox, int engine,
                       int drsFault, int ersFault);
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
    QLabel*         cardEngine   = nullptr;
    QLabel*         cardErs      = nullptr;
    QLabel*         cardFuel     = nullptr;
    QLabel*         cardPos      = nullptr;
    QLabel*         cardTyre     = nullptr;
    // Top-right heading-row sub-labels (e.g. ERS mode, "+4.2 vs fin", "Lap 1").
    QLabel*         cardDrsSub   = nullptr;
    QLabel*         cardErsSub   = nullptr;
    QLabel*         cardFuelSub  = nullptr;
    QLabel*         cardPosSub   = nullptr;
    QLabel*         cardTyreSub  = nullptr;
    // The ERS sub shows the deploy mode, but a fault (from the damage packet)
    // overrides it. Both arrive on separate packets, so latch each and recompute.
    int             ovErsMode_   = -1;
    bool            ovErsFault_  = false;
    void            refreshErsSub();
    TelemetryChart* chart        = nullptr;
    SessionModel*   model_       = nullptr;
    QComboBox*      ov_lapCombo_ = nullptr;   // compare-lap selector (Overview)
    QPushButton*    ov_compareBtn_ = nullptr; // enabled only while a file is loaded
    QPushButton*    ov_defaultBtn_ = nullptr; // re-selected when a recording closes
    QWidget*        ov_modeBar_  = nullptr;   // chart-mode row; hidden together with the chart
    QFrame*         ov_statsFrame_ = nullptr; // stats row container; hidden if all stat cards are hidden
    QFrame*         ov_sep1_     = nullptr;   // separator below the stats row
    QFrame*         ov_sep2_     = nullptr;   // separator above the damage rows
    QFrame*           ov_dmgFrame_ = nullptr;   // damage rows container; hidden if both rows are hidden
    QFrame*           ov_dmgRowA_  = nullptr;   // tyre/brake damage row
    QFrame*           ov_dmgRowB_  = nullptr;   // wing/body damage row
    QFrame*           ov_dmgHdiv_  = nullptr;   // separator between the two damage rows
    // Tyre section
    QFrame*           ov_tyreSep_    = nullptr;
    TyreCardsWidget*  ov_tyreCards_  = nullptr;
    TyreChartsWidget* ov_tyreCharts_ = nullptr;
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
    struct RowContrastColors {
        QColor normal;
        QColor highlighted;
        QColor fastestLap;
    };
    std::vector<int> tableRowCarIdx;             // row index → car idx
    std::vector<RowContrastColors> rowSafeColors; // cached contrast colors per row
    float            lastContrastThreshold = -1.0f;
    nlohmann::json   lastTimingData;
    nlohmann::json   lastParticipantsData;
    nlohmann::json   lastAllStatusData;
    int              fastestLapCarIdx_ = -1;
    bool             fastestLapSet_    = false;
    std::unordered_map<int, int> sessionHistoryBest_;
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
    TyreCardsWidget* tp_tyreCards_    = nullptr;
    QTableWidget*    tp_drySetsTable  = nullptr;
    QTableWidget*    tp_wetSetsTable  = nullptr;
    nlohmann::json   lastPlayerTelemetryData;
    nlohmann::json   lastPlayerDamageData;
    nlohmann::json   lastTyreSetsData;

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

    // ── Misc tab ──────────────────────────────────────────────
    QWidget*      misc_gforceContainer_  = nullptr;
    QWidget*      misc_rideHeightContainer_ = nullptr;
    QFrame*       misc_hdiv_             = nullptr;
    GForceChart*  gforceChart_           = nullptr;
    RideHeightChart* rideHeightChart_    = nullptr;

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

    // ── Toolbar overflow ──────────────────────────────────────────
    // When the window is too narrow to fit everything, low-priority items collapse
    // into tb_overflowBtn_'s menu (window-size segment → icon actions → page tabs,
    // right-to-left). See relayoutToolbar().
    QButtonGroup* tb_pageGroup_   = nullptr;   // exclusive page tabs
    QButtonGroup* tb_windowGroup_ = nullptr;   // exclusive window-size segment
    QWidget*      tb_windowSeg_   = nullptr;   // window-size segmented control widget
    QAction*      tb_windowAct_   = nullptr;   // its toolbar action (hide to free space)
    QToolButton*  tb_overflowBtn_ = nullptr;   // "⋯" menu button (hidden until needed)
    QAction*      tb_overflowAct_ = nullptr;   // its toolbar action (toggle visibility)
    QMenu*        tb_overflowMenu_= nullptr;
    QWidget*      tb_extButton_   = nullptr;   // Qt's native QToolBar extension — kept hidden
    std::vector<class QToolButton*> tb_pageButtons_;   // the 7 page-tab buttons
    int           tb_windowIdx_   = 1;         // selected window-size option (default 30s)
    void applyChartWindow(int idx);            // set chart window + sync inline/menu state
    void relayoutToolbar();                    // collapse/expand on resize

    // ── Session timer ─────────────────────────────────────────────
    // Elapsed session time (header session_time), formatted M:SS, shown in the
    // toolbar. Lives inside the expanding spacer so it never overflows. Hidden
    // until the first packet with a session_time arrives.
    QLabel*       tb_timerLabel_  = nullptr;
    int           tb_timerSec_    = -1;        // last shown whole second (skip redundant sets)
    int           tb_timerW_      = 0;         // last reserved label width (re-layout on change)
    void updateSessionTimer(float sessionTime);
    void resetSessionTimer();

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
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    // Keeps Qt's native toolbar extension button (tb_extButton_) hidden — we do
    // our own overflow via the ⋯ menu.
    bool eventFilter(QObject* obj, QEvent* e) override;
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
    QWidget* buildMiscPage();
    void     saveOverviewLayout(const OverviewLayout& layout);
    void     applyOverviewLayout(const OverviewLayout& layout);
    void     saveInputLayout(const InputLayout& layout);
    void     applyInputLayout(const InputLayout& layout);
    void     savePowerLayout(const PowerLayout& layout);
    void     applyPowerLayout(const PowerLayout& layout);
    void     saveMiscLayout(const MiscLayout& layout);
    void     applyMiscLayout(const MiscLayout& layout);
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

    // ── Rendering gate (pause all UI work when the window isn't displayed) ──
    // Recording (UDP → parse → .tnrd) is independent of these and keeps running.
    bool renderingActive_   = true;
    bool windowFilterHooked_ = false;   // installed the QWindow expose filter yet?
    void updateRenderingState();        // recompute desired state from window flags
    void setRenderingActive(bool on);   // start/stop the rendering subsystems

    // ── Recording helpers ─────────────────────────────────────────
    void startNewStream(int trackId, int sessionType, int format);
    void closeActiveStream();
    void flushBufferToDisk(const std::vector<BufferEntry>& entries);
    void flushOldBufferEntries();
    void truncateTimeline(float newSessionTime);
    void processPacket(const uint8_t* data, int length);
    void recordRow(const nlohmann::json& row, float sessionTime);
    void resetFastestLapState();
    void emitLiveData(const nlohmann::json& row);

    // Event toast notifications (vendored qt-toast). showToast() builds and shows
    // one transient popup, themed + gated by settings; lastSafetyCarStatus_ tracks
    // the session packet's SC state so changes can be toasted.
    void   showToast(const ToastSpec& spec);
    Toast* m_persistentToast_ = nullptr; // single persistent slot; null when empty
    int    lastSafetyCarStatus_ = 0;
    // Set when the player seeks: the safety-car snapshot that follows resyncs
    // lastSafetyCarStatus_ without toasting (a jump isn't a live SC change).
    bool scSuppressOnce_ = false;
    void ingestForModel(const nlohmann::json& row, float sessionTime);
    bool isDuplicate(const std::string& type, const nlohmann::json& row);

    gzFile gzOpenPath(const QString& path, const char* mode);

    static std::string getISOTimestamp();
    static std::string getFilenameTimestamp();
    static std::string sanitizeName(const std::string& name);
};
