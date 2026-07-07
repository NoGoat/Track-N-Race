#include "MainWindow.h"
#include "AppToolbar.h"
#include "PlaybackController.h"
#include "SessionModel.h"
#include "EngineSink.h"
#include "Labels.h"
#include "components/EditOverviewLayoutDialog.h"
#include "components/EditInputLayoutDialog.h"
#include "components/EditPowerLayoutDialog.h"
#include "components/EditMiscLayoutDialog.h"
#include "components/OverviewPage.h"
#include "components/StandingsPage.h"
#include "components/SessionPage.h"
#include "components/TyresPage.h"
#include "components/InputPage.h"
#include "components/PowerPage.h"
#include "components/MiscPage.h"
#include "components/StrategyPage.h"
#include "components/SettingsDialog.h"
#include "components/TrackMapWidget.h"
#include "components/ChartView.h"   // ChartView::reapplyRenderSettings (chart GPU settings)
#include "components/ToastEvents.h"
#include "components/Toast.h"
#include "components/ToastHost.h"
#include "BreezePalette.h"
#include "IconUtils.h"   // setApplicationStyle (style swap in setStyleName)

#include <QApplication>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QSizePolicy>
#include <QStyleHints>
#include <QCoreApplication>
#include <QTimer>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QMoveEvent>
#include <QWindowStateChangeEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QProgressBar>
#include <QStyleFactory>

#include <algorithm>
#include <map>

#include <tnrp/Engine.h>
#include <tnrp/Config.h>
#include <tnrp/AeroMode.h>

// Packet IDs, header layout, rate-limit/dedup tables and the F1 24/25 packet
// parsers used to live here; they now belong to libtnrp (tnrp::Parser /
// tnrp::TnrdWriter), which the engine drives. See onEngineRow().

// A geometry "looks maximized" if it (nearly) fills the screen's available area.
// Such a value must never be treated as the windowed/normal size: an earlier bug
// recorded the maximized size as the normal geometry, and restoring to it makes
// un-maximize look like a no-op. Rejecting it (on load and while tracking) both
// clears any already-poisoned saved value and stops it from regenerating.
static bool looksMaximized(const QRect& g, const QScreen* s) {
    if (!g.isValid()) return false;
    const QScreen* scr = s ? s : QGuiApplication::primaryScreen();
    if (!scr) return false;
    const QRect avail = scr->availableGeometry();
    return g.width() >= avail.width() - 12 && g.height() >= avail.height() - 12;
}

// ── Construction ───────────────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Track N Race Background Recorder");
    setMinimumSize(480, 520);   // toolbar collapses into its ⋯ menu below ~full width
    // Restore the last windowed size/position, then maximize directly if we were
    // closed maximized — setting the state before the first show maps the window
    // maximized straight away (no small-window flash). Un-maximizing afterwards is
    // corrected back to normalGeometry_ in changeEvent (the WM has no pre-maximize
    // geometry to restore to when we launch already maximized). See closeEvent.
    QRect saved = settings.value("window/normalGeometry").toRect();
    if (!saved.isValid() || looksMaximized(saved, nullptr)) {
        // First run, or a poisoned saved value (the maximized size stored as the
        // windowed geometry) — fall back to a sane centred default so un-maximize
        // always lands on a real window.
        const QScreen* scr = QGuiApplication::primaryScreen();
        const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1280, 800);
        saved = QRect(QPoint(0, 0), QSize(780, 580));
        saved.moveCenter(avail.center());
    }
    normalGeometry_ = saved;
    setGeometry(saved);
    if (settings.value("window/maximized", false).toBool())
        setWindowState(windowState() | Qt::WindowMaximized);

    outputDirectory = settings.value("outputDirectory").toString();
    wantRecord      = settings.value("autoRecord", false).toBool();

    const QString theme = settings.value("theme", "system").toString();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (theme == "light")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
    else if (theme == "dark")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif

    // Self-contained toolbar: page tabs, session timer, chart-window segment,
    // Open/Edit Layout/Settings actions, ⋯ overflow. Page names must match the
    // Page enum and the stack->addWidget() order below.
    toolbar_ = new AppToolbar(
        { "Overview", "Standings", "Session", "Tyres", "Strategy", "Input", "Power", "Misc" },
        settings.value("ui/toolbarShowLabels", false).toBool(), this);
    addToolBar(Qt::TopToolBarArea, toolbar_);
    // QMainWindow draws its own separator line between the toolbar area and the
    // central widget — a different element from QToolBar's own borders; keep it
    // suppressed so no stray line shows under the toolbar.
    setStyleSheet("QMainWindow::separator { width: 0px; height: 0px; background: transparent; }");

    connect(toolbar_, &AppToolbar::chartWindowChanged, this, [this](float secs) {
        if (overviewPage_) overviewPage_->setWindowSeconds(secs);
        if (tyresPage_) tyresPage_->setWindowSeconds(secs);
        if (inputPage_) inputPage_->setWindowSeconds(secs);
        if (powerPage_) powerPage_->setWindowSeconds(secs);
        if (miscPage_) miscPage_->setWindowSeconds(secs);
    });
    connect(toolbar_, &AppToolbar::editLayoutRequested, this, [this] {
        if (currentPage_ == Overview) {
            EditOverviewLayoutDialog* dlg = new EditOverviewLayoutDialog(overviewPage_, this);
            connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
            dlg->show();
        } else if (currentPage_ == Input) {
            EditInputLayoutDialog* dlg = new EditInputLayoutDialog(inputPage_, this);
            connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
            dlg->show();
        } else if (currentPage_ == Power) {
            EditPowerLayoutDialog* dlg = new EditPowerLayoutDialog(powerPage_, this);
            connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
            dlg->show();
        } else if (currentPage_ == Misc) {
            EditMiscLayoutDialog* dlg = new EditMiscLayoutDialog(miscPage_, this);
            connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
            dlg->show();
        }
    });
    connect(toolbar_, &AppToolbar::settingsRequested, this, [this] {
        SettingsDialog* dlg = new SettingsDialog(this, this);
        connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
        dlg->show();
    });

    // Lap-aware session model — the chart's single source of truth, fed by both
    // live UDP and playback. Created before the Overview tab so the chart can bind it.
    model_ = new SessionModel(this);

    // Order must match the Page enum and the AppToolbar page-name list above.
    QStackedWidget* stack = new QStackedWidget(this);
    stack->addWidget(overviewPage_ = new OverviewPage(model_));   // Overview
    stack->addWidget(standingsPage_ = new StandingsPage);   // Standings
    // A row click changed the selection; re-feed the cached rows immediately
    // (same synchronous rebuild as the old in-page click handler).
    connect(standingsPage_, &StandingsPage::refreshRequested, this, [this] {
        standingsPage_->updateTimingTable(lastTimingData, lastParticipantsData, lastAllStatusData);
        standingsPage_->updateRacePanel(lastTimingData, lastParticipantsData,
                                        lastPlayerLapData, lastPlayerStatusData, lastAllStatusData);
    });
    stack->addWidget(sessionPage_ = new SessionPage);   // Session
    stack->addWidget(tyresPage_ = new TyresPage(model_));   // Tyres
    stack->addWidget(buildStrategyPage());  // Strategy
    stack->addWidget(inputPage_ = new InputPage(model_));   // Input
    stack->addWidget(powerPage_ = new PowerPage(model_));   // Power
    stack->addWidget(miscPage_ = new MiscPage(model_));   // Misc

    // Apply persisted per-graph Chart/Table choices now that every page exists.
    applyGraphViews();

    // Coalesces panel rebuilds to one per event-loop pass (one per arriving packet,
    // 20..60 Hz) so bursts can't stack redundant rebuilds, without a fixed rate cap.
    uiRefreshTimer_ = new QTimer(this);
    uiRefreshTimer_->setSingleShot(true);
    uiRefreshTimer_->setInterval(0);
    connect(uiRefreshTimer_, &QTimer::timeout, this, &MainWindow::flushUiRefresh);

    // ── Bottom playback bar ────────────────────────────────────────────────────
    // Owns the TnrdPlayer and the transport bar; MainWindow reacts to its
    // entered/exited/rowReady/timeChanged signals below.
    playback_ = new PlaybackController(model_, this);
    playback_->setShowLabels(toolbarLabelsEnabled());   // match the toolbar labels option

    // Stack + separator + playback bar stacked vertically as the central widget
    container_ = new QWidget(this);
    auto* vbox = new QVBoxLayout(container_);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);
    vbox->addWidget(stack);
    vbox->addWidget(playback_->separator());
    vbox->addWidget(playback_->bar());
    setCentralWidget(container_);

    // Event toast notifications, rendered inside the central content widget.
    toasts_ = new ToastHost(container_);

    // Loading overlay (shown over the entire central area while decompressing/indexing)
    loadingOverlay_ = new QWidget(container_);
    loadingOverlay_->setAutoFillBackground(true);
    {
        QPalette pal = loadingOverlay_->palette();
        pal.setColor(QPalette::Window, palette().color(QPalette::Window));
        loadingOverlay_->setPalette(pal);
    }
    auto* ol = new QVBoxLayout(loadingOverlay_);
    ol->setAlignment(Qt::AlignCenter);
    ol->setSpacing(12);
    auto* loadingLabel = new QLabel("Loading recording…", loadingOverlay_);
    loadingLabel->setAlignment(Qt::AlignCenter);
    ol->addWidget(loadingLabel);
    auto* spinner = new QProgressBar(loadingOverlay_);
    spinner->setRange(0, 0);
    spinner->setFixedWidth(300);
    spinner->setTextVisible(false);
    ol->addWidget(spinner);
    loadingOverlay_->hide();

    connect(toolbar_, &AppToolbar::pageSelected, stack, &QStackedWidget::setCurrentIndex);
    connect(toolbar_, &AppToolbar::pageSelected, this, [this](int i) {
        currentPage_ = static_cast<Page>(i);   // refresh the newly-shown page from any pending data
        toolbar_->setEditLayoutEnabled(currentPage_ == Overview || currentPage_ == Input ||
                                       currentPage_ == Power || currentPage_ == Misc);
        flushUiRefresh();
    });

    connect(toolbar_, &AppToolbar::openRecordingRequested, this, [this] {
        QString path = QFileDialog::getOpenFileName(
            this, "Open File", outputDirectory,
            "TNRD Recordings (*.tnrd *.trnd)");
        if (!path.isEmpty()) {
            QMessageBox msgBox(this);
            msgBox.setWindowTitle("Open Session File");
            msgBox.setText("Are you sure you want to open this file?");
            msgBox.setInformativeText("Opening it will stop the event bridge and if you have an session right now with the game, it will be closed.\n\nSelected file: " + QFileInfo(path).fileName());
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            msgBox.setDefaultButton(QMessageBox::No);
            if (msgBox.exec() == QMessageBox::Yes) {
                const QFileInfo fi(path);
                qInfo("[open] recording requested: '%s' exists=%d readable=%d size=%lld bytes",
                      qUtf8Printable(path), fi.exists(), fi.isReadable(),
                      static_cast<long long>(fi.size()));
                playback_->load(path);
            }
        }
    });

    connect(playback_, &PlaybackController::loadingStarted, this, [this] {
        loadingOverlay_->setGeometry(container_->rect());
        loadingOverlay_->raise();
        loadingOverlay_->show();
    });

    connect(playback_, &PlaybackController::loadFailed, this, [this] {
        qWarning("[open] load failed — surfacing 'Load Failed' dialog (see [player]/[tnrd] logs above)");
        loadingOverlay_->hide();
        QMessageBox::warning(this, "Load Failed", "Could not open the recording file.");
    });

    connect(playback_, &PlaybackController::entered, this,
            [this](const nlohmann::json& hdr, float currentTime) {
        loadingOverlay_->hide();
        inPlayback_ = true;
        // Resolve labels against the recorded clip's format (DRS vs Straight Line
        // Mode, etc.) for the duration of playback.
        if (hdr.contains("protocol") && hdr["protocol"].is_number()) {
            const uint16_t fmt = hdr["protocol"].get<uint16_t>();
            tnr::Labels::instance().setFormat(fmt);
            if (overviewPage_) overviewPage_->refreshTitles();   // re-label all stat cards (wing flips DRS↔SLM)
            if (powerPage_) powerPage_->applyHarvestScale(fmt);  // 4 MJ → 8 MJ in 2026
            // Overtaking-aid overlay follows the clip's format: DRS (F1 24/25) vs
            // SLM (F1 26). The live protocol_status handler is skipped in playback.
            if (TrackMapWidget* map = sessionPage_ ? sessionPage_->trackMap() : nullptr)
                map->setAeroMode(tnrp::aeroMode(fmt) == "slm");
        }
        // Clear any frozen live value; the first replayed packet sets it afresh.
        if (toolbar_) toolbar_->resetSessionTimer();
        if (overviewPage_) overviewPage_->setPlaybackMode(true, currentTime);
        if (tyresPage_) tyresPage_->setPlaybackMode(true, currentTime);
        if (inputPage_) inputPage_->setPlaybackMode(true, currentTime);
        if (powerPage_) powerPage_->setPlaybackMode(true, currentTime);
        if (miscPage_) miscPage_->setPlaybackMode(true, currentTime);
        hotSmoother_.reset();   // entering playback: drop live fill state
        applyEngineLogging();   // inPlayback_ is set → stops live recording while reviewing
        QString trackName = QString::fromStdString(hdr.value("track_name", "Unknown"));
        QString sessName  = QString::fromStdString(hdr.value("session_name", "Unknown"));
        setWindowTitle(QString("Track N Race — %1 %2 [Playback]").arg(trackName, sessName));
    });

    connect(playback_, &PlaybackController::rowReady, this, [this](const nlohmann::json& j) {
        emitLiveData(j);   // panels only — the charts read the pre-scanned model
    });

    // A seek replays a state snapshot (incl. the session packet); swallow the one
    // safety-car toast that snapshot would otherwise raise — race_events aren't
    // replayed on seek, so those need no special handling.
    connect(playback_, &PlaybackController::seeked, this, [this] { scSuppressOnce_ = true; });

    connect(playback_, &PlaybackController::timeChanged, this, [this](float t) {
        if (overviewPage_) overviewPage_->setCurrentTime(t);
        if (tyresPage_) tyresPage_->setCurrentTime(t);
        if (inputPage_) inputPage_->setCurrentTime(t);
        if (powerPage_) powerPage_->setCurrentTime(t);
        if (miscPage_) miscPage_->setCurrentTime(t);
    });

    connect(playback_, &PlaybackController::exited, this, [this] {
        inPlayback_ = false;
        hotSmoother_.reset();   // back to live: start the fill state fresh
        applyEngineLogging();   // back to live: resume recording if it was enabled
        // Drop the playback timer value; live packets (if any) repopulate it.
        if (toolbar_) toolbar_->resetSessionTimer();
        if (overviewPage_) overviewPage_->setPlaybackMode(false);
        if (tyresPage_) tyresPage_->setPlaybackMode(false);
        if (inputPage_) inputPage_->setPlaybackMode(false);
        if (powerPage_) powerPage_->setPlaybackMode(false);
        if (miscPage_) miscPage_->setPlaybackMode(false);
        setWindowTitle("Track N Race Background Recorder");
    });

    // ── Telemetry engine (libtnrp) ────────────────────────────────────────
    // The engine owns the UDP socket, F1 24/25 parsing and .tnrd recording. Its
    // rows arrive as JSON on the GUI thread via EngineSink → onEngineRow(). The
    // sink connection must exist before the engine is constructed, because the
    // engine emits an initial protocol_status row from its constructor.
    engineSink_ = new EngineSink(this);
    connect(engineSink_, &EngineSink::rowReady, this, &MainWindow::onEngineRow);

    tnrp::Config cfg;
    cfg.port            = static_cast<uint16_t>(udpPort());
    cfg.bindAddress     = udpBindAddress().toStdString();
    cfg.protocol        = tnrp::overrideFromString(currentProtocolOverride().toStdString());
    cfg.hotRowsAsJson   = true;   // in-process consumer: hot rows as JSON, no binary channel
    cfg.loggingEnabled  = wantRecord && !outputDirectory.isEmpty();
    cfg.outputDirectory = outputDirectory.toStdString();
    engine_ = std::make_unique<tnrp::Engine>(cfg, engineSink_);

    if (!engine_->startUdp())
        QMessageBox::critical(this, "UDP Error",
            QString("Failed to bind to UDP port %1.\n"
            "Is another telemetry tool or Track-N-Race already open?").arg(udpPort()));

    // Forward-fill timer: re-emits the last hot row during dropped/late frames so
    // the live charts stay smooth on a lossy link (see HotRowSmoother). Runs at the
    // measured frame cadence; bootstraps at 60 Hz until the real rate is detected.
    hotFillTimer_ = new QTimer(this);
    hotFillTimer_->setInterval(hotSmoother_.periodMs());
    connect(hotFillTimer_, &QTimer::timeout, this, &MainWindow::onHotFillTick);
    hotFillTimer_->start();
}

// Tyre view/graph settings used by the Settings dialog — the Overview page owns
// the widgets and persistence.
OverviewLayout::TyreView MainWindow::currentTyreView() {
    return overviewPage_ ? overviewPage_->currentTyreView() : OverviewLayout::TyreCards;
}

void MainWindow::setTyreView(OverviewLayout::TyreView v) {
    if (overviewPage_) overviewPage_->setTyreView(v);
}

bool MainWindow::tyreGraphLifeMode() const {
    return overviewPage_ ? overviewPage_->tyreGraphLifeMode() : true;
}

void MainWindow::setTyreGraphLifeMode(bool life) {
    if (overviewPage_) overviewPage_->setTyreGraphLifeMode(life);
}

MainWindow::~MainWindow() {
    // The engine's destructor stops the UDP thread and flushes/closes any active
    // .tnrd stream. Reset explicitly so it tears down before the sink it points at.
    engine_.reset();
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    scheduleNormalGeometryCapture();
    if (loadingOverlay_ && loadingOverlay_->isVisible() && container_)
        loadingOverlay_->setGeometry(container_->rect());
    // Keep any visible toasts pinned to the content area's top-right corner.
    Toast::updateAllPositions();
}

void MainWindow::moveEvent(QMoveEvent* e) {
    QMainWindow::moveEvent(e);
    scheduleNormalGeometryCapture();
}

// Cache the windowed geometry, but DEFERRED to the next event-loop turn. A resize
// from a maximize/minimize can arrive while isMaximized()/isMinimized() still read
// false (the state flag updates a beat later); reading the state synchronously here
// would record the maximized size as the windowed geometry — the root cause of the
// "maximized geometry gets stored" bug. By the time the deferred slot runs, the
// state is settled, so we only ever record a genuine windowed rect. Coalesced via
// captureScheduled_ so a burst of resize/move events triggers one capture.
void MainWindow::scheduleNormalGeometryCapture() {
    if (captureScheduled_) return;
    captureScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        captureScheduled_ = false;
        if (isVisible() && !isMaximized() && !isMinimized() && !isFullScreen()
            && !looksMaximized(geometry(), screen()))
            normalGeometry_ = geometry();
    });
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
    // Expose on our backing QWindow: the platform tells us when the window becomes
    // fully obscured (e.g. covered by another window) or visible again. On X11 and
    // Windows isExposed() flips with occlusion; on Wayland occlusion isn't reported
    // so this just no-ops there (minimize/hide still handled via changeEvent).
    if (obj == windowHandle() && e->type() == QEvent::Expose)
        updateRenderingState();
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    // The backing QWindow only exists once the widget has been shown — install the
    // expose filter on first show so we can detect occlusion thereafter.
    if (!windowFilterHooked_) {
        if (QWindow* wh = windowHandle()) {
            wh->installEventFilter(this);
            windowFilterHooked_ = true;
        }
    }
    updateRenderingState();
}

void MainWindow::hideEvent(QHideEvent* e) {
    QMainWindow::hideEvent(e);
    updateRenderingState();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    // normalGeometry_ always holds the windowed bounds (never the maximized rect),
    // so persist it as-is plus the maximized flag. Neither carries the minimized
    // state, so quitting minimized still reopens at the normal/maximized size.
    if (normalGeometry_.isValid())
        settings.setValue("window/normalGeometry", normalGeometry_);
    settings.setValue("window/maximized", isMaximized());
    QMainWindow::closeEvent(e);
}

// Computes whether the window is actually being displayed, and pauses/resumes the
// rendering subsystems accordingly. Recording (.tnrd writing) is unaffected.
void MainWindow::updateRenderingState() {
    const QWindow* wh = windowHandle();
    const bool active = isVisible()
                     && !(windowState() & Qt::WindowMinimized)
                     && (!wh || wh->isExposed());
    if (active != renderingActive_)
        setRenderingActive(active);
}

void MainWindow::setRenderingActive(bool on) {
    renderingActive_ = on;
    if (on) {
        // Resume: let the model flush the data it kept ingesting while paused (one
        // emit rebuilds the charts from full history), wake the track map, and
        // rebuild the visible page from the dirty flags accumulated while paused.
        if (model_)    model_->setLiveFlushActive(true);
        if (sessionPage_) sessionPage_->setRenderingActive(true);
        flushUiRefresh();
    } else {
        // Pause: stop every timer-driven repaint. Data ingest (ingestForModel)
        // and the engine's UDP/recording path keep running so nothing is lost.
        if (uiRefreshTimer_) uiRefreshTimer_->stop();
        if (model_)    model_->setLiveFlushActive(false);
        if (sessionPage_) sessionPage_->setRenderingActive(false);
    }
}

void MainWindow::changeEvent(QEvent* e) {
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange) {
        auto* se = static_cast<QWindowStateChangeEvent*>(e);
        const bool wasMaximized = se->oldState() & Qt::WindowMaximized;
        const bool nowMaximized = isMaximized();

        if (!wasMaximized && nowMaximized) {
            // Entering maximized: normalGeometry_ still holds the geometry from just
            // BEFORE this maximize (tracking never records a maximized-sized rect).
            // Persist it right here so that closing while maximized restores to this
            // windowed geometry — never the maximized one.
            if (normalGeometry_.isValid())
                settings.setValue("window/normalGeometry", normalGeometry_);
        } else if (wasMaximized && !nowMaximized && !isMinimized() && !isFullScreen()
                   && normalGeometry_.isValid()) {
            // Un-maximizing: force our remembered windowed geometry (the WM may
            // otherwise restore a wrong size). Capture first — a WM restore-resize
            // could overwrite the member before the singleShot runs.
            const QRect target = normalGeometry_;
            QTimer::singleShot(0, this, [this, target] { setGeometry(target); });
        }
        updateRenderingState();            // minimize/restore toggles rendering
    }
}

// ── Slots ──────────────────────────────────────────────────────────────────

void MainWindow::setOutputDirectory(const QString& dir) {
    outputDirectory = dir;
    settings.setValue("outputDirectory", dir);
    applyEngineLogging();
}

void MainWindow::setAutoRecord(bool checked) {
    wantRecord = checked;
    settings.setValue("autoRecord", checked);
    applyEngineLogging();
}

// Push the current record intent to the engine's writer. Recording is suppressed
// while a clip is loaded for playback (matching the old "live UDP ignored during
// playback" behaviour) and resumed when the clip is closed.
void MainWindow::applyEngineLogging() {
    if (!engine_) return;
    const bool on = wantRecord && !outputDirectory.isEmpty() && !inPlayback_;
    engine_->setLogging(on, outputDirectory.toStdString());
}

void MainWindow::setTheme(const QString& theme) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (theme == "light")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
    else if (theme == "dark")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    else
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Unknown);
#endif
    settings.setValue("theme", theme);
    // Only reload Breeze's light/dark palette if Breeze is the active style; other
    // styles follow the theme via setColorScheme above.
    if (currentStyleName().compare("breeze", Qt::CaseInsensitive) == 0)
        applyBreezePalette(theme);
    // Re-evaluate whether the toolbar needs the window-colour override for the new
    // mode (forced light/dark over an opposite OS scheme).
    if (toolbar_) toolbar_->updateColorScheme();
}

void MainWindow::setStyleName(const QString& name) {
    // Swap the active QStyle live — Qt repolishes every existing widget, so no
    // restart is needed. "system" restores the platform default captured at
    // startup (main.cpp), since by now it may already have been overridden.
    const bool breeze = name.compare("breeze", Qt::CaseInsensitive) == 0;
    // Leaving Breeze: give the palette back to the platform *before* the new style
    // polishes, so a style that sets its own palette (e.g. Kvantum) or relies on
    // the platform one (windows11) isn't left wearing Breeze's colours. Restore the
    // platform font and icon theme too, since Breeze swapped in Noto Sans + the
    // bundled Breeze icons.
    if (!breeze) {
        restoreDefaultPalette();
        restoreDefaultFont();
        restoreDefaultIconTheme();
    }

    if (name == "system") {
        const QString def = qApp->property("defaultStyleName").toString();
        if (!def.isEmpty())
            setApplicationStyle(QStyleFactory::create(def),
                                def.compare("breeze", Qt::CaseInsensitive) == 0);
    } else {
        setApplicationStyle(QStyleFactory::create(name), breeze);
    }
    settings.setValue("style", name);
    // Breeze's plugin draws shapes but never sets a palette — apply the KDE one
    // after the style is in place, along with Breeze's default font (Noto Sans)
    // and the bundled Breeze icon theme. (No-ops in non-bundled builds.)
    if (breeze) {
        applyBreezePalette(currentTheme());
        applyBreezeFont();
        applyBreezeIconTheme();
    }
    // The toolbar override only applies under Breeze, so re-evaluate on style change.
    if (toolbar_) toolbar_->updateColorScheme();
}

void MainWindow::setToolbarLabels(bool checked) {
    settings.setValue("ui/toolbarShowLabels", checked);
    if (toolbar_) toolbar_->setShowLabels(checked);
    if (playback_) playback_->setShowLabels(checked);   // playback bar's Close File button
    // The map's Enlarge/Restore button follows the same toggle.
    if (TrackMapWidget* map = sessionPage_ ? sessionPage_->trackMap() : nullptr)
        map->setShowLabels(checked);
}

void MainWindow::setCompactSection(tnr::CompactSection s, bool on) {
    settings.setValue(tnr::compactKey(s), on);
    // Rebuild only the affected section, then repaint. Overview stats/damage repaint
    // themselves in their setters (their cards refresh per-packet, not on the dirty
    // tick); the rest are repopulated via the coalesced refresh below (only the
    // visible page runs now, the others refresh when next shown).
    using CS = tnr::CompactSection;
    switch (s) {
        case CS::OverviewStats:   if (overviewPage_) overviewPage_->setStatsCompact(on);  break;
        case CS::OverviewDamage:  if (overviewPage_) overviewPage_->setDamageCompact(on); break;
        case CS::OverviewTyres:   setTyresCompactLevel(on ? 1 : 0); return;   // tyres use the int-level path
        case CS::SessionCards:    if (sessionPage_)  sessionPage_->setCardsCompact(on);   dirtySession_  = true; break;
        case CS::SessionWeather:  if (sessionPage_)  sessionPage_->setWeatherCompact(on); dirtySession_  = true; break;
        case CS::SessionHeader:   if (sessionPage_)  sessionPage_->setHeaderCompact(on);  dirtySession_  = true; break;
        case CS::PowerCards:      if (powerPage_)    powerPage_->setCompactMode(on);      dirtyPower_    = true; break;
        case CS::StrategySummary: if (strategyPage_) strategyPage_->setCompactMode(on);   dirtyStrategy_ = true; break;
        default: break;
    }
    scheduleUiRefresh();
}

void MainWindow::setTyresCompactLevel(int level) {
    settings.setValue(tnr::compactKey(tnr::CompactSection::OverviewTyres), level);
    if (overviewPage_) overviewPage_->setTyresLevel(level);
    dirtyTyres_ = true;
    scheduleUiRefresh();
}

void MainWindow::dispatchGraphView(tnr::GraphSection s, bool table) {
    using GS = tnr::GraphSection;
    switch (s) {
        case GS::OverviewTelemetry:  if (overviewPage_) overviewPage_->setTelemetryTable(table); break;
        // Tyre graphs are shared: the Tyres page and the Overview tyre strip both follow them.
        case GS::TyreSurface:        if (tyresPage_) tyresPage_->setGraphSectionTable(0, table);
                                     if (overviewPage_) overviewPage_->setTyreGraphTable(0, table); break;
        case GS::TyreInner:          if (tyresPage_) tyresPage_->setGraphSectionTable(1, table);
                                     if (overviewPage_) overviewPage_->setTyreGraphTable(1, table); break;
        case GS::TyreBrake:          if (tyresPage_) tyresPage_->setGraphSectionTable(2, table);
                                     if (overviewPage_) overviewPage_->setTyreGraphTable(2, table); break;
        case GS::TyreWear:           if (tyresPage_) tyresPage_->setGraphSectionTable(3, table);
                                     if (overviewPage_) overviewPage_->setTyreGraphTable(3, table); break;
        case GS::InputGear:          if (inputPage_) inputPage_->setGraphSectionTable(0, table); break;
        case GS::InputThrottleBrake: if (inputPage_) inputPage_->setGraphSectionTable(1, table); break;
        case GS::InputSteering:      if (inputPage_) inputPage_->setGraphSectionTable(2, table); break;
        case GS::PowerSplit:         if (powerPage_) powerPage_->setGraphSectionTable(0, table); break;
        case GS::PowerHarvest:       if (powerPage_) powerPage_->setGraphSectionTable(1, table); break;
        case GS::PowerStore:         if (powerPage_) powerPage_->setGraphSectionTable(2, table); break;
        case GS::PowerFuel:          if (powerPage_) powerPage_->setGraphSectionTable(3, table); break;
        case GS::MiscGForce:         if (miscPage_) miscPage_->setGraphSectionTable(0, table); break;
        case GS::MiscRideHeight:     if (miscPage_) miscPage_->setGraphSectionTable(1, table); break;
        default: break;
    }
}

void MainWindow::setGraphView(tnr::GraphSection s, bool table) {
    settings.setValue(tnr::graphViewKey(s), table);
    dispatchGraphView(s, table);
}

void MainWindow::applyGraphViews() {
    for (int i = 0; i < (int)tnr::GraphSection::Count_; ++i) {
        const tnr::GraphSection s = (tnr::GraphSection)i;
        dispatchGraphView(s, graphView(s));
    }
}

void MainWindow::setContrastThreshold(float val) {
    settings.setValue("ui/contrastThreshold", val);
    dirtyTiming_ = true;
    scheduleUiRefresh();
}

void MainWindow::setChartMsaaSamples(int samples) {
    settings.setValue("ui/chartMsaaSamples", samples);
    ChartView::reapplyRenderSettings();   // live re-apply to every chart, no restart
}

void MainWindow::setTrackMapLabelMode(int mode) {
    settings.setValue("ui/trackMapLabelMode", mode);
    if (TrackMapWidget* map = sessionPage_ ? sessionPage_->trackMap() : nullptr) {
        map->setLabelMode(static_cast<TrackMapWidget::LabelMode>(mode));
    }
}

void MainWindow::setTrackMapSectorColors(bool on) {
    settings.setValue("ui/trackMapSectorColors", on);
    if (TrackMapWidget* map = sessionPage_ ? sessionPage_->trackMap() : nullptr)
        map->setSectorColors(on);
}

void MainWindow::setTrackMapOpacity(int pct) {
    settings.setValue("ui/trackMapOpacity", pct);
    if (TrackMapWidget* map = sessionPage_ ? sessionPage_->trackMap() : nullptr)
        map->setMapOpacity(pct / 100.0);
}

void MainWindow::setTrackMapIdleTimeout(int secs) {
    settings.setValue("ui/trackMapIdleTimeout", secs);
    if (TrackMapWidget* map = sessionPage_ ? sessionPage_->trackMap() : nullptr)
        map->setIdleTimeout(secs);
}

void MainWindow::setProtocolOverride(const QString& ovr) {
    settings.setValue("protocolOverride", ovr);
    if (engine_) engine_->setOverride(tnrp::overrideFromString(ovr.toStdString()));
}

// Port / bind-address changes rebind the live UDP socket without recreating the
// engine (Engine::restartUdp stops the listener, re-binds and resets the parser).
// Both guard against no-op re-applies so the socket isn't churned when the field
// loses focus unchanged.
void MainWindow::setUdpPort(int port) {
    if (port == udpPort()) return;
    settings.setValue("udp/port", port);
    if (engine_) engine_->restartUdp(static_cast<uint16_t>(port), udpBindAddress().toStdString());
}

void MainWindow::setUdpBindAddress(const QString& addr) {
    if (addr == udpBindAddress()) return;
    settings.setValue("udp/bindAddress", addr);
    if (engine_) engine_->restartUdp(static_cast<uint16_t>(udpPort()), addr.toStdString());
}

void MainWindow::onEngineRow(const QByteArray& json) {
    // While a clip is loaded, TnrdPlayer drives the UI; drop any live rows the
    // engine is still parsing in the background (mirrors the old datagram drain).
    if (inPlayback_) return;

    nlohmann::json row;
    try {
        row = nlohmann::json::parse(json.constData(), json.constData() + json.size());
    } catch (...) {
        return;
    }
    if (!row.contains("type")) return;

    // Track the active packet format so UI labels resolve through the library's
    // i18n catalog (tnr::Labels). The engine emits protocol_status on connect and
    // on every format change, so labels re-theme when 2025↔2026 switches.
    if (row.value("type", std::string{}) == "protocol_status") {
        if (row.contains("detected_format") && row["detected_format"].is_number()) {
            lastDetectedProtocolFormat_ = row["detected_format"].get<int>();
        }
        if (row.contains("active_format") && row["active_format"].is_number()) {
            const uint16_t fmt = row["active_format"].get<uint16_t>();
            tnr::Labels::instance().setFormat(fmt);
            if (overviewPage_) overviewPage_->refreshTitles();   // re-label all stat cards (wing flips DRS↔SLM)
            if (powerPage_) powerPage_->applyHarvestScale(fmt);  // 4 MJ → 8 MJ in 2026
            // Overtaking-aid overlay follows the format: DRS (F1 24/25) vs SLM (F1 26).
            if (TrackMapWidget* map = sessionPage_ ? sessionPage_->trackMap() : nullptr)
                map->setAeroMode(tnrp::aeroMode(fmt) == "slm");
        }
        return;
    }

    // The engine has already done format detection, rate-limiting, recording and
    // (when enabled) state-row deduplication; we only fan the row out to the live
    // UI panels and the lap-aware SessionModel. Header session_time rides on every
    // hot/cold row that needs it.
    const std::string type = row.value("type", std::string{});
    const float sessionTime = row.value("session_time", -1.0f);
    emitLiveData(row);
    ingestForModel(row, sessionTime);
    feedHotSmoother(type, row, sessionTime);
}

// Records the latest real hot row in the forward-fill smoother (live only). The
// fill timer (onHotFillTick) re-emits these during gaps so the charts stay smooth
// on a lossy link. See HotRowSmoother.
void MainWindow::feedHotSmoother(const std::string& type, const nlohmann::json& row, float sessionTime) {
    if (type == "telemetry")      hotSmoother_.onTelemetry(row, sessionTime);
    else if (type == "motion")    hotSmoother_.onMotion(row);
    else if (type == "motion_ex") hotSmoother_.onMotionEx(row);
}

// Fires at the measured frame cadence. When the last interval had no fresh
// telemetry (a dropped/late frame), the smoother yields held-forward rows which we
// push through the same live path as real rows — display-only, never recorded.
void MainWindow::onHotFillTick() {
    if (inPlayback_) return;   // playback feeds the model from the file, no fills
    std::vector<nlohmann::json> fills = hotSmoother_.tick();
    for (const nlohmann::json& f : fills) {
        const float st = f.value("session_time", -1.0f);
        emitLiveData(f);
        ingestForModel(f, st);
    }
    // Track the detected cadence so the timer beats with the game's send rate.
    const int p = hotSmoother_.periodMs();
    if (hotFillTimer_ && hotFillTimer_->interval() != p) hotFillTimer_->setInterval(p);
}

// ── Live data extraction → signals ─────────────────────────────────────────

void MainWindow::emitLiveData(const nlohmann::json& row) {
    const std::string type = row["type"].get<std::string>();
    // Every packet carries the header session_time; drive the toolbar timer from it.
    if (row.contains("session_time"))
        if (toolbar_) toolbar_->updateSessionTimer(row["session_time"].get<float>());
    if (type == "telemetry") {
        if (overviewPage_) overviewPage_->onTelemetry(row);
        lastPlayerTelemetryData = row;
        dirtyTyres_ = true; scheduleUiRefresh();
    } else if (type == "status") {
        if (overviewPage_) overviewPage_->onStatus(row);
        lastPlayerStatusData = row;
        dirtyRacePanel_ = true; dirtyPower_ = true; dirtyStrategy_ = true; scheduleUiRefresh();
    } else if (type == "damage") {
        if (overviewPage_) overviewPage_->onDamage(row);
        lastPlayerDamageData = row;
        dirtyTyres_ = true; dirtyStrategy_ = true; scheduleUiRefresh();
    } else if (type == "tyre_sets") {
        lastTyreSetsData = row;
        dirtyTyreSets_ = true; dirtyStrategy_ = true; scheduleUiRefresh();
    } else if (type == "lap") {
        if (overviewPage_) overviewPage_->onLap(row);
        lastPlayerLapData = row;
        dirtyRacePanel_ = true; dirtyStrategy_ = true; scheduleUiRefresh();
    } else if (type == "positions") {
        lastPositionsData = row;
        dirtyTrackMap_ = true; scheduleUiRefresh();
    } else if (type == "session") {
        lastSessionData = row;
        // Safety-car state changes update the persistent banner. SC/VSC/FL show a
        // persistent toast; returning to green (sc=0) silently dismisses it — no
        // "Track Clear" notification, matching the Electron app. Seeks are suppressed
        // via the one-shot flag set in seeked().
        const int sc = row.value("safety_car_status", 0);
        const bool suppress = scSuppressOnce_;
        scSuppressOnce_ = false;
        if (!suppress && sc != lastSafetyCarStatus_) {
            if (auto spec = safetyCarToast(lastSafetyCarStatus_, sc)) {
                toasts_->show(*spec);
            } else if (sc == 0) {
                toasts_->dismissPersistent();
            }
        }
        lastSafetyCarStatus_ = sc;
        dirtySession_ = true; dirtyTrackMap_ = true; dirtyStrategy_ = true; scheduleUiRefresh();
    } else if (type == "race_event") {
        if (row.value("code", "") == "SSTA") {
            if (sessionPage_) sessionPage_->clearEvents();
            if (standingsPage_) standingsPage_->resetForNewSession();
            if (strategyPage_) strategyPage_->resetForNewSession();
            lastSafetyCarStatus_ = 0;
        }
        if (sessionPage_) sessionPage_->addEvent(row);
        // Transient notification for the event, both live and during playback.
        // race_events are never replayed on seek, so scrubbing won't re-fire them.
        if (auto spec = buildToast(row, lastParticipantsData)) toasts_->show(*spec);
        dirtyEvents_ = true; scheduleUiRefresh();
    } else if (type == "timing") {
        lastTimingData = row;
        dirtyTiming_ = true; dirtyProximity_ = true; dirtyStrategy_ = true; scheduleUiRefresh();
    } else if (type == "participants") {
        lastParticipantsData = row;
        dirtyTiming_ = true; dirtyTrackMap_ = true; dirtyStrategy_ = true; scheduleUiRefresh();
    } else if (type == "all_status") {
        lastAllStatusData = row;
        dirtyTiming_ = true; dirtyStrategy_ = true; scheduleUiRefresh();
    } else if (type == "fastest_lap") {
        if (standingsPage_) standingsPage_->noteFastestLap(row.value("car_idx", -1));
        dirtyTiming_ = true; scheduleUiRefresh();
    } else if (type == "session_history_fastest") {
        if (standingsPage_ && standingsPage_->noteSessionHistoryFastest(
                row.value("car_idx", -1), row.value("best_lap_time_ms", 0))) {
            dirtyTiming_ = true; scheduleUiRefresh();
        }
    }
}

QWidget* MainWindow::buildStrategyPage() {
    strategyPage_ = new StrategyPage(this);
    return strategyPage_;
}

void MainWindow::updateStrategyPage() {
    if (!strategyPage_) return;
    // Actual lap times for the per-stint target tables, sourced from the
    // authoritative SessionModel laps (live ingest or the playback pre-scan), so
    // the table stays correct under playback seeking — not just sequential replay.
    std::map<int, int> lapTimesByNum;
    if (model_)
        for (const LapBlock& lb : model_->data().laps)
            if (lb.lapTimeMs > 0) lapTimesByNum[lb.lapNum] = lb.lapTimeMs;
    strategyPage_->update(lastPlayerLapData, lastSessionData, lastPlayerStatusData,
                          lastPlayerDamageData, lastTimingData, lastParticipantsData,
                          lastTyreSetsData, lastAllStatusData, lapTimesByNum);
}

// The Overview and Tyres pages show the same per-corner tyre cards, refreshed
// together off the single dirtyTyres_ flag so neither goes stale while hidden.
void MainWindow::updateTyreCards() {
    if (tyresPage_) tyresPage_->updateTyreCards(lastPlayerTelemetryData, lastPlayerDamageData);
    if (overviewPage_) overviewPage_->updateTyreCards(lastPlayerTelemetryData, lastPlayerDamageData);
}

void MainWindow::scheduleUiRefresh() {
    // While paused (window hidden/minimized/occluded) packets still set the dirty
    // flags, but we don't run the timer — the visible page is rebuilt once on
    // resume (see setRenderingActive).
    if (!renderingActive_) return;
    if (!uiRefreshTimer_->isActive()) uiRefreshTimer_->start();
}

void MainWindow::flushUiRefresh() {
    // Only rebuild the panels on the visible page; others stay marked dirty and
    // refresh when shown (see the page-switch handler). This keeps hidden pages
    // (e.g. the 20-row standings table) from hitching the visible page's
    // animations on the shared UI thread.
    switch (currentPage_) {
        case Overview:
            if (dirtyTyres_)     { updateTyreCards();        dirtyTyres_     = false; }
            break;
        case Standings:
            if (dirtyTiming_)    { standingsPage_->updateTimingTable(lastTimingData, lastParticipantsData, lastAllStatusData); dirtyTiming_ = false; }
            if (dirtyRacePanel_) { standingsPage_->updateRacePanel(lastTimingData, lastParticipantsData, lastPlayerLapData, lastPlayerStatusData, lastAllStatusData); dirtyRacePanel_ = false; }
            break;
        case Session:
            if (dirtyProximity_) { sessionPage_->updateProximity(lastTimingData, lastParticipantsData); dirtyProximity_ = false; }
            if (dirtySession_)   { sessionPage_->updateSession(lastSessionData, lastTimingData);        dirtySession_   = false; }
            if (dirtyEvents_)    { sessionPage_->updateEvents(lastParticipantsData);                    dirtyEvents_    = false; }
            if (dirtyTrackMap_)  { sessionPage_->updateTrackMap(lastSessionData, lastParticipantsData, lastPositionsData); dirtyTrackMap_ = false; }
            break;
        case Tyres:
            if (dirtyTyres_)     { updateTyreCards();        dirtyTyres_     = false; }
            if (dirtyTyreSets_)  { if (tyresPage_) tyresPage_->updateTyreSets(lastTyreSetsData); dirtyTyreSets_ = false; }
            break;
        case Strategy:
            if (dirtyStrategy_)  { updateStrategyPage();     dirtyStrategy_  = false; }
            break;
        case Power:
            if (dirtyPower_)     { if (powerPage_) powerPage_->update(lastPlayerStatusData); dirtyPower_ = false; }
            break;
        default:
            break;
    }
}

// Routes a parsed row into the lap-aware SessionModel. Shared by the live UDP
// path and (for completeness) any streamed source. The model — not the chart —
// owns chart state; the chart re-queries the model on its change signals.
void MainWindow::ingestForModel(const nlohmann::json& row, float sessionTime) {
    if (!model_ || inPlayback_) return;   // playback feeds the model via the load scan
    const std::string rtype = row.value("type", std::string{});
    // A backward jump in session time is an in-game flashback/rewind: drop only the
    // samples newer than the rewind point and keep the rest (NOT a full reset — that
    // wiped all live data). A genuine restart rewinds to ~0, which truncates to empty
    // anyway. Same 0.2s guard as the recording-side truncate above.
    if (rtype == "telemetry" && sessionTime < model_->data().latestTime - 0.2f)
        model_->truncateAfter(sessionTime);
    if (rtype == "telemetry") {
        model_->onTelemetry(sessionTime,
                            row.value("speed_kph", 0.0f),
                            row.value("rpm", 0),
                            row.value("gear", 0),
                            row.value("throttle", 0.0f),
                            row.value("brake", 0.0f),
                            row.value("steering", 0.0f));
        // Combine live tyre temps with last-seen wear from the damage packet.
        model_->onTyre(sessionTime,
            row.value("tyre_temp_surface_fl", 0.0f), row.value("tyre_temp_surface_fr", 0.0f),
            row.value("tyre_temp_surface_rl", 0.0f), row.value("tyre_temp_surface_rr", 0.0f),
            row.value("tyre_temp_inner_fl",   0.0f), row.value("tyre_temp_inner_fr",   0.0f),
            row.value("tyre_temp_inner_rl",   0.0f), row.value("tyre_temp_inner_rr",   0.0f),
            row.value("brake_temp_fl",        0.0f), row.value("brake_temp_fr",        0.0f),
            row.value("brake_temp_rl",        0.0f), row.value("brake_temp_rr",        0.0f),
            lastPlayerDamageData.value("tyre_wear_fl",  0.0f), lastPlayerDamageData.value("tyre_wear_fr",  0.0f),
            lastPlayerDamageData.value("tyre_wear_rl",  0.0f), lastPlayerDamageData.value("tyre_wear_rr",  0.0f));
    }
    else if (rtype == "status")
        model_->onStatus(
            sessionTime,
            row.value("ers_pct", 0.0f),
            row.value("fuel_kg", 0.0f),
            row.value("engine_power_ice_kw", 0.0f),
            row.value("engine_power_mguk_kw", 0.0f),
            row.value("ers_harvested_mguk_j", 0.0f),
            row.value("ers_harvested_mguh_j", 0.0f)
        );
    else if (rtype == "lap")
        model_->onLap(row.value("lap_num", 0),
                     row.value("current_lap_ms", 0),
                     row.value("last_lap_ms", 0),
                     row.value("lap_invalid", false));
    else if (rtype == "motion")
        model_->onMotion(sessionTime,
                         row.value("g_lat", 0.0f),
                         row.value("g_long", 0.0f));
    else if (rtype == "motion_ex")
        model_->onMotionEx(sessionTime,
                           row.value("front_aero_height_mm", 0.0f),
                           row.value("rear_aero_height_mm", 0.0f));
}
