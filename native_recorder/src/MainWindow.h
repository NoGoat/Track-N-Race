#pragma once

#include <QMainWindow>
#include <QSettings>
#include <QByteArray>

#include <string>
#include <memory>
#include <optional>

#include <tnrp/AnyRow.h>

#include "HotRowSmoother.h"
#include "CompactSettings.h"
#include "GraphViewSettings.h"
#include "components/OverviewLayout.h"

class OverviewPage;
class StandingsPage;
class SessionPage;
class StrategyPage;
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
class QTimer;
class QLabel;
class QProgressBar;
class QThread;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // Page tabs / stacked-widget order. Declaration order *is* the tab order and
    // the QStackedWidget index, so inserting a page here (and in the matching
    // AppToolbar page-name list and stack->addWidget() list) renumbers everything
    // for free. PageCount is the tab count — keep it last.
    enum Page { Overview, Standings, Session, Tyres, Strategy, Input, Power, Misc, PageCount };

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Tyre view/graph settings used by the Settings dialog — one-line forwarders
    // to the Overview page, which owns the widgets and persistence.
    OverviewLayout::TyreView currentTyreView();
    void setTyreView(OverviewLayout::TyreView v);
    bool tyreGraphLifeMode() const;
    void setTyreGraphLifeMode(bool life);

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
    bool    compactSection(tnr::CompactSection s) const { return settings.value(tnr::compactKey(s), false).toBool(); }
    void    setCompactSection(tnr::CompactSection s, bool on);
    int     weatherCompactLevel() const;
    void    setWeatherCompactLevel(int level);
    // Overview tyre cards have four density levels (0 Full … 3 Ultra Compact 2),
    // so they use an int level rather than the on/off compactSection() path.
    int     tyresCompactLevel() const { return settings.value(tnr::compactKey(tnr::CompactSection::OverviewTyres), 0).toInt(); }
    void    setTyresCompactLevel(int level);
    // Per-graph view mode: false = chart (default), true = raw-values table.
    bool    graphView(tnr::GraphSection s) const { return settings.value(tnr::graphViewKey(s), false).toBool(); }
    void    setGraphView(tnr::GraphSection s, bool table);
    float   contrastThreshold() const { return settings.value("ui/contrastThreshold", 1.75f).toFloat(); }
    void    setContrastThreshold(float val);
    int     chartMsaaSamples() const { return settings.value("ui/chartMsaaSamples", 4).toInt(); }
    void    setChartMsaaSamples(int samples);
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
    QString currentProtocolOverride() const { return settings.value("protocolOverride", "auto").toString(); }
    void    setProtocolOverride(const QString& ovr);
    int     lastDetectedProtocolFormat() const { return lastDetectedProtocolFormat_; }
    int     udpPort() const { return settings.value("udp/port", 20777).toInt(); }
    void    setUdpPort(int port);
    QString udpBindAddress() const { return settings.value("udp/bindAddress", "0.0.0.0").toString(); }
    void    setUdpBindAddress(const QString& addr);

private slots:
    // Receives one pre-serialised JSON row (cold/control) from the libtnrp engine
    // (marshalled onto the GUI thread by EngineSink) and parses it into a typed
    // tnrp::AnyRow. Hot 60 Hz rows arrive packed via onEngineBinary instead.
    void onEngineRow(const QByteArray& json);
    // Receives one packed hot-row batch (telemetry/motion/motion_ex/positions)
    // and decodes it with tnrp::bin::decodeBatch — no JSON on the live hot path.
    void onEngineBinary(const QByteArray& batch);

private:
    // ── Overview tab ──────────────────────────────────────────────
    // Self-contained page widget (stat cards, telemetry chart, tyre section,
    // damage rows); fed rows synchronously via on*() from emitLiveData and
    // playback state via its setters.
    OverviewPage*   overviewPage_ = nullptr;
    SessionModel*   model_        = nullptr;

    // ── Standings page ────────────────────────────────────────────
    // Self-contained page widget (timing table + race panel + selection and
    // fastest-lap state); fed the cached rows below via its update methods.
    StandingsPage*   standingsPage_      = nullptr;
    std::optional<TimingRow>             lastTimingData;
    std::optional<tnrp::ParticipantsRow> lastParticipantsData;
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

    // ── Strategy page ─────────────────────────────────────────────
    // Self-contained: fed the cached JSON rows below on refresh, runs its own
    // stint/undercut/wear calculations internally. No SessionModel binding.
    StrategyPage* strategyPage_ = nullptr;

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
    // Self-contained (TnrdPlayer + transport bar); MainWindow reacts to its
    // entered/exited/rowReady/timeChanged signals.
    PlaybackController* playback_ = nullptr;
    bool         inPlayback_     = false;
    QWidget*     container_      = nullptr;
    QWidget*     loadingOverlay_ = nullptr;

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
    void applyEngineLogging();   // push wantRecord/outputDirectory to the engine

    // Cached from the most recent protocol_status row (see onEngineRow()) so the
    // on-demand Settings dialog can show "Detected Protocol" without a push
    // channel into a dialog that may not be open. 0 = not yet known.
    int lastDetectedProtocolFormat_ = 0;

    // Forward-fill smoother for the live hot stream: on a dropped/late frame it
    // re-emits the last telemetry/motion/motion_ex one frame forward so the charts
    // don't stutter on a flaky link. Display-only; driven by hotFillTimer_.
    HotRowSmoother hotSmoother_;
    QTimer*        hotFillTimer_ = nullptr;
    void feedHotSmoother(const tnrp::AnyRow& row);
    void onHotFillTick();

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
    QTimer* uiRefreshTimer_ = nullptr;
    Page currentPage_    = Overview;   // visible stack page; updaters skip hidden pages
    bool dirtyTiming_    = false;
    bool dirtyRacePanel_ = false;
    bool dirtyTyres_     = false;
    bool dirtyTyreSets_  = false;
    bool dirtyStrategy_  = false;
    bool dirtySession_   = false;
    bool dirtyEvents_    = false;
    bool dirtyProximity_ = false;
    bool dirtyTrackMap_  = false;
    bool dirtyPower_     = false;
    void scheduleUiRefresh();
    void flushUiRefresh();

    // Route a per-graph Chart/Table choice to the owning page. Shared by
    // setGraphView() (on user change) and applyGraphViews() (persisted, at startup).
    void dispatchGraphView(tnr::GraphSection s, bool table);
    void applyGraphViews();   // apply every persisted graph view mode once, at startup

    // ── Rendering gate (pause all UI work when the window isn't displayed) ──
    // Recording (UDP → parse → .tnrd) is independent of these and keeps running.
    bool renderingActive_   = true;
    bool windowFilterHooked_ = false;   // installed the QWindow expose filter yet?

    // Last windowed bounds (never the maximized rect). Restored on launch, and
    // re-applied when the user un-maximizes — the WM can otherwise restore a wrong
    // size, especially when we launched already maximized (see changeEvent).
    QRect normalGeometry_;
    bool  captureScheduled_ = false;   // coalesces the deferred geometry capture
    void updateRenderingState();        // recompute desired state from window flags
    void setRenderingActive(bool on);   // start/stop the rendering subsystems

    // ── Live data routing ─────────────────────────────────────────
    void emitLiveData(const tnrp::AnyRow& row);
    // Shared tail of the live paths (JSON cold rows, binary hot rows, fills):
    // panels + SessionModel + forward-fill smoother.
    void routeLiveRow(const tnrp::AnyRow& row);

    // Event toast notifications live in ToastHost; lastSafetyCarStatus_ tracks
    // the session packet's SC state so changes can be toasted (routing decision).
    ToastHost* toasts_ = nullptr;
    int    lastSafetyCarStatus_ = 0;
    // Set when the player seeks: the safety-car snapshot that follows resyncs
    // lastSafetyCarStatus_ without toasting (a jump isn't a live SC change).
    bool scSuppressOnce_ = false;
    void ingestForModel(const tnrp::AnyRow& row);
};
