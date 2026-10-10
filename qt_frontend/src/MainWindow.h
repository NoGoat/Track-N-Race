#pragma once

#include <QMainWindow>
#include <QPixmap>
#include <QPointer>
#include <QSettings>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QVector>

#include <string>
#include <cstdint>
#include <memory>
#include <optional>

#include <tnrp/AnyRow.h>
#include <tnrp/RecordingScope.h>

#include "PlaybackPatchMerger.h"
#include "CompactSettings.h"
#include "GraphViewSettings.h"
#include "components/OverviewLayout.h"

class OverviewPage;
class AnalysisPage;
class StandingsPage;
class DriverLapsDialog;
class SessionPage;
class StrategyPage;
class TrendsPage;
class DamagePage;
class TyresPage;
class InputPage;
class PowerPage;
class MiscPage;
class AppToolbar;
class PlaybackController;
class SessionModel;
class EngineSink;
namespace tnrp { class Engine; }
class ToastHost;
class SeekLoadingOverlay;
class QTimer;
class QLabel;
class QProgressBar;
class QThread;
class QMessageBox;
class QJsonObject;

struct UdpForwardTargetSetting {
    QString address;
    int     port = 20777;

    bool operator==(const UdpForwardTargetSetting&) const = default;
};

struct PairDeviceState {
    QString id;
    QString name;
    qint64 pairedAt = 0;
    qint64 lastSeenAt = 0;
    bool connected = false;
};

struct PairServiceState {
    bool enabled = false;
    QString serverId;
    int port = 20779;
    bool pairingOpen = false;
    qint64 pairingExpiresAt = 0;
    QString matchingCode;
    QString qrPayload;
    // A phone that proved the QR or code and waits for this desktop to allow it.
    QString pendingDeviceId;
    QString pendingDeviceName;
    QVector<PairDeviceState> devices;
    QString error;
};

class ScreenshotTour;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // Page tabs / stacked-widget order. Declaration order *is* the tab order and
    // the QStackedWidget index, so inserting a page here (and in the matching
    // AppToolbar page-name list and stack->addWidget() list) renumbers everything
    // for free. PageCount is the tab count — keep it last.
    enum Page { Overview, Analyze, Standings, Session, Tyres, Strategy, Trends, Damage, Input, Power, Misc, PageCount };

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Shared entry point for toolbar, command-line/file-association, and
    // single-instance open requests. External activations deliberately use the
    // same warning and load path as a file selected in the toolbar.
    void offerRecordingFile(const QString& path);

    // Tyre view/graph settings used by the Settings dialog — one-line forwarders
    // to the Overview page, which owns the widgets and persistence.
    OverviewLayout::TyreView currentTyreView();
    void setTyreView(OverviewLayout::TyreView v);
    bool tyreGraphLifeMode() const;
    void setTyreGraphLifeMode(bool life);

    // Settings dialog reads/writes through these. Most controls apply
    // immediately; network controls are the exception and remain a local draft
    // until Apply & Restart is pressed.
    QString currentOutputDirectory() const { return outputDirectory; }
    QString lastDialogDirectory() const;
    void    rememberDialogDirectory(const QString& path, bool isDirectory = false);
    void    setOutputDirectory(const QString& dir);
    bool    autoRecordEnabled() const { return wantRecord; }
    void    setAutoRecord(bool checked);
    // Which drivers a session category records: category is practice,
    // qualifying, race or time_trial; scope is all_drivers, driver_only, both
    // or ask (tnrp::RecordingScope).
    QString recordingScope(const QString& category) const;
    void    setRecordingScope(const QString& category, const QString& scope);
    QString currentTheme() const { return settings.value("theme", "system").toString(); }
    void    setTheme(const QString& theme);
    QString currentStyleName() const { return settings.value("style", "system").toString(); }
    void    setStyleName(const QString& name);
    bool    toolbarLabelsEnabled() const { return settings.value("ui/toolbarShowLabels", false).toBool(); }
    void    setToolbarLabels(bool checked);
    bool    verticalChartLayout(Page page) const;
    void    setVerticalChartLayout(Page page, bool vertical);
    QString inputPedalLayout() const;
    void    setInputPedalLayout(const QString& layout);
    // Settings ▸ Layout ▸ Trends: "separate", "combined", "combinedNoRecharge" or "bars".
    QString trendsChartLayout() const;
    void    setTrendsChartLayout(const QString& layout);
    bool    miscSplitLayout(bool gForce) const;
    void    setMiscSplitLayout(bool gForce, bool split);
    tnr::DensityMode densitySection(tnr::CompactSection s) const;
    void    setDensitySection(tnr::CompactSection s, tnr::DensityMode mode);
    bool    compactSection(tnr::CompactSection s) const { return densitySection(s) == tnr::DensityMode::Compact; }
    void    setCompactSection(tnr::CompactSection s, bool on);
    int     weatherCompactLevel() const;
    void    setWeatherCompactLevel(int level);
    int     headerCompactLevel() const;
    void    setHeaderCompactLevel(int level);
    // Overview tyre cards use Electron's specialised 0–6 level control.
    int     tyresCompactLevel() const { return qBound(0, settings.value(tnr::compactKey(tnr::CompactSection::OverviewTyres), 0).toInt(), 6); }
    void    setTyresCompactLevel(int level);
    // Per-graph view mode: false = chart (default), true = raw-values table.
    bool    graphView(tnr::GraphSection s) const { return settings.value(tnr::graphViewKey(s), false).toBool(); }
    void    setGraphView(tnr::GraphSection s, bool table);
    bool    chartDynamicYAxis(tnr::GraphSection s) const;
    void    setChartDynamicYAxis(tnr::GraphSection s, bool dynamic);
    bool    chartAnalysisDynamicYAxis(const QString& scaleKey) const;
    void    setChartAnalysisDynamicYAxis(const QString& scaleKey, bool dynamic);
    bool    chartSecondaryVerticalCrosshair() const;
    bool    chartSecondaryHorizontalCrosshair() const;
    void    setChartSecondaryCrosshairs(bool vertical, bool horizontal);
    float   contrastThreshold() const { return settings.value("ui/contrastThreshold", 1.75f).toFloat(); }
    void    setContrastThreshold(float val);
    int     chartMsaaSamples() const { return settings.value("ui/chartMsaaSamples", 4).toInt(); }
    void    setChartMsaaSamples(int samples);
    QString chartGraphicsBackend() const { return settings.value("ui/chartGraphicsBackend", "auto").toString(); }
    void    setChartGraphicsBackend(const QString& backend);
    int     chartFpsInFocus() const { return settings.value("ui/chartFpsInFocus", -1).toInt(); }
    int     chartFpsOutOfFocus() const { return settings.value("ui/chartFpsOutOfFocus", 30).toInt(); }
    void    setChartFpsInFocus(int fps);
    void    setChartFpsOutOfFocus(int fps);
    int     deltaUpdateInterval() const { return settings.value("ui/deltaUpdateInterval", 0).toInt(); }
    void    setDeltaUpdateInterval(int ms);
    bool    reduceAnimations() const { return settings.value("ui/reduceAnimations", false).toBool(); }
    void    setReduceAnimations(bool on);
    int     trackMapLabelMode() const { return settings.value("ui/trackMapLabelMode", 0).toInt(); }
    void    setTrackMapLabelMode(int mode);
    bool    trackMapSectorColors() const { return settings.value("ui/trackMapSectorColors", false).toBool(); }
    void    setTrackMapSectorColors(bool on);
    int     trackMapOpacity() const { return settings.value("ui/trackMapOpacity", 100).toInt(); }
    void    setTrackMapOpacity(int pct);
    int     trackMapIdleTimeout() const { return settings.value("ui/trackMapIdleTimeout", 10).toInt(); }
    void    setTrackMapIdleTimeout(int secs);
    bool    toastsEnabled() const { return settings.value("ui/toastsEnabled", true).toBool(); }
    void    setToastsEnabled(bool on) { settings.setValue("ui/toastsEnabled", on); }
    int     toastDurationSecs() const { return settings.value("ui/bannerDuration", 3).toInt(); }
    void    setToastDurationSecs(int s) { settings.setValue("ui/bannerDuration", s); }
    bool    updateChecksEnabled() const { return settings.value("updates/enabled", true).toBool(); }
    void    setUpdateChecksEnabled(bool on) { settings.setValue("updates/enabled", on); }
    bool    additionalLoggingEnabled() const { return settings.value("debug/additionalLogging", false).toBool(); }
    void    setAdditionalLoggingEnabled(bool on);
    bool    memoryLogEnabled() const { return settings.value("debug/memoryLog", false).toBool(); }
    void    setMemoryLogEnabled(bool on);
    QString currentProtocolOverride() const { return settings.value("protocolOverride", "auto").toString(); }
    void    setProtocolOverride(const QString& ovr);
    QByteArray teamColorCatalogJson() const;
    QByteArray teamColorOverridesJson() const;
    void setTeamColorOverridesJson(const QByteArray& json);
    int     lastDetectedProtocolFormat() const { return lastDetectedProtocolFormat_; }
    int     detectedProtocolWarningFormat() const { return detectedProtocolWarningFormat_; }
    int     forcedProtocolWarningFormat() const { return forcedProtocolWarningFormat_; }
    int     udpPort() const { return settings.value("udp/port", 20777).toInt(); }
    QString udpBindAddress() const { return settings.value("udp/bindAddress", "0.0.0.0").toString(); }
    bool    udpForwardingEnabled() const { return settings.value("udp/forwardingEnabled", false).toBool(); }
    QVector<UdpForwardTargetSetting> udpForwardTargets() const;
    // Persists the normalized draft first, then recreates the host-owned engine
    // so Config::udpForwardTargets takes effect. Returns an empty string on success.
    QString applyUdpConfiguration(int port, const QString& bindAddress,
                                  bool forwardingEnabled,
                                  const QVector<UdpForwardTargetSetting>& targets);
    const PairServiceState& pairServiceState() const { return pairServiceState_; }
    void setPairServiceEnabled(bool enabled);
    void openPairingWindow();
    void closePairingWindow();
    void respondToPairing(bool approve);
    void removePairDevice(const QString& id);

signals:
    void protocolWarningChanged(int detectedFormat, int forcedFormat);
    void pairServiceStateChanged();

private slots:
    // Receives a coalesced JSONL batch (cold/control) from the libtnrp engine
    // (marshalled onto the GUI thread by EngineSink) and parses it into typed
    // tnrp::AnyRow. Hot 60 Hz rows arrive packed via onEngineBinary instead.
    void onEngineRow(const QByteArray& json);
    // Receives one packed hot-row batch (telemetry/motion/motion_ex/positions)
    // and decodes it with tnrp::bin::decodeBatch — no JSON on the live hot path.
    void onEngineBinary(const QByteArray& batch);

private:
    // ── Website screenshots (MainWindowScreenshots.cpp) ───────────
    // F7 saves one capture to Pictures/Track N Race Screenshots. F8 runs the
    // screenshot tour: opens test.tnrd from the AppImage's (or executable's)
    // directory and captures every page at the Electron screenshots' session
    // times into screenshots/ beside it; F8 again cancels. Shift+F7 restores
    // the window and sizes its frame to 1200x700, the Electron window size.
    // On X11 captures are read from the screen, title bar included, at the
    // display's scale; elsewhere the window renders itself at 2x without one.
    friend class ScreenshotTour;
    void captureScreenshot();
    void sizeForScreenshot();
    void toggleScreenshotTour();
    QPixmap grabForScreenshot();
    void showScreenshotNotice(const QString& text, int msecs = 3000);
    QPointer<ScreenshotTour> screenshotTour_;
    QString screenshotNotice_;        // title shown after a capture; empty when none
    QString titleBeforeScreenshot_;   // restored when the notice expires

    // ── Overview tab ──────────────────────────────────────────────
    // Self-contained page widget (stat cards, telemetry chart, tyre section,
    // damage rows); fed rows synchronously via on*() from emitLiveData and
    // playback state via its setters.
    OverviewPage*   overviewPage_ = nullptr;
    AnalysisPage*   analyzePage_  = nullptr;
    SessionModel*   model_        = nullptr;

    // ── Standings page ────────────────────────────────────────────
    // Self-contained page widget (timing table + race panel + selection and
    // fastest-lap state); fed the cached rows below via its update methods.
    StandingsPage*   standingsPage_      = nullptr;
    std::optional<TimingRow>             lastTimingData;
    // The roster merged by car index, as Electron's store keeps it: a
    // participants row can list fewer cars than timing still shows.
    std::optional<tnrp::ParticipantsRow> lastParticipantsData;
    bool newParticipantsRoster_ = true;   // the next participants row starts a new roster
    std::optional<AllStatusRow>          lastAllStatusData;
    std::optional<LapRow>                lastPlayerLapData;
    std::optional<StatusRow>             lastPlayerStatusData;

    // ── Tyres page ───────────────────────────────────────────────
    // Self-contained page widget; fed the latest cached rows via
    // updateTyreCards()/updateTyreSets() from flushUiRefresh().
    TyresPage*       tyresPage_       = nullptr;
    std::optional<TelemetryRow>      lastPlayerTelemetryData;
    std::optional<DamageRow>         lastPlayerDamageData;
    std::optional<tnrp::TyreSetsRow> lastTyreSetsData;
    QHash<int, tnrp::TyreSetsRow>    liveTyreSetsByCar_;   // live: latest sets per car index

    // ── Strategy page ─────────────────────────────────────────────
    // Snapshot-only renderer; libtnrp owns all strategy state and calculations.
    StrategyPage* strategyPage_ = nullptr;
    std::optional<tnrp::StrategySnapshotRow> lastStrategyData;

    // ── Trends / Damage pages ─────────────────────────────────────
    // Both follow the streamed driver (the player live, the driver selector's
    // car in V6 playback). Trends reads the model's status/damage history and
    // the driver lap history the engine pushes while the page claims the car;
    // Damage shows the latest damage row.
    TrendsPage* trendsPage_ = nullptr;
    DamagePage* damagePage_ = nullptr;

    // ── Session page ──────────────────────────────────────────────
    // Self-contained page widget (header, stat cards, track map, proximity,
    // events log); fed the cached rows below via its update methods. Owns the
    // event log and TrackMapWidget (settings setters push via trackMap()).
    SessionPage*   sessionPage_ = nullptr;
    std::optional<tnrp::SessionRow> lastSessionData;
    std::optional<PositionsRow>     lastPositionsData;

    // ── Input tab ──────────────────────────────────────────────
    // Self-contained page widget (charts bound to model_); MainWindow only
    // forwards playback state through its setters.
    InputPage*    inputPage_         = nullptr;

    // ── Misc tab ──────────────────────────────────────────────
    // Self-contained page widget (charts bound to model_); MainWindow only
    // forwards playback state through its setters.
    MiscPage*     miscPage_              = nullptr;

    // ── Power page ────────────────────────────────────────────────
    // Self-contained page widget (stat cards + charts bound to model_);
    // fed the latest status row via update(), playback state via setters.
    PowerPage* powerPage_ = nullptr;

    // ── Toolbar ───────────────────────────────────────────────────
    // Self-contained (page tabs, session timer, chart-window segment, action
    // icons, ⋯ overflow); MainWindow reacts to its signals and forwards the
    // label/color-scheme settings.
    AppToolbar* toolbar_ = nullptr;

    // ── Playback ──────────────────────────────────────────────────
    // Self-contained (engine command facade + transport bar); MainWindow reacts
    // to its entered/exited/timeChanged signals.
    PlaybackController* playback_ = nullptr;
    bool         inPlayback_     = false;
    // Electron's playback seek barrier (bridgeManager.ts). From seekStarted
    // until the matching authoritative history flush arrives, stream rows are
    // from the old cursor and are dropped (waiting-flush). From that flush until
    // the history is installed they belong to the new cursor and are buffered
    // in engine order (waiting-renderer), then replayed on historyInstalled.
    bool         playbackSeekInstalling_ = false;   // either phase is active
    bool         playbackSeekFlushReceived_ = false; // waiting-renderer phase
    struct SeekReplayItem { QByteArray data; bool binary = false; };
    QVector<SeekReplayItem> playbackSeekReplay_;
    qsizetype    playbackSeekReplayBytes_ = 0;
    void bufferSeekReplay(const QByteArray& data, bool binary);
    void resetSeekGate();
    SeekLoadingOverlay* seekOverlay_ = nullptr;   // "LOADING" after 300 ms of a pending seek
    bool         playbackRequirementsPending_ = false;
    uint64_t     playbackSeekGeneration_ = 0;
    PlaybackPatchMerger playbackPatchMerger_;
    bool         playerStatusDrsAvailable_ = true;
    QSet<int>    allStatusDrsAvailable_;
    bool         playbackDriverRestricted_ = false;
    int          selectedPlaybackDriverIndex_ = -1;
    bool         playbackParticipantsReady_ = false;
    bool         playbackSparseRebuildPending_ = false;
    QWidget*     container_      = nullptr;
    QWidget*     loadingOverlay_ = nullptr;
    void refreshPlaybackDriverSelector();
    void resetPlaybackDriverSelection();

    // ── Excel export (playback bar → here) ────────────────────────
    // Export runs off the GUI thread (XlsxExportWorker on exportThread_); progress
    // is shown on a determinate overlay mirroring loadingOverlay_. exporting_ guards
    // against a second export starting while one is in flight.
    QWidget*      exportOverlay_     = nullptr;
    QLabel*       exportStageLabel_  = nullptr;
    QProgressBar* exportProgressBar_ = nullptr;
    QThread*      exportThread_      = nullptr;
    bool          exporting_         = false;
    void onExportXlsxRequested();   // save dialog + off-thread export of the loaded clip

    // ── Telemetry engine (libtnrp) ────────────────────────────────
    // Owns UDP receive, F1 24/25 parsing, .tnrd recording, and (Stage 2) playback.
    // engineSink_ marshals its JSON rows onto the GUI thread → onEngineRow().
    std::unique_ptr<tnrp::Engine> engine_;
    EngineSink*                   engineSink_ = nullptr;
    PairServiceState              pairServiceState_;
    QByteArray                    storedPairEngineState_;
    // The saved document is sealed but could not be opened this session; the
    // engine runs on a fresh identity and must not overwrite the pairings.
    bool                          keepStoredPairState_ = false;
    void applyEngineLogging();   // push wantRecord/outputDirectory to the engine
    QString recreateEngine();    // stop/create/start using the current persisted host config
    void showUdpListenerStatus(const QString& error);   // titlebar "UDP ERROR"; empty clears it
    void setLapHistoryCar(int carIdx);   // subscribe the engine to one car's lap times (-1 = none)
    QPointer<DriverLapsDialog> lapsDialog_;   // the open lap-times dialog, fed by pushed rows
    // Electron's lapHistoryCar claims: the engine pushes one car at a time;
    // the newest claim wins and releasing it hands the car back to the
    // previous claim (the laps dialog and the Trends page).
    struct LapHistoryClaim { int id = 0; int carIdx = -1; };
    QVector<LapHistoryClaim> lapHistoryClaims_;
    int nextLapHistoryClaim_ = 1;
    int claimLapHistoryCar(int carIdx);
    void releaseLapHistoryCar(int claimId);
    int trendsLapHistoryClaim_ = 0;   // the Trends page's claim while it is shown (0 = none)
    int streamedDriverIndex() const;  // the player live, the selected car in playback (-1 = unknown)
    void syncTrendsDriver();          // follow the streamed driver and (re)claim its lap history
    void receivePairState(const QByteArray& publicStateJson,
                          const QByteArray& persistedStateJson,
                          const QString& fallbackError = {});
    void syncPairStateFromEngine(const QString& fallbackError = {});
    void persistPairStateFromEngine();
    // Writes the engine document through PairStateVault, skipping unchanged
    // documents (a keyring write is a D-Bus round trip on Linux).
    void storePairEngineState(const QByteArray& json);
    bool handleRecordingErrorRow(const QByteArray& json);
    void showRecordingError(const QString& operation, const QString& message,
                            const QString& path);
    QMessageBox* recordingErrorDialog_ = nullptr;
    // Ask scope: a finished recording wrote both files and the user picks
    // which to keep. Choices queue while one is open; the prompt cannot be
    // dismissed without one.
    tnrp::RecordingScopes recordingScopes() const;
    bool handleRecordingFinishedRow(const QByteArray& json);
    struct RecordingChoice { QString allPath, driverPath, sessionName, trackName; };
    QList<RecordingChoice> pendingRecordingChoices_;
    QMessageBox* recordingChoiceDialog_ = nullptr;
    void showNextRecordingChoice();

    // Cached from the most recent protocol_status row (see onEngineRow()) so the
    // on-demand Settings dialog can show "Detected Protocol" without a push
    // channel into a dialog that may not be open. 0 = not yet known.
    int lastDetectedProtocolFormat_ = 0;
    int detectedProtocolWarningFormat_ = 0;
    int forcedProtocolWarningFormat_   = 0;


    // Optional diagnostics. The launch/fatal log in Diagnostics remains active
    // regardless of these settings; this timer only emits the detailed native
    // pipeline health snapshot selected on the Debug settings page.
    QTimer* diagnosticTimer_ = nullptr;
    qint64 diagnosticStartedAtMs_ = 0;
    quint64 diagnosticJsonRows_ = 0;
    quint64 diagnosticJsonBytes_ = 0;
    quint64 diagnosticBinaryCallbacks_ = 0;
    quint64 diagnosticBinaryBytes_ = 0;
    bool diagnosticWarnedNoDatagrams_ = false;
    bool diagnosticWarnedNoOutput_ = false;
    bool diagnosticWarnedNoConsumerMask_ = false;
    void resetAdditionalDiagnostics();
    void logAdditionalDiagnostics(const QString& reason);
    QJsonObject memoryDiagnosticsSnapshot() const;

    // ── Persistence ───────────────────────────────────────────────
    QSettings settings{ "TrackNRace", "NativeRecorder" };

    // ── Recording state ───────────────────────────────────────────
    // The actual write pipeline (gzip .tnrd, rolling flashback buffer, session
    // rotation, dedup) lives in the engine's TnrdWriter; we only retain the user
    // intent here and feed it to the engine via applyEngineLogging().
    bool    wantRecord = false;
    QString outputDirectory;

    void resizeEvent(QResizeEvent* e) override;
    void moveEvent(QMoveEvent* e) override;     // tracks the windowed bounds
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void closeEvent(QCloseEvent* e) override;   // persists window geometry on quit
    void scheduleNormalGeometryCapture();       // caches the windowed bounds (deferred)
    // Watches the backing QWindow's expose events for the rendering gate.
    bool eventFilter(QObject* obj, QEvent* e) override;
    // Window-state tracking (maximize/un-maximize geometry correction).
    void changeEvent(QEvent* e) override;

    // ── Builders ──────────────────────────────────────────────────
    QWidget* buildStrategyPage();
    // Refreshes the per-corner tyre cards on BOTH the Overview and Tyres pages
    // off the single dirtyTyres_ flag (they show the same live data).
    void     updateTyreCards();
    void     updateStrategyPage();

    // ── Coalesced panel refresh ───────────────────────────────────
    // Packets can arrive in bursts (especially fast playback); rebuilding a
    // panel per packet locks the UI. Each packet only marks its panel dirty and
    // the heavy rebuild runs once per refresh tick.
    Page currentPage_    = Overview;   // visible stack page; updaters skip hidden pages
    bool dirtyTiming_    = false;
    bool dirtyRacePanel_ = false;
    bool dirtyTyres_     = false;
    bool dirtyTyreSets_  = false;
    bool dirtyStrategy_  = false;
    // Electron's strategyRebuilding: set by a playback seek, cleared by the
    // next Strategy row (the engine's rebuilt snapshot for the new cursor).
    bool strategyRebuilding_ = false;
    bool dirtySession_   = false;
    bool dirtyEvents_    = false;
    bool dirtyProximity_ = false;
    bool dirtyTrackMapSession_      = false;
    bool dirtyTrackMapParticipants_ = false;
    bool dirtyTrackMapPositions_    = false;
    bool dirtyPower_     = false;
    bool dirtyTrends_    = false;
    bool dirtyDamage_    = false;
    bool uiRefreshPending_ = false;
    qint64 lastToolbarDeltaUpdateMs_ = -1;
    void scheduleUiRefresh();
    void flushUiRefresh();

    // Route a per-graph Chart/Table choice to the owning page. Shared by
    // setGraphView() (on user change) and applyGraphViews() (persisted, at startup).
    void dispatchGraphView(tnr::GraphSection s, bool table);
    void applyGraphViews();   // apply every persisted graph view mode once, at startup

    // ── Rendering gate (pause all UI work when the window isn't displayed) ──
    // Recording (UDP → parse → .tnrd) is independent of these and keeps running.
    bool renderingActive_   = true;
    // While the window is minimized or hidden: gives unused heap pages and the
    // working set back to the system (IdleMemory), shortly after hiding and
    // then periodically, since ingest keeps running.
    QTimer* idleMemoryTimer_ = nullptr;
    bool windowFilterHooked_ = false;   // installed the QWindow expose filter yet?

    // Last windowed bounds (never the maximized rect). Restored on launch, and
    // re-applied when the user un-maximizes — the WM can otherwise restore a wrong
    // size, especially when we launched already maximized (see changeEvent).
    QRect normalGeometry_;
    bool  captureScheduled_ = false;   // coalesces the deferred geometry capture
    void updateRenderingState();        // recompute desired state from window flags
    void setRenderingActive(bool on);   // start/stop the rendering subsystems

    // ── Live data routing ─────────────────────────────────────────
    void rewindLiveClock(float sessionTime);
    void emitLiveData(const tnrp::AnyRow& row,
                      const QJsonObject* sparseObject = nullptr);
    // Shared tail of the live paths (JSON cold rows, binary hot rows):
    // panels + SessionModel.
    void routeLiveRow(const tnrp::AnyRow& row,
                      const QJsonObject* sparseObject = nullptr);

    // Event toast notifications live in ToastHost.
    ToastHost* toasts_ = nullptr;
    std::optional<int> lastRaceLeader_;

    // Race events as Electron's telemetry store keeps them. streamedEvents_ is
    // every race_event row received (its raceEventsArr): the live list, and in
    // playback only the retirement de-duplication. In playback the list is the
    // recording's catalog events up to the playhead instead.
    std::vector<tnrp::RaceEventRow> streamedEvents_;
    std::vector<tnrp::RaceEventRow> playbackEvents_;   // catalog, sorted by session_time
    std::size_t playbackEventCount_ = 0;                // prefix at or before the playhead
    void loadPlaybackEvents();
    void updatePlaybackEventCursor();
    std::vector<tnrp::RaceEventRow> shownEvents() const;
    // The persistent safety-car toast, re-derived (Electron's useRaceBanners) on
    // session rows and event changes; re-shown only when its text changes.
    std::optional<std::pair<QString, QString>> shownSafetyCarBanner_;
    void refreshSafetyCarBanner();
    // Window title: app name plus the current session type, and in playback the
    // recording's track. Session type follows session rows (-1 until one
    // arrives); playback falls back to the recording header's session name.
    void updateWindowTitle();
    int     titleSessionType_ = -1;
    QString playbackTrackName_, playbackSessionName_;
    void ingestForModel(const tnrp::AnyRow& row);
    void schedulePlaybackDataRequirements();
    void updatePlaybackDataRequirements();
};
