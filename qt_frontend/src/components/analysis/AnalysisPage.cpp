#include "AnalysisPage.h"

#include "AnalysisBrakingZones.h"
#include "AnalysisLapSlot.h"
#include "AnalysisMapView.h"
#include "AnalysisMetricPicker.h"
#include "AnalysisPanels.h"
#include "AnalysisSeriesModel.h"
#include "AnalysisSplitter.h"
#include "AnalysisWidgets.h"

#include "../AnalyzeChart.h"
#include "../PageUiHelpers.h"
#include "../SegmentedControl.h"
#include "../UnderlineTabBar.h"
#include "../TyreHelpers.h"
#include "../../AnalysisFileReader.h"
#include "../../IconUtils.h"
#include "../../SessionModel.h"
#include "../../TnrdPlayer.h"

#include <QAction>
#include <QActionGroup>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QHideEvent>
#include <QShowEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QThreadPool>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <utility>

namespace {

constexpr int kSettingsVersion = 10;
constexpr int kWorkspaceVersion = 1;   // QMainWindow::saveState of the Analysis docks
constexpr int kPlaybackTab = 0;
constexpr int kFixedTab = 1;

QIcon themed(const QWidget* widget, std::initializer_list<const char*> names,
             QStyle::StandardPixmap fallback) {
    QIcon icon;
    for (const char* name : names) {
        icon = QIcon::fromTheme(QString::fromLatin1(name));
        if (!icon.isNull()) break;
    }
    return adaptThemeIcon(icon, widget->palette().color(QPalette::WindowText),
                          widget->style()->standardIcon(fallback));
}

QAction* makeToggle(QObject* parent, const QIcon& icon, const QString& text,
                    const QString& whatsThis) {
    auto* action = new QAction(icon, text, parent);
    action->setCheckable(true);
    action->setToolTip(text);
    action->setWhatsThis(whatsThis);
    action->setStatusTip(whatsThis);
    return action;
}

QString viewKey(int view) {
    switch (view) {
    case 1: return QStringLiteral("split");
    case 2: return QStringLiteral("map");
    default: return QStringLiteral("graph");
    }
}

AnalysisLapChoice lapChoiceFor(const LapBlock& lap, int fastestLapNum) {
    AnalysisLapChoice choice;
    choice.lapNum = lap.lapNum;
    choice.lapTimeMs = lap.lapTimeMs;
    choice.fastest = lap.lapNum == fastestLapNum;
    for (auto it = lap.sts.crbegin(); it != lap.sts.crend(); ++it) {
        if (it->tyre_compound <= 0) continue;
        choice.compound = tyreLabel(it->tyre_compound);
        choice.compoundColor = tyreTextColor(it->tyre_compound, it->visual_compound);
        break;
    }
    return choice;
}

} // namespace

AnalysisPage::AnalysisPage(SessionModel* model, QWidget* parent)
    : QWidget(parent), model_(model) {
    seriesModel_ = new AnalysisSeriesModel(this);
    secondaryReader_ = new AnalysisFileReader(this);

    buildToolBar();

    chart_ = new AnalyzeChart(this);
    chart_->setModel(model_);
    map_ = new AnalysisMapView(this);
    graphPane_ = buildGraphPane();
    mapPane_ = buildMapPane();

    // Split view: graphs | map, kept as a percentage of the width (default 50 / 50).
    contentSplitter_ = new AnalysisSplitter(this);
    contentSplitter_->setChildrenCollapsible(false);
    contentSplitter_->setHandleWidth(kAnalysisGripExtent);
    contentSplitter_->addWidget(graphPane_);
    contentSplitter_->addWidget(mapPane_);
    contentSplitter_->setStretchFactor(0, 1);
    contentSplitter_->setStretchFactor(1, 1);

    workspace_ = new AnalysisWorkspace(this);
    workspace_->setCentralWidget(contentSplitter_);
    workspace_->fillPanelsMenu = [this](QMenu* menu) { fillPanelsMenu(menu); };
    buildDocks();
    leftRail_ = new AnalysisDockRail(false, this);
    rightRail_ = new AnalysisDockRail(true, this);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(leftRail_);
    body->addWidget(workspace_, 1);
    body->addWidget(rightRail_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addLayout(body, 1);

    layoutSaveTimer_ = new QTimer(this);
    layoutSaveTimer_->setSingleShot(true);
    layoutSaveTimer_->setInterval(400);
    connect(layoutSaveTimer_, &QTimer::timeout, this, [this] {
        recordArrangement();
        saveSettings();
    });
    for (QDockWidget* dock : {lapsDock_, metricsDock_, zonesDock_}) dock->installEventFilter(this);
    connect(zones_, &AnalysisBrakingZones::splitChanged, layoutSaveTimer_, qOverload<>(&QTimer::start));

    loadSettings();
    restoreLayout();
    settingsReady_ = true;

    // ── Wiring ──
    connect(seriesModel_, &AnalysisSeriesModel::seriesEdited, this, [this] {
        saveSettings();
        refreshMetricActions();
        applyState();
    });
    connect(seriesDelegate_, &AnalysisSeriesDelegate::swatchClicked, this,
            [this](const QModelIndex& index, const QString& part) {
                chooseSeriesColor(index.row(), part);
            });
    connect(seriesView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &AnalysisPage::refreshMetricActions);

    connect(modeTabs_, &UnderlineTabBar::tabClicked, this, [this](int index) {
        showSlotPage(index);
        applyState();
    });
    connect(compareSlot_, &AnalysisLapSlot::driverActivated, this, [this] {
        compareSlot_->clearLap();
        refreshLapChoices();
    });
    for (AnalysisLapSlot* slot : {lapASlot_, lapBSlot_})
        connect(slot, &AnalysisLapSlot::driverActivated, this, [this, slot] {
            slot->clearLap();
            refreshLapChoices();
        });
    for (AnalysisLapSlot* slot : {compareSlot_, lapASlot_, lapBSlot_})
        connect(slot, &AnalysisLapSlot::lapActivated, this, &AnalysisPage::applyState);
    for (AnalysisLapSlot* slot : {currentSlot_, compareSlot_, lapASlot_, lapBSlot_})
        connect(slot, &AnalysisLapSlot::labelEdited, this, [this] {
            saveSettings();
            applyState();
        });
    for (AnalysisLapSlot* slot : {currentSlot_, compareSlot_, lapASlot_, lapBSlot_})
        connect(slot, &AnalysisLapSlot::expandedChanged, this, &AnalysisPage::fitLapPanel);
    for (AnalysisLapSlot* slot : {currentSlot_, lapASlot_})
        connect(slot, &AnalysisLapSlot::colorPicked, this, &AnalysisPage::setPrimaryColor);
    for (AnalysisLapSlot* slot : {compareSlot_, lapBSlot_})
        connect(slot, &AnalysisLapSlot::colorPicked, this, &AnalysisPage::setComparisonColor);

    connect(openSecondary_, &QAction::triggered, this, [this] { loadSecondaryFile(); });
    connect(removeSecondary_, &QAction::triggered, this, [this] { clearSecondaryFile(true); });
    connect(secondaryReader_, &AnalysisFileReader::catalogLoaded, this,
            &AnalysisPage::onSecondaryCatalog);
    connect(secondaryReader_, &AnalysisFileReader::loadFailed, this, [this](const QString& reason) {
        secondaryLoading_ = false;
        clearSecondaryFile(false);
        showMessage(reason.isEmpty() ? QStringLiteral("The recording could not be opened.")
                                     : reason);
    });
    connect(secondaryReader_, &AnalysisFileReader::lapDataReady, this,
            [this](uint64_t generation, int driverIndex, int lapNum, uint32_t mask,
                   const std::shared_ptr<PlaybackHistoryBatch>& batch) {
                if (secondary_ && secondary_->generation == generation)
                    installLap(*secondary_, driverIndex, lapNum, mask, batch);
            });

    connect(chart_, &ChartView::inspectionRequested, this,
            [this](double coordinate, bool distance) { inspectMap(coordinate, distance); });
    connect(map_, &AnalysisMapView::cursorElapsedChanged, chart_, &AnalyzeChart::setMapCursorElapsed);
    // A chosen braking zone puts the cursors at its braking point, without
    // changing the view.
    connect(zones_, &AnalysisBrakingZones::zoneActivated, this,
            [this](double distanceM) { inspectMap(distanceM, true, false); });
    connect(contentSplitter_, &AnalysisSplitter::ratioChanged, this, [this](double ratio) {
        settings_.setValue("analyze/splitRatio", ratio);
    });
    connect(model_, &SessionModel::lapsChanged, this, &AnalysisPage::refreshLapChoices);
    connect(model_, &SessionModel::chartConfigurationChanged, this, &AnalysisPage::applyState);

    refreshSecondaryRow();
    refreshMetricActions();
    refreshDriverChoices();
    refreshLapChoices();
    fitLapPanel();
    refreshRails();
}

void AnalysisPage::showSlotPage(int index) {
    for (int page = 0; page < slotPages_.size(); ++page) slotPages_[page]->setVisible(page == index);
    fitLapPanel();
}

QString AnalysisPage::stageComparison(const QString& driverA, int lapA, const QString& labelA,
                                      const QString& driverB, int lapB, const QString& labelB,
                                      const QStringList& metricIds) {
    if (!playback_ || !primary_) return QStringLiteral("no recording is open");
    auto refOf = [this](const QString& name) {
        for (int index : primary_->driverOrder())
            if (const auto* driver = primary_->driver(index);
                driver && driver->name.compare(name, Qt::CaseInsensitive) == 0)
                return AnalysisDriverRef{true, false, index};
        return AnalysisDriverRef{};
    };
    const AnalysisDriverRef a = refOf(driverA);
    const AnalysisDriverRef b = refOf(driverB);
    if (!a.valid) return QStringLiteral("driver %1 not in the recording").arg(driverA);
    if (!b.valid) return QStringLiteral("driver %1 not in the recording").arg(driverB);

    modeTabs_->setCurrentIndex(kFixedTab);
    showSlotPage(kFixedTab);
    refreshDriverChoices();
    lapASlot_->selectDriver(a);
    lapBSlot_->selectDriver(b);
    refreshLapChoices();
    if (!lapASlot_->selectLap(lapA)) return QStringLiteral("%1 has no lap %2").arg(driverA).arg(lapA);
    if (!lapBSlot_->selectLap(lapB)) return QStringLiteral("%1 has no lap %2").arg(driverB).arg(lapB);
    lapASlot_->setLabel(labelA);
    lapBSlot_->setLabel(labelB);

    // Exactly these metrics, in this order: drop the rest (delta always stays
    // first), then append the wanted ones.
    seriesModel_->setMetricsSelected(seriesModel_->selectedIds().values(), false);
    seriesModel_->setMetricsSelected(metricIds, true);

    preferredView_ = View::Split;
    saveSettings();
    applyState();
    return {};
}

void AnalysisPage::focusMapElapsed(double seconds) {
    if (map_) map_->focusElapsed(seconds);
}

// The lap panel may not grow past its content, so it scrolls only when part of
// it is actually cut off. Measured once layouts have settled after a change.
void AnalysisPage::fitLapPanel() {
    // While the dock catches up with its content their heights disagree, which
    // would flash the scroll bar. It returns below if the dock could not fit.
    if (resizePanelsAction_->isChecked()) lapScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QTimer::singleShot(0, this, [this] {
        // Showing a widget refreshes the layouts above it at once; hiding one
        // only reaches them through later events, so mark them all stale here
        // or a collapse would be measured at its old height.
        QWidget* panel = lapsDock_->widget();
        for (QWidget* child : panel->findChildren<QWidget*>()) child->updateGeometry();
        if (QLayout* layout = panel->layout()) layout->activate();
        if (QLayout* layout = lapScroll_->widget()->layout()) layout->activate();
        fitLapsDock();
        // resizeDocks only schedules the new sizes; apply them before the
        // scroll bar is allowed to judge the fit.
        if (QLayout* layout = workspace_->layout()) layout->activate();
        lapScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    });
}

// Resize Panels as needed: the Laps dock takes the height of its content, and
// the panel stacked with it gives or takes the difference.
void AnalysisPage::fitLapsDock() {
    if (!resizePanelsAction_->isChecked() || !lapsDock_->isVisible()) return;
    // A tab group shares one height.
    if (!workspace_->tabifiedDockWidgets(lapsDock_).isEmpty()) return;
    const int wanted = lapsDock_->height() - lapScroll_->viewport()->height() + lapScroll_->widget()->sizeHint().height();
    if (wanted == lapsDock_->height()) return;
    if (lapsDock_->isFloating()) {
        lapsDock_->resize(lapsDock_->width(), wanted);
        return;
    }

    const Qt::DockWidgetArea area = workspace_->dockWidgetArea(lapsDock_);
    const QRect lapsRect = lapsDock_->geometry();
    int remaining = wanted - lapsDock_->height();
    QList<QDockWidget*> docks{lapsDock_};
    QList<int> heights{wanted};
    for (QDockWidget* other : {metricsDock_, zonesDock_}) {
        if (other->isFloating() || !other->isVisible() || workspace_->dockWidgetArea(other) != area) continue;
        const QRect otherRect = other->geometry();
        if (otherRect.right() < lapsRect.left() || otherRect.left() > lapsRect.right()) continue;   // beside Laps
        const int height = qMax(other->height() - remaining, other->minimumSizeHint().height());
        remaining -= other->height() - height;
        docks << other;
        heights << height;
    }
    if (docks.size() == 1) return;   // Laps already fills its column
    heights[0] -= remaining;         // what the others could not give up
    workspace_->resizeDocks(docks, heights, Qt::Vertical);
}

AnalysisPage::~AnalysisPage() {
    // The last panel sizes, in case a save was still pending.
    if (layoutSaveTimer_ && layoutSaveTimer_->isActive()) saveSettings();
}

bool AnalysisPage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize && settingsReady_ && layoutSaveTimer_ &&
        (watched == lapsDock_ || watched == metricsDock_ || watched == zonesDock_))
        layoutSaveTimer_->start();
    return QWidget::eventFilter(watched, event);
}

void AnalysisPage::hideEvent(QHideEvent* event) {
    if (layoutSaveTimer_ && layoutSaveTimer_->isActive()) {
        layoutSaveTimer_->stop();
        saveSettings();
    }
    QWidget::hideEvent(event);
}

void AnalysisPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (lapsFittedOnShow_) return;
    lapsFittedOnShow_ = true;
    fitLapPanel();
}

// ── Construction ────────────────────────────────────────────────────────────

void AnalysisPage::buildToolBar() {
    // The view switch and the panel controls sit in the app tool bar
    // (MainWindow hands them to AppToolbar::setAnalyzePageWidgets).
    const int iconSize = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    auto controlRow = [this] {
        auto* row = new QWidget(this);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(2, 0, 2, 0);
        layout->setSpacing(2);
        return row;
    };
    auto addButton = [iconSize](QWidget* row, QAction* action, Qt::ToolButtonStyle style) {
        auto* button = new AnalysisToggleButton(row);
        button->setDefaultAction(action);
        button->setToolButtonStyle(style);
        button->setAutoRaise(true);
        button->setIconSize(QSize(iconSize, iconSize));
        row->layout()->addWidget(button);
        return button;
    };
    viewControls_ = controlRow();
    panelControls_ = controlRow();

    viewGroup_ = new QActionGroup(this);
    viewGroup_->setExclusive(true);
    auto addView = [this, addButton](QAction*& action, std::initializer_list<const char*> icons,
                                     QStyle::StandardPixmap fallback, const QString& text,
                                     const QString& help, View view) {
        action = makeToggle(this, themed(this, icons, fallback), text, help);
        action->setData(static_cast<int>(view));
        viewGroup_->addAction(action);
        addButton(viewControls_, action, Qt::ToolButtonTextBesideIcon);
    };
    addView(graphsAction_, {"office-chart-line", "labplot-xy-curve"}, QStyle::SP_FileDialogContentsView,
            QStringLiteral("Graphs"), QStringLiteral("Plots the selected metrics of both laps."),
            View::Graphs);
    addView(splitAction_, {"view-split-left-right"}, QStyle::SP_FileDialogListView,
            QStringLiteral("Split"), QStringLiteral("Graphs and the circuit map side by side."),
            View::Split);
    addView(mapAction_, {"map-flat", "globe"}, QStyle::SP_DriveNetIcon, QStringLiteral("Map"),
            QStringLiteral("Both laps as cars on the circuit map."), View::Map);

    // ── Graph strip actions (buildGraphPane) ──
    // Each action's iconText is its short label under the strip icon; its text
    // names it in tooltips, menus and Help.
    auto strip = [](QAction* action, const QString& label, const QKeySequence& key = {}) {
        action->setIconText(label);
        if (!key.isEmpty()) {
            action->setShortcut(key);
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            action->setToolTip(QStringLiteral("%1 (%2)").arg(action->text(),
                                                            key.toString(QKeySequence::NativeText)));
        }
    };
    auto plain = [this](std::initializer_list<const char*> icons, QStyle::StandardPixmap fallback,
                        const QString& text) {
        auto* action = new QAction(themed(this, icons, fallback), text, this);
        action->setToolTip(text);
        return action;
    };
    zoomInAction_ = plain({"zoom-in"}, QStyle::SP_ArrowUp, QStringLiteral("Zoom In"));
    zoomOutAction_ = plain({"zoom-out"}, QStyle::SP_ArrowDown, QStringLiteral("Zoom Out"));
    resetZoomAction_ = plain({"zoom-original", "zoom-fit-best"}, QStyle::SP_BrowserReload,
                             QStringLiteral("Reset Zoom"));
    strip(zoomInAction_, QStringLiteral("Zoom In"), QKeySequence(Qt::CTRL | Qt::Key_Plus));
    strip(zoomOutAction_, QStringLiteral("Zoom Out"), QKeySequence(Qt::CTRL | Qt::Key_Minus));
    strip(resetZoomAction_, QStringLiteral("Reset"), QKeySequence(Qt::CTRL | Qt::Key_0));

    stackedAction_ = makeToggle(this, themed(this, {"view-split-top-bottom"}, QStyle::SP_TitleBarShadeButton),
                                QStringLiteral("Individual Graphs"),
                                QStringLiteral("Gives every metric its own graph instead of "
                                               "overlaying them all on one."));
    syncedTooltipAction_ = makeToggle(this, themed(this, {"link", "insert-link"}, QStyle::SP_CommandLink),
                                      QStringLiteral("Synced Tooltips"),
                                      QStringLiteral("Hovering one graph shows the values of every "
                                                     "graph at that point."));
    sectorBoundariesAction_ = makeToggle(this, themed(this, {"distribute-horizontal-x", "distribute-horizontal"},
                                                      QStyle::SP_ToolBarVerticalExtensionButton),
                                         QStringLiteral("Sector Boundaries"),
                                         QStringLiteral("Labels the X axis by sector instead of distance."));
    sectorDeltaAction_ = makeToggle(this, themed(this, {"office-chart-bar"}, QStyle::SP_FileDialogInfoView),
                                    QStringLiteral("Sector Delta"),
                                    QStringLiteral("Restarts the delta at every sector instead of "
                                                   "accumulating it over the lap."));
    splitCursorsAction_ = makeToggle(this, themed(this, {"distribute-horizontal-center", "object-columns"},
                                                  QStyle::SP_ToolBarHorizontalExtensionButton),
                                     QStringLiteral("Split Cursors"),
                                     QStringLiteral("In Split Mode while comparing, shows where both "
                                                    "cars are on the lap as lines on the graphs."));
    strip(stackedAction_, QStringLiteral("Stacked"));
    strip(syncedTooltipAction_, QStringLiteral("Sync"));
    strip(sectorBoundariesAction_, QStringLiteral("Sectors"));
    strip(sectorDeltaAction_, QStringLiteral("Sector Δ"));
    strip(splitCursorsAction_, QStringLiteral("Cursors"));

    // ── Map strip actions (buildMapPane) ──
    layerGroup_ = new QActionGroup(this);
    layerGroup_->setExclusive(true);
    struct Layer { const char* mode; const char* text; const char* label; const char* icon; const char* altIcon;
                   QStyle::StandardPixmap fallback; const char* help; };
    const Layer layers[] = {
        {"none", "No Layer", "None", "draw-eraser", "edit-clear", QStyle::SP_DialogCancelButton,
         "Draws the track plain."},
        {"dominance", "Track Dominance", "Dominance", "flag", "flag-green", QStyle::SP_DialogApplyButton,
         "Colours each mini-sector by the lap that was faster through it."},
        {"inputs", "Driver Inputs", "Inputs", "input-gaming", "games-config-options", QStyle::SP_ComputerIcon,
         "Each lap's own map, coloured by braking, coasting, partial and full throttle."},
        {"speed", "Speed", "Speed", "speedometer", "chronometer", QStyle::SP_MediaSeekForward,
         "Each lap's own map, coloured by speed on one scale for both."},
        {"ers", "ERS Usage", "ERS", "battery-good", "battery", QStyle::SP_MediaVolume,
         "Each lap's own map, coloured by MGU-K deployment against the car's peak."},
        {"harvest", "ERS Harvest", "Harvest", "battery-070-charging", "battery-good-charging",
         QStyle::SP_ArrowDown, "Each lap's own map, coloured by energy harvested."},
    };
    for (const Layer& layer : layers) {
        QAction* action = makeToggle(this, themed(this, {layer.icon, layer.altIcon}, layer.fallback),
                                     QString::fromUtf8(layer.text), QString::fromUtf8(layer.help));
        action->setData(QString::fromLatin1(layer.mode));
        action->setIconText(QString::fromUtf8(layer.label));
        layerGroup_->addAction(action);
    }
    layerGroup_->actions().first()->setChecked(true);

    inputsAction_ = makeToggle(this, themed(this, {"object-columns", "view-list-details"},
                                            QStyle::SP_FileDialogDetailedView),
                               QStringLiteral("Data Comparison"),
                               QStringLiteral("Shows a card on the map with both laps' steering, "
                                              "pedals, ERS, speed and gear at the map cursor."));
    brakingAction_ = makeToggle(this, themed(this, {"media-optical", "drive-optical"},
                                             QStyle::SP_DriveCDIcon),
                                QStringLiteral("Braking Points"),
                                QStringLiteral("Highlights each braking zone on the map; hover one "
                                               "for both laps' braking, corners and exit."));
    strip(brakingAction_, QStringLiteral("Braking"), QKeySequence(Qt::Key_B));
    strip(inputsAction_, QStringLiteral("Data"), QKeySequence(Qt::Key_D));

    // ── Panels ──
    edgeTabsAction_ = new QAction(QStringLiteral("Closed Panels as Edge Tabs"), this);
    edgeTabsAction_->setCheckable(true);
    edgeTabsAction_->setChecked(true);
    lockPanelsAction_ = new QAction(QStringLiteral("Lock Panels"), this);
    lockPanelsAction_->setCheckable(true);
    resizePanelsAction_ = new QAction(QStringLiteral("Resize Panels as needed"), this);
    resizePanelsAction_->setCheckable(true);
    resizePanelsAction_->setChecked(true);
    resetLayoutAction_ = new QAction(themed(this, {"view-restore", "edit-undo"}, QStyle::SP_BrowserReload),
                                     QStringLiteral("Reset Layout"), this);

    panelsButton_ = new QToolButton(panelControls_);
    panelsButton_->setText(QStringLiteral("Panels"));
    panelsButton_->setIcon(themed(this, {"view-split-left-right", "window"}, QStyle::SP_TitleBarNormalButton));
    panelsButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    panelsButton_->setPopupMode(QToolButton::InstantPopup);
    panelsButton_->setAutoRaise(true);
    panelsButton_->setIconSize(QSize(iconSize, iconSize));
    panelsButton_->setToolTip(QStringLiteral("Show, hide and arrange the panels"));
    auto* panelsMenu = new QMenu(panelsButton_);
    connect(panelsMenu, &QMenu::aboutToShow, this, [this, panelsMenu] {
        panelsMenu->clear();
        fillPanelsMenu(panelsMenu);
    });
    panelsButton_->setMenu(panelsMenu);
    panelControls_->layout()->addWidget(panelsButton_);

    helpAction_ = new QAction(themed(this, {"help-contextual", "help-about"}, QStyle::SP_MessageBoxQuestion),
                              QStringLiteral("Analysis Help"), this);
    helpAction_->setToolTip(QStringLiteral("What the Analysis controls do"));
    addButton(panelControls_, helpAction_, Qt::ToolButtonIconOnly);

    for (QAction* action : {zoomInAction_, zoomOutAction_, resetZoomAction_, brakingAction_, inputsAction_})
        addAction(action);   // their shortcuts work anywhere on the page
    connect(zoomInAction_, &QAction::triggered, this, &AnalysisPage::zoomIn);
    connect(zoomOutAction_, &QAction::triggered, this, &AnalysisPage::zoomOut);
    connect(resetZoomAction_, &QAction::triggered, this, &AnalysisPage::resetZoom);
    connect(viewGroup_, &QActionGroup::triggered, this, [this](QAction* action) {
        preferredView_ = static_cast<View>(action->data().toInt());
        saveSettings();
        applyState();
    });
    connect(sectorBoundariesAction_, &QAction::toggled, this, [this](bool on) {
        if (!on) sectorDeltaAction_->setChecked(false);
    });
    for (QAction* action : {stackedAction_, syncedTooltipAction_, sectorBoundariesAction_,
                            sectorDeltaAction_, splitCursorsAction_, inputsAction_, brakingAction_})
        connect(action, &QAction::triggered, this, [this] {
            saveSettings();
            applyState();
        });
    connect(layerGroup_, &QActionGroup::triggered, this, [this] {
        saveSettings();
        applyState();
    });
    connect(edgeTabsAction_, &QAction::triggered, this, [this] {
        saveSettings();
        refreshRails();
    });
    connect(lockPanelsAction_, &QAction::triggered, this, [this] {
        saveSettings();
        applyPanelLock();
    });
    connect(resizePanelsAction_, &QAction::triggered, this, [this] {
        saveSettings();
        fitLapPanel();
    });
    connect(resetLayoutAction_, &QAction::triggered, this, &AnalysisPage::resetPanels);
    connect(helpAction_, &QAction::triggered, this, &AnalysisPage::showControlsHelp);
}

// Graph pane: its tool strip on the outer edge, then the result band over the graphs.
QWidget* AnalysisPage::buildGraphPane() {
    auto* pane = new QWidget(this);
    auto* row = new QHBoxLayout(pane);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    graphStrip_ = new AnalysisToolStrip(QStringLiteral("Graph tools"), pane);
    for (QAction* action : {zoomInAction_, zoomOutAction_, resetZoomAction_}) graphStrip_->addTool(action);
    graphStrip_->addSeparator();
    for (QAction* action : {stackedAction_, syncedTooltipAction_, sectorBoundariesAction_,
                            sectorDeltaAction_, splitCursorsAction_})
        graphStrip_->addTool(action);
    row->addWidget(graphStrip_);
    row->addWidget(tnrui::vline());

    auto* column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    deltaReadout_ = new AnalysisDeltaReadout(pane);
    column->addWidget(deltaReadout_);
    column->addWidget(tnrui::hline());
    column->addWidget(chart_, 1);
    row->addLayout(column, 1);
    return pane;
}

// Map pane: the map, then its tool strip on the outer edge.
QWidget* AnalysisPage::buildMapPane() {
    auto* pane = new QWidget(this);
    auto* row = new QHBoxLayout(pane);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(map_, 1);
    row->addWidget(tnrui::vline());

    mapStrip_ = new AnalysisToolStrip(QStringLiteral("Map tools"), pane);
    mapStrip_->addCaption(QStringLiteral("Layer"));
    for (QAction* action : layerGroup_->actions()) mapStrip_->addTool(action);
    mapStrip_->addSeparator();
    mapStrip_->addTool(brakingAction_);
    mapStrip_->addTool(inputsAction_);
    row->addWidget(mapStrip_);
    return pane;
}

// Laps and Metrics on the left, Braking Zones on the right: the default
// layout, also what Reset Layout returns to.
void AnalysisPage::buildDocks() {
    auto makeDock = [this](const QString& title, const QString& name, QWidget* content) {
        auto* dock = new QDockWidget(title, workspace_);
        dock->setObjectName(name);   // saveState keys docks by object name
        dock->setWidget(content);
        dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        QAction* toggle = dock->toggleViewAction();
        toggle->setText(title);
        // Qt shares the space out afresh whenever the arrangement changes; put
        // back the sizes this arrangement had instead.
        connect(toggle, &QAction::toggled, this, [this, dock](bool shown) {
            if (settingsReady_) {
                if (!shown && layoutSaveTimer_->isActive()) {
                    // A resize not yet recorded: take it now, with the closing
                    // panel still counted as open (the layout has not reflowed).
                    layoutSaveTimer_->stop();
                    recordArrangement(dock);
                }
                restoreArrangement();
            }
            refreshRails();
            saveSettings();
        });
        connect(dock, &QDockWidget::dockLocationChanged, this, [this] {
            if (settingsReady_) restoreArrangement();
            refreshRails();
            saveSettings();
        });
        connect(dock, &QDockWidget::topLevelChanged, this, [this] {
            if (settingsReady_) restoreArrangement();
            saveSettings();
        });
        return dock;
    };

    // Laps: the mode tabs on top, the two laps (scrolling on a short window) and
    // the second recording pinned to the bottom.
    auto* lapsPanel = new QWidget(this);
    auto* lapsPanelLayout = new QVBoxLayout(lapsPanel);
    lapsPanelLayout->setContentsMargins(0, 0, 0, 0);
    lapsPanelLayout->setSpacing(0);
    lapsPanelLayout->addWidget(buildModeTabs());
    lapScroll_ = new QScrollArea(lapsPanel);
    lapScroll_->setFrameShape(QFrame::NoFrame);
    lapScroll_->setWidgetResizable(true);
    lapScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* laps = new QWidget(lapScroll_);
    auto* lapsLayout = new QVBoxLayout(laps);
    lapsLayout->setContentsMargins(0, 0, 0, 0);
    lapsLayout->setSpacing(0);
    lapsLayout->addWidget(buildLapGroup());
    lapsLayout->addStretch(1);
    lapScroll_->setWidget(laps);
    lapScroll_->setMinimumWidth(260);
    // A scroll area's own minimum is its scroll bar's length, which is taller
    // than two collapsed laps and would leave a gap under them.
    lapScroll_->setMinimumHeight(1);
    lapsPanelLayout->addWidget(lapScroll_, 1);
    lapsPanelLayout->addWidget(buildRecordingGroup());

    lapsDock_ = makeDock(QStringLiteral("Laps"), QStringLiteral("analysisLapsDock"), lapsPanel);
    metricsDock_ = makeDock(QStringLiteral("Metrics"), QStringLiteral("analysisMetricsDock"),
                            buildMetricGroup());
    zones_ = new AnalysisBrakingZones(this);
    zones_->setMinimumWidth(240);
    zonesDock_ = makeDock(QStringLiteral("Braking Zones"), QStringLiteral("analysisZonesDock"), zones_);
    // The zone list is only worked out while someone can see it.
    connect(zonesDock_->toggleViewAction(), &QAction::toggled, this, &AnalysisPage::requestMapLayers);
    resetPanels();
}

void AnalysisPage::resetPanels() {
    layoutMemory_.clear();
    latestPanelSizes_.clear();
    for (QDockWidget* dock : {lapsDock_, metricsDock_, zonesDock_}) dock->setFloating(false);
    workspace_->addDockWidget(Qt::LeftDockWidgetArea, lapsDock_);
    workspace_->addDockWidget(Qt::LeftDockWidgetArea, metricsDock_);
    workspace_->splitDockWidget(lapsDock_, metricsDock_, Qt::Vertical);
    workspace_->addDockWidget(Qt::RightDockWidgetArea, zonesDock_);
    for (QDockWidget* dock : {lapsDock_, metricsDock_, zonesDock_}) dock->show();
    workspace_->resizeDocks({lapsDock_, zonesDock_}, {300, 280}, Qt::Horizontal);
    workspace_->resizeDocks({lapsDock_, metricsDock_}, {420, 300}, Qt::Vertical);
    refreshRails();
    saveSettings();
    fitLapPanel();
}

QString AnalysisPage::arrangementKey(Qt::DockWidgetArea side, QDockWidget* closing) const {
    QStringList parts;
    for (QDockWidget* dock : {lapsDock_, metricsDock_, zonesDock_}) {
        if (dock->isFloating() || workspace_->dockWidgetArea(dock) != side) continue;
        if (dock != closing && !dock->toggleViewAction()->isChecked()) continue;
        parts << dock->objectName() + QLatin1Char('@') + QString::number(int(side));
    }
    parts.sort();
    return parts.join(QLatin1Char(','));
}

void AnalysisPage::recordArrangement(QDockWidget* closing) {
    QHash<QString, QHash<QString, QSize>> recorded;
    for (QDockWidget* dock : {lapsDock_, metricsDock_, zonesDock_}) {
        if (dock->isFloating()) continue;
        if (dock != closing && !dock->toggleViewAction()->isChecked()) continue;
        // A panel behind another's tab has no size of its own on screen.
        if (!(dock == closing || dock->isVisible()) || dock->width() <= 0 || dock->height() <= 0) continue;
        recorded[arrangementKey(workspace_->dockWidgetArea(dock), closing)].insert(dock->objectName(), dock->size());
        latestPanelSizes_.insert(dock->objectName(), dock->size());
    }
    for (auto it = recorded.cbegin(); it != recorded.cend(); ++it) layoutMemory_.insert(it.key(), it.value());
}

void AnalysisPage::restoreArrangement() {
    // Once the window has laid the panels out again.
    QTimer::singleShot(0, this, [this] {
        if (QLayout* layout = workspace_->layout()) layout->activate();
        QList<QDockWidget*> docks;
        QList<int> widths;
        QList<int> heights;
        for (QDockWidget* dock : {lapsDock_, metricsDock_, zonesDock_}) {
            if (dock->isFloating() || !dock->isVisible()) continue;
            QSize size;
            const auto remembered = layoutMemory_.constFind(arrangementKey(workspace_->dockWidgetArea(dock)));
            if (remembered != layoutMemory_.cend()) size = remembered->value(dock->objectName());
            if (!size.isValid()) size = latestPanelSizes_.value(dock->objectName());
            if (!size.isValid() || size.isEmpty()) continue;
            docks << dock;
            widths << size.width();
            heights << size.height();
        }
        if (docks.isEmpty()) return;
        // resizeDocks moves whichever separators lie between these panels,
        // however they are stacked, side by side or nested.
        workspace_->resizeDocks(docks, widths, Qt::Horizontal);
        workspace_->resizeDocks(docks, heights, Qt::Vertical);
        fitLapsDock();
        layoutSaveTimer_->start();   // records the result for this arrangement
    });
}

void AnalysisPage::fillPanelsMenu(QMenu* menu) {
    menu->addSection(QStringLiteral("Panels"));
    for (QDockWidget* dock : {lapsDock_, metricsDock_, zonesDock_}) menu->addAction(dock->toggleViewAction());
    menu->addSeparator();
    menu->addAction(edgeTabsAction_);
    menu->addAction(lockPanelsAction_);
    menu->addAction(resizePanelsAction_);
    menu->addSeparator();
    menu->addAction(resetLayoutAction_);
}

// A closed dock gets a tab on the edge it was docked to (Laps and Metrics
// default left, Braking Zones right, when it was floating).
void AnalysisPage::refreshRails() {
    if (!leftRail_ || !rightRail_) return;
    QVector<QDockWidget*> left;
    QVector<QDockWidget*> right;
    if (edgeTabsAction_->isChecked()) {
        for (QDockWidget* dock : {lapsDock_, metricsDock_, zonesDock_}) {
            // Closed, not merely behind another panel's tab.
            if (dock->toggleViewAction()->isChecked()) continue;
            Qt::DockWidgetArea area = workspace_->dockWidgetArea(dock);
            if (area != Qt::LeftDockWidgetArea && area != Qt::RightDockWidgetArea)
                area = dock == zonesDock_ ? Qt::RightDockWidgetArea : Qt::LeftDockWidgetArea;
            (area == Qt::RightDockWidgetArea ? right : left).push_back(dock);
        }
    }
    leftRail_->setDocks(left);
    rightRail_->setDocks(right);
}

// Locked panels can still be closed, but not dragged or floated by accident.
void AnalysisPage::applyPanelLock() {
    const QDockWidget::DockWidgetFeatures features = lockPanelsAction_->isChecked()
        ? QDockWidget::DockWidgetClosable
        : QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable;
    for (QDockWidget* dock : {lapsDock_, metricsDock_, zonesDock_}) dock->setFeatures(features);
}

QWidget* AnalysisPage::buildRecordingGroup() {
    recordingGroup_ = new QWidget(this);
    auto* outer = new QVBoxLayout(recordingGroup_);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* divider = new QFrame(recordingGroup_);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    outer->addWidget(divider);
    auto* layout = new QVBoxLayout;
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    outer->addLayout(layout);

    // A read-only field with its browse and close actions inside it.
    secondaryField_ = new QLineEdit(recordingGroup_);
    secondaryField_->setReadOnly(true);
    secondaryField_->setPlaceholderText(QStringLiteral("No second recording"));
    secondaryField_->setAccessibleName(QStringLiteral("Second recording"));
    // Trailing actions stack leftwards, so the one added first sits at the edge.
    removeSecondary_ = secondaryField_->addAction(
        themed(this, {"window-close", "dialog-close"}, QStyle::SP_DialogCloseButton), QLineEdit::TrailingPosition);
    removeSecondary_->setToolTip(QStringLiteral("Close the second recording"));
    openSecondary_ = secondaryField_->addAction(
        themed(this, {"folder-open", "document-open"}, QStyle::SP_DirOpenIcon), QLineEdit::TrailingPosition);
    layout->addWidget(secondaryField_);

    // Inline error, in the spirit of KMessageWidget: stays until dismissed or resolved.
    messageBar_ = new QFrame(recordingGroup_);
    messageBar_->setFrameShape(QFrame::StyledPanel);
    auto* messageLayout = new QHBoxLayout(messageBar_);
    messageLayout->setContentsMargins(6, 4, 2, 4);
    messageLayout->setSpacing(6);
    auto* messageIcon = new QLabel(messageBar_);
    const int iconSize = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    messageIcon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning)
                               .pixmap(iconSize, iconSize));
    messageIcon->setAlignment(Qt::AlignTop);
    messageText_ = new QLabel(messageBar_);
    messageText_->setWordWrap(true);
    messageText_->setTextFormat(Qt::PlainText);
    auto* dismiss = new QToolButton(messageBar_);
    dismiss->setAutoRaise(true);
    dismiss->setIcon(themed(this, {"window-close", "dialog-close"}, QStyle::SP_DialogCloseButton));
    dismiss->setToolTip(QStringLiteral("Dismiss"));
    messageLayout->addWidget(messageIcon, 0, Qt::AlignTop);
    messageLayout->addWidget(messageText_, 1);
    messageLayout->addWidget(dismiss, 0, Qt::AlignTop);
    messageBar_->hide();
    layout->addWidget(messageBar_);
    connect(dismiss, &QToolButton::clicked, messageBar_, &QWidget::hide);
    connect(dismiss, &QToolButton::clicked, this, &AnalysisPage::fitLapPanel);

    return recordingGroup_;
}

QWidget* AnalysisPage::buildModeTabs() {
    modeTabs_ = new UnderlineTabBar(this);
    modeTabs_->addTab(QStringLiteral("Follow Playback"));   // kPlaybackTab
    modeTabs_->addTab(QStringLiteral("Fixed Laps"));        // kFixedTab
    modeTabs_->setTinted(false);
    modeTabs_->setBottomBorder(true);
    modeTabs_->setTabToolTip(kPlaybackTab, QStringLiteral("Compare the lap under the playback cursor with a chosen lap"));
    modeTabs_->setTabToolTip(kFixedTab, QStringLiteral("Compare any two laps, from either recording"));
    return modeTabs_;
}

QWidget* AnalysisPage::buildLapGroup() {
    auto* group = new QWidget(this);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Both pages live in one layout and only the active one is shown, so the
    // group is always exactly as tall as the page on screen.
    auto makePage = [this, layout, group](AnalysisLapSlot* first, AnalysisLapSlot* second) {
        auto* page = new QWidget(group);
        auto* pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        pageLayout->setSpacing(0);
        pageLayout->addWidget(first);
        pageLayout->addWidget(second);
        slotPages_.push_back(page);
        layout->addWidget(page);
    };
    currentSlot_ = new AnalysisLapSlot(QStringLiteral("Current Lap"), QStringLiteral("Current"),
                                       AnalysisLapSlot::Mode::FollowsPlayback);
    compareSlot_ = new AnalysisLapSlot(QStringLiteral("Compare With"), QStringLiteral("Compare"),
                                       AnalysisLapSlot::Mode::Selectable);
    lapASlot_ = new AnalysisLapSlot(QStringLiteral("Lap A"), QStringLiteral("Lap A"),
                                    AnalysisLapSlot::Mode::Selectable);
    lapBSlot_ = new AnalysisLapSlot(QStringLiteral("Lap B"), QStringLiteral("Lap B"),
                                    AnalysisLapSlot::Mode::Selectable);
    // The followed lap has nothing to choose, so Compare starts open.
    currentSlot_->setExpanded(false);
    lapBSlot_->setExpanded(false);
    makePage(currentSlot_, compareSlot_);
    makePage(lapASlot_, lapBSlot_);
    slotPages_[kFixedTab]->hide();
    return group;
}

QWidget* AnalysisPage::buildMetricGroup() {
    // The dock's title names it, so no group box title of its own.
    auto* group = new QWidget(this);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    seriesView_ = new QListView(group);
    seriesView_->setModel(seriesModel_);
    seriesDelegate_ = new AnalysisSeriesDelegate(seriesView_);
    seriesView_->setItemDelegate(seriesDelegate_);
    seriesView_->setSelectionMode(QAbstractItemView::SingleSelection);
    seriesView_->setDragDropMode(QAbstractItemView::InternalMove);
    seriesView_->setDefaultDropAction(Qt::MoveAction);
    seriesView_->setDragDropOverwriteMode(false);
    seriesView_->setDropIndicatorShown(true);
    seriesView_->setUniformItemSizes(true);
    seriesView_->setContextMenuPolicy(Qt::CustomContextMenu);
    seriesView_->setMinimumHeight(140);
    seriesView_->setAccessibleName(QStringLiteral("Charted metrics"));
    layout->addWidget(seriesView_, 1);

    // Actions on the selected metric, shared by the buttons and the context menu.
    auto makeAction = [this](std::initializer_list<const char*> icons, QStyle::StandardPixmap fallback,
                             const QString& text) {
        auto* action = new QAction(themed(this, icons, fallback), text, this);
        action->setToolTip(text);
        return action;
    };
    removeMetricAction_ = makeAction({"list-remove"}, QStyle::SP_TrashIcon, QStringLiteral("Remove Metric"));
    removeMetricAction_->setShortcut(QKeySequence::Delete);
    removeMetricAction_->setShortcutContext(Qt::WidgetShortcut);
    seriesView_->addAction(removeMetricAction_);
    moveUpAction_ = makeAction({"go-up"}, QStyle::SP_ArrowUp, QStringLiteral("Move Up"));
    moveDownAction_ = makeAction({"go-down"}, QStyle::SP_ArrowDown, QStringLiteral("Move Down"));
    changeColorAction_ = makeAction({"color-management", "color-picker"}, QStyle::SP_DesktopIcon,
                                    QStringLiteral("Change Colour…"));
    resetColorAction_ = makeAction({"edit-undo", "view-refresh"}, QStyle::SP_BrowserReload,
                                   QStringLiteral("Reset Colour"));
    yAxisAction_ = new QAction(QStringLiteral("Show Y Axis"), this);
    yAxisAction_->setCheckable(true);
    allYAxesAction_ = new QAction(QStringLiteral("Show All Y Axes"), this);
    allYAxesAction_->setCheckable(true);

    picker_ = new AnalysisMetricPicker(seriesModel_, this);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    auto* add = new QToolButton(group);
    add->setIcon(themed(this, {"list-add"}, QStyle::SP_FileDialogNewFolder));
    add->setText(QStringLiteral("Add"));
    add->setToolTip(QStringLiteral("Choose the metrics to chart"));
    add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(add, &QToolButton::clicked, this, [this, add] { picker_->popup(add); });
    buttons->addWidget(add);
    buttons->addStretch(1);
    for (QAction* action : {moveUpAction_, moveDownAction_, changeColorAction_, removeMetricAction_}) {
        auto* button = new QToolButton(group);
        button->setDefaultAction(action);
        button->setAutoRaise(true);
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);

    connect(removeMetricAction_, &QAction::triggered, this, [this] {
        seriesModel_->removeRowAt(currentSeriesRow());
    });
    auto move = [this](int delta) {
        const int row = currentSeriesRow();
        if (row < 0) return;
        const int target = row + delta;
        if (target < 0 || target >= seriesModel_->rowCount()) return;
        seriesModel_->moveRow(QModelIndex(), row, QModelIndex(), delta > 0 ? target + 1 : target);
        seriesView_->setCurrentIndex(seriesModel_->index(target));
    };
    connect(moveUpAction_, &QAction::triggered, this, [move] { move(-1); });
    connect(moveDownAction_, &QAction::triggered, this, [move] { move(1); });
    connect(changeColorAction_, &QAction::triggered, this, [this] {
        chooseSeriesColor(currentSeriesRow(), {});
    });
    connect(resetColorAction_, &QAction::triggered, this, [this] {
        seriesModel_->resetColors(currentSeriesRow());
    });
    connect(yAxisAction_, &QAction::triggered, this, [this](bool on) {
        seriesModel_->setShowYAxis(currentSeriesRow(), on);
    });
    connect(allYAxesAction_, &QAction::triggered, this, [this](bool on) {
        seriesModel_->setAllYAxes(on);
    });
    connect(seriesView_, &QListView::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = seriesView_->indexAt(pos);
        if (index.isValid()) seriesView_->setCurrentIndex(index);
        refreshMetricActions();
        QMenu menu(this);
        if (index.isValid()) {
            const bool delta = !index.data(AnalysisSeriesModel::RemovableRole).toBool();
            const int row = index.row();
            if (delta) {
                menu.addAction(QStringLiteral("Slower Colour…"), this,
                               [this, row] { chooseSeriesColor(row, {}); });
                menu.addAction(QStringLiteral("Faster Colour…"), this, [this, row] {
                    chooseSeriesColor(row, AnalysisSeriesDelegate::negativePart());
                });
            } else if (index.data(AnalysisSeriesModel::CornerColorsRole).isValid()) {
                menu.addAction(QStringLiteral("Axis Colour…"), this,
                               [this, row] { chooseSeriesColor(row, {}); });
                QMenu* corners = menu.addMenu(QStringLiteral("Corner Colour"));
                for (const AnalyzeTyreCorner& corner : analyzeTyreCorners())
                    corners->addAction(corner.label + QStringLiteral("…"), this,
                                       [this, row, key = corner.key] { chooseSeriesColor(row, key); });
            } else {
                menu.addAction(changeColorAction_);
            }
            menu.addAction(resetColorAction_);
            menu.addSeparator();
            menu.addAction(yAxisAction_);
            menu.addSeparator();
            menu.addAction(moveUpAction_);
            menu.addAction(moveDownAction_);
            if (!delta) {
                menu.addSeparator();
                menu.addAction(removeMetricAction_);
            }
            menu.addSeparator();
        }
        menu.addAction(allYAxesAction_);
        menu.exec(seriesView_->viewport()->mapToGlobal(pos));
    });
    return group;
}

// ── Settings ────────────────────────────────────────────────────────────────

void AnalysisPage::loadSettings() {
    const QString view = settings_.value("analyze/view", "graph").toString();
    preferredView_ = view == QLatin1String("split") ? View::Split
                   : view == QLatin1String("map")   ? View::Map
                                                    : View::Graphs;
    const QColor primary(settings_.value("analyze/mapCurrentColor", "#5794F2").toString());
    const QColor comparison(settings_.value("analyze/mapComparisonColor", "#C4162A").toString());
    if (primary.isValid()) primaryColor_ = primary;
    if (comparison.isValid()) comparisonColor_ = comparison;
    for (AnalysisLapSlot* slot : {currentSlot_, lapASlot_}) slot->setColor(primaryColor_);
    for (AnalysisLapSlot* slot : {compareSlot_, lapBSlot_}) slot->setColor(comparisonColor_);

    // Earlier versions stored the default names verbatim; those mean "no custom name".
    auto loadLabel = [this](const char* key, const QString& legacyDefault) {
        const QString value = settings_.value(QString::fromLatin1(key)).toString().left(40);
        return value == legacyDefault ? QString() : value;
    };
    currentSlot_->setLabel(loadLabel("analyze/currentLabel", QStringLiteral("Current")));
    compareSlot_->setLabel(loadLabel("analyze/compareLabel", QStringLiteral("Compare")));
    lapASlot_->setLabel(loadLabel("analyze/lapALabel", QStringLiteral("Lap A")));
    lapBSlot_->setLabel(loadLabel("analyze/lapBLabel", QStringLiteral("Lap B")));

    const auto setChecked = [](QAction* action, bool on) {
        QSignalBlocker guard(action);
        action->setChecked(on);
    };
    setChecked(stackedAction_, settings_.value("analyze/individualGraphs", false).toBool());
    setChecked(syncedTooltipAction_, settings_.value("analyze/syncedTooltip", false).toBool());
    const bool boundaries = settings_.value("analyze/sectorBoundaries", false).toBool();
    setChecked(sectorBoundariesAction_, boundaries);
    setChecked(sectorDeltaAction_, boundaries && settings_.value("analyze/sectorDelta", false).toBool());
    setChecked(splitCursorsAction_, settings_.value("analyze/splitCursors", true).toBool());
    setChecked(inputsAction_, settings_.value("analyze/inputsPanel", true).toBool());
    setChecked(brakingAction_, settings_.value("analyze/brakingPoints", false).toBool());
    {
        const QString layer = settings_.value("analyze/mapColoring", "none").toString();
        QAction* chosen = layerGroup_->actions().first();
        for (QAction* action : layerGroup_->actions())
            if (action->data().toString() == layer) chosen = action;
        QSignalBlocker guard(chosen);
        chosen->setChecked(true);
    }
    setChecked(edgeTabsAction_, settings_.value("analyze/edgeTabs", true).toBool());
    setChecked(lockPanelsAction_, settings_.value("analyze/lockPanels", false).toBool());
    setChecked(resizePanelsAction_, settings_.value("analyze/resizePanels", true).toBool());

    const bool defaultAxis = settings_.value("analyze/showYAxis", true).toBool();
    const QJsonDocument doc = QJsonDocument::fromJson(settings_.value("analyze/series").toByteArray());
    QVector<AnalyzeSeriesSetting> series;
    if (doc.isArray()) {
        for (const QJsonValue& value : doc.array()) {
            const QJsonObject object = value.toObject();
            AnalyzeSeriesSetting setting;
            setting.metricId = object["metricId"].toString();
            setting.visible = object["visible"].toBool(true);
            setting.showYAxis = object.contains("showYAxis") ? object["showYAxis"].toBool() : defaultAxis;
            const QColor color(object["color"].toString());
            if (setting.metricId == QLatin1String("delta")) {
                const QColor negative(object["negativeColor"].toString());
                setting.color = color.isValid() ? color : AnalysisSeriesModel::kDeltaPositive;
                setting.negativeColor = negative.isValid() ? negative : AnalysisSeriesModel::kDeltaNegative;
            } else if (const AnalyzeTyreRow* combined = analyzeCombinedRow(setting.metricId)) {
                setting.color = color.isValid() ? color : combined->combinedColor;
                // A combined series saved without a corner list draws all four.
                if (object.contains("corners")) {
                    for (const QJsonValue& corner : object["corners"].toArray())
                        setting.corners << corner.toString();
                } else {
                    for (const AnalyzeTyreCorner& corner : analyzeTyreCorners())
                        setting.corners << corner.key;
                }
                const QJsonObject colors = object["cornerColors"].toObject();
                for (auto it = colors.constBegin(); it != colors.constEnd(); ++it) {
                    const QColor cornerColor(it.value().toString());
                    if (cornerColor.isValid()) setting.cornerColors.insert(it.key(), cornerColor);
                }
            } else if (const AnalyzeMetric* metric = analyzeMetric(setting.metricId)) {
                setting.color = color.isValid() ? color : metric->defaultColor;
            } else {
                continue;
            }
            series.push_back(setting);
        }
    } else {
        for (const char* id : {"speed", "rpm", "ers"}) {
            const AnalyzeMetric* metric = analyzeMetric(QString::fromLatin1(id));
            series.push_back({metric->id, metric->defaultColor, QColor(), true, defaultAxis});
        }
    }
    seriesModel_->setSeries(series);
}

void AnalysisPage::saveSettings() {
    if (!settingsReady_) return;
    settings_.setValue("analyze/version", kSettingsVersion);
    settings_.setValue("analyze/view", viewKey(static_cast<int>(preferredView_)));
    settings_.setValue("analyze/individualGraphs", stackedAction_->isChecked());
    settings_.setValue("analyze/syncedTooltip", syncedTooltipAction_->isChecked());
    settings_.setValue("analyze/sectorBoundaries", sectorBoundariesAction_->isChecked());
    settings_.setValue("analyze/sectorDelta", sectorDeltaAction_->isChecked());
    settings_.setValue("analyze/splitCursors", splitCursorsAction_->isChecked());
    settings_.setValue("analyze/inputsPanel", inputsAction_->isChecked());
    settings_.setValue("analyze/brakingPoints", brakingAction_->isChecked());
    settings_.setValue("analyze/mapColoring", layerMode());
    settings_.setValue("analyze/edgeTabs", edgeTabsAction_->isChecked());
    settings_.setValue("analyze/lockPanels", lockPanelsAction_->isChecked());
    settings_.setValue("analyze/resizePanels", resizePanelsAction_->isChecked());
    settings_.setValue("analyze/showYAxis", seriesModel_->allYAxesShown());
    settings_.setValue("analyze/mapCurrentColor", primaryColor_.name());
    settings_.setValue("analyze/mapComparisonColor", comparisonColor_.name());
    settings_.setValue("analyze/currentLabel", currentSlot_->label());
    settings_.setValue("analyze/compareLabel", compareSlot_->label());
    settings_.setValue("analyze/lapALabel", lapASlot_->label());
    settings_.setValue("analyze/lapBLabel", lapBSlot_->label());

    QJsonArray series;
    for (const AnalyzeSeriesSetting& setting : seriesModel_->series()) {
        QJsonObject object;
        object["metricId"] = setting.metricId;
        object["color"] = setting.color.name();
        if (setting.metricId == QLatin1String("delta"))
            object["negativeColor"] = setting.negativeColor.name();
        object["visible"] = setting.visible;
        object["showYAxis"] = setting.showYAxis;
        if (analyzeCombinedRow(setting.metricId)) {
            object["corners"] = QJsonArray::fromStringList(setting.corners);
            QJsonObject colors;
            for (auto it = setting.cornerColors.constBegin(); it != setting.cornerColors.constEnd(); ++it)
                colors[it.key()] = it.value().name();
            if (!colors.isEmpty()) object["cornerColors"] = colors;
        }
        series.append(object);
    }
    settings_.setValue("analyze/series", QJsonDocument(series).toJson(QJsonDocument::Compact));

    if (workspace_) settings_.setValue("analyze/workspace", workspace_->saveState(kWorkspaceVersion));
    if (zones_) settings_.setValue("analyze/zonesSplit", zones_->splitState());
    QVariantMap memory;
    for (auto it = layoutMemory_.cbegin(); it != layoutMemory_.cend(); ++it) {
        QVariantMap sizes;
        for (auto size = it.value().cbegin(); size != it.value().cend(); ++size) sizes.insert(size.key(), size.value());
        memory.insert(it.key(), sizes);
    }
    settings_.setValue("analyze/sideLayoutMemory", memory);
    QVariantMap latest;
    for (auto it = latestPanelSizes_.cbegin(); it != latestPanelSizes_.cend(); ++it) latest.insert(it.key(), it.value());
    settings_.setValue("analyze/panelSizes", latest);
}

void AnalysisPage::restoreLayout() {
    contentSplitter_->setRatio(settings_.value("analyze/splitRatio", AnalysisSplitter::kDefaultRatio).toDouble());
    // Dock positions, sizes, tabs and which are closed; the default layout
    // (buildDocks) stays when nothing was saved or it cannot be read.
    workspace_->restoreState(settings_.value("analyze/workspace").toByteArray(), kWorkspaceVersion);
    zones_->restoreSplitState(settings_.value("analyze/zonesSplit").toByteArray());
    const QVariantMap memory = settings_.value("analyze/sideLayoutMemory").toMap();
    for (auto it = memory.cbegin(); it != memory.cend(); ++it) {
        const QVariantMap sizes = it.value().toMap();
        QHash<QString, QSize>& arrangement = layoutMemory_[it.key()];
        for (auto size = sizes.cbegin(); size != sizes.cend(); ++size)
            if (const QSize value = size.value().toSize(); value.isValid() && !value.isEmpty())
                arrangement.insert(size.key(), value);
    }
    const QVariantMap latest = settings_.value("analyze/panelSizes").toMap();
    for (auto it = latest.cbegin(); it != latest.cend(); ++it)
        if (const QSize value = it.value().toSize(); value.isValid() && !value.isEmpty())
            latestPanelSizes_.insert(it.key(), value);
    applyPanelLock();
    graphPane_->show();
    mapPane_->show();
}

// ── Queries ─────────────────────────────────────────────────────────────────

bool AnalysisPage::fixedMode() const {
    return playback_ && modeTabs_->currentIndex() == kFixedTab;
}

AnalysisPage::View AnalysisPage::effectiveView() const {
    return playback_ && !circuitsMismatched() ? preferredView_ : View::Graphs;
}

bool AnalysisPage::circuitsMismatched() const {
    return primary_ && secondary_ && primary_->trackId >= 0 && secondary_->trackId >= 0 &&
           primary_->trackId != secondary_->trackId;
}

AnalysisRecording* AnalysisPage::recordingFor(const AnalysisDriverRef& driver) const {
    if (!driver.valid) return nullptr;
    return driver.secondary ? secondary_.get() : primary_.get();
}

AnalysisDriverRef AnalysisPage::followedDriver() const {
    if (!primary_ || !primary_->driver(currentDriverIndex_)) return {};
    return {true, false, currentDriverIndex_};
}

uint32_t AnalysisPage::playbackRowMask() const {
    uint32_t mask = AnalysisRows::Progress;
    const View view = effectiveView();
    if (view != View::Graphs)
        mask |= AnalysisRows::Positions | AnalysisRows::Telemetry | AnalysisRows::Status;
    if (view == View::Map) return mask;
    for (const AnalyzeSeriesSetting& setting : seriesModel_->series()) {
        if (!setting.visible || !analyzeSeriesHasLines(setting)) continue;
        const AnalyzeMetric* metric = analyzeScaleMetric(setting.metricId);
        if (!metric) continue;
        switch (metric->source) {
        case AnalyzeSource::Telemetry:
        case AnalyzeSource::Tyre: mask |= AnalysisRows::Telemetry; break;
        case AnalyzeSource::Status: mask |= AnalysisRows::Status; break;
        case AnalyzeSource::Damage: mask |= AnalysisRows::Damage; break;
        case AnalyzeSource::Motion: mask |= AnalysisRows::Motion; break;
        case AnalyzeSource::MotionEx: mask |= AnalysisRows::MotionEx; break;
        }
    }
    return mask;
}

// ── Selectors ───────────────────────────────────────────────────────────────

void AnalysisPage::refreshDriverChoices() {
    auto choicesOf = [](const AnalysisRecording* recording, bool secondary) {
        QVector<AnalysisDriverChoice> choices;
        if (!recording) return choices;
        for (int index : recording->driverOrder())
            if (const auto* driver = recording->driver(index))
                choices.push_back({{true, secondary, index}, driver->name});
        return choices;
    };
    const auto primary = choicesOf(primary_.get(), false);
    const auto secondary = choicesOf(secondary_.get(), true);
    const AnalysisDriverRef fallback = followedDriver();
    for (AnalysisLapSlot* slot : {compareSlot_, lapASlot_, lapBSlot_})
        slot->setDriverChoices(primary, secondary, fallback);
}

void AnalysisPage::refreshLapChoices() {
    for (AnalysisLapSlot* slot : {compareSlot_, lapASlot_, lapBSlot_}) {
        const AnalysisDriverRef ref = slot->driver();
        const AnalysisRecording* recording = recordingFor(ref);
        const auto* driver = recording ? recording->driver(ref.driverIndex) : nullptr;
        QVector<AnalysisLapChoice> laps;
        if (driver)
            for (const LapBlock& lap : driver->catalog.laps)
                laps.push_back(lapChoiceFor(lap, driver->catalog.fastestLapNum));
        slot->setLapChoices(laps);
    }
    applyState();
}

void AnalysisPage::refreshFollowedSlot() {
    QString driverName;
    if (primary_)
        if (const auto* driver = primary_->driver(currentDriverIndex_)) driverName = driver->name;
    const LapBlock* lap = model_->chartPrimaryLap(currentTime_);
    if (!lap) {
        currentSlot_->setFollowed(driverName, {});
        return;
    }
    const AnalysisLapChoice choice = lapChoiceFor(*lap, model_->data().fastestLapNum);
    const QString running = analysis::formatLapTime(
        qRound((currentTime_ - lap->startSessionTime) * 1000.0f));
    currentSlot_->setFollowed(driverName, choice, running);
}

void AnalysisPage::refreshSecondaryRow() {
    const QString fileText = secondaryLoading_ ? QStringLiteral("Loading…")
                             : secondary_ ? secondary_->filename : QString();
    if (secondaryField_->text() != fileText) {
        secondaryField_->setText(fileText);
        secondaryField_->setCursorPosition(0);
    }
    secondaryField_->setToolTip(secondary_ && !secondaryLoading_ ? secondary_->path : QString());
    openSecondary_->setToolTip(secondary_ ? QStringLiteral("Replace the second recording")
                                          : QStringLiteral("Open a second recording to compare against"));
    openSecondary_->setEnabled(playback_ && !secondaryLoading_);
    removeSecondary_->setVisible(secondary_ != nullptr);
    removeSecondary_->setEnabled(!secondaryLoading_);
    recordingGroup_->setEnabled(playback_);
}

void AnalysisPage::refreshMetricActions() {
    const int row = currentSeriesRow();
    const int rows = seriesModel_->rowCount();
    const QModelIndex index = seriesModel_->index(row);
    const bool valid = row >= 0;
    removeMetricAction_->setEnabled(valid && index.data(AnalysisSeriesModel::RemovableRole).toBool());
    moveUpAction_->setEnabled(valid && row > 0);
    moveDownAction_->setEnabled(valid && row + 1 < rows);
    changeColorAction_->setEnabled(valid);
    resetColorAction_->setEnabled(valid);
    yAxisAction_->setEnabled(valid);
    yAxisAction_->setChecked(valid && index.data(AnalysisSeriesModel::ShowYAxisRole).toBool());
    allYAxesAction_->setChecked(seriesModel_->allYAxesShown());
}

int AnalysisPage::currentSeriesRow() const {
    const QModelIndex index = seriesView_->currentIndex();
    return index.isValid() ? index.row() : -1;
}

// ── State ───────────────────────────────────────────────────────────────────

void AnalysisPage::applyState() {
    const bool fixed = fixedMode();
    const AnalysisLapRef compare = compareSlot_->selection();
    const AnalysisLapRef lapA = lapASlot_->selection();
    const AnalysisLapRef lapB = lapBSlot_->selection();

    auto hasDistance = [this](const AnalysisLapRef& ref) {
        const AnalysisRecording* recording = recordingFor(ref.driver);
        return !ref.isValid() || (recording && recording->distanceAvailable);
    };
    const bool distance = model_->lapCoordinatesAvailable() &&
        (fixed ? hasDistance(lapA) && hasDistance(lapB) : hasDistance(compare));
    const bool mismatch = circuitsMismatched();
    if (mismatch && preferredView_ != View::Graphs) {
        // Laps from different circuits cannot share a map.
        preferredView_ = View::Graphs;
        saveSettings();
    }
    const bool mapAllowed = playback_ && !mismatch;
    const View view = effectiveView();
    const bool showMap = view != View::Graphs;
    const bool showChart = view != View::Map;

    // ── Tool bar ──
    for (QAction* action : viewGroup_->actions()) {
        QSignalBlocker guard(action);
        action->setChecked(static_cast<View>(action->data().toInt()) == view);
    }
    splitAction_->setEnabled(mapAllowed);
    mapAction_->setEnabled(mapAllowed);
    syncedTooltipAction_->setEnabled(stackedAction_->isChecked());
    sectorBoundariesAction_->setEnabled(!mismatch);
    sectorDeltaAction_->setEnabled(!mismatch && sectorBoundariesAction_->isChecked());
    splitCursorsAction_->setEnabled(view == View::Split && fixed);
    inputsAction_->setEnabled(showMap);
    brakingAction_->setEnabled(showMap);
    layerGroup_->setEnabled(showMap);
    const bool navigation = showChart && fixed && lapA.isValid();
    for (QAction* action : {zoomInAction_, zoomOutAction_, resetZoomAction_}) action->setEnabled(navigation);

    // ── Sidebar ──
    const bool haveLaps = playback_ && !model_->data().laps.isEmpty();
    modeTabs_->setTabEnabled(kFixedTab, haveLaps);
    if (!haveLaps && modeTabs_->currentIndex() == kFixedTab) {
        modeTabs_->setCurrentIndex(kPlaybackTab);
        showSlotPage(kPlaybackTab);
    }
    const qsizetype driverCount = (primary_ ? primary_->driverOrder().size() : 0) +
                                  (secondary_ ? secondary_->driverOrder().size() : 0);
    for (AnalysisLapSlot* slot : {compareSlot_, lapASlot_, lapBSlot_}) {
        slot->setDriverSelectable(playback_ && driverCount > 1);
        slot->setLapSelectable(playback_);
    }
    seriesModel_->setDeltaSupported(!playback_ || distance);
    refreshSecondaryRow();
    refreshFollowedSlot();

    // ── Resolve the two laps ──
    auto resolve = [this](const AnalysisLapRef& ref) {
        ResolvedLap out;
        const AnalysisRecording* recording = ref.isValid() ? recordingFor(ref.driver) : nullptr;
        const auto* driver = recording ? recording->driver(ref.driver.driverIndex) : nullptr;
        if (!driver) return out;
        out.data = &driver->catalog;
        out.trackId = recording->trackId;
        out.lap = recording->loadedLap(ref.driver.driverIndex, ref.lapNum);
        return out;
    };
    ResolvedLap primary;
    ResolvedLap comparison;
    if (fixed) {
        primary = resolve(lapA);
        comparison = resolve(lapB);
    } else {
        primary = {&model_->data(), model_->chartPrimaryLap(currentTime_), primaryTrackId_};
        comparison = resolve(compare);
    }
    shownPrimary_ = primary;
    shownComparison_ = comparison;
    shownFixed_ = fixed;

    const QString primaryLabel = (fixed ? lapASlot_ : currentSlot_)->resolvedLabel();
    const QString comparisonLabel = (fixed ? lapBSlot_ : compareSlot_)->resolvedLabel();

    // ── Graphs ──
    chart_->setConfig(seriesModel_->series(), true);
    chart_->setLabels(primaryLabel, comparisonLabel);
    chart_->setDistanceMode(distance);
    chart_->setIndividualGraphs(stackedAction_->isChecked(), syncedTooltipAction_->isChecked());
    chart_->setSectorOptions(!mismatch && sectorBoundariesAction_->isChecked(),
                             !mismatch && sectorDeltaAction_->isChecked());
    chart_->setSelectedLaps(fixed, primary.data, primary.lap, comparison.data, comparison.lap);
    chart_->setMapCursors(showMap && showChart && fixed && splitCursorsAction_->isChecked(),
                          primaryColor_, comparisonColor_);

    // ── Map ──
    showFollowedLap(primary.lap);
    map_->setColors(primaryColor_, comparisonColor_);
    map_->setLabels(primaryLabel, comparisonLabel);
    map_->setReadoutVisible(inputsAction_->isChecked());
    map_->setCurrentTime(currentTime_);
    zones_->setLaps(primaryLabel, primaryColor_, comparisonLabel, comparisonColor_);
    graphPane_->setVisible(showChart);
    mapPane_->setVisible(showMap);
    contentSplitter_->applyRatio();   // Split view opens at the saved percentage

    refreshDelta();
    emit navigationEnabledChanged(showChart && fixed && lapA.isValid());
    emit dataRequirementsChanged();
    requestAnalysisLaps();
}

// Hands the primary lap to the map. Split out of applyState() because in
// Playback mode the primary lap changes under the cursor at every lap line.
void AnalysisPage::showFollowedLap(const LapBlock* lap) {
    shownPrimary_.lap = lap;
    const bool compatible = !shownPrimary_.lap || !shownComparison_.lap ||
                            shownPrimary_.trackId < 0 || shownComparison_.trackId < 0 ||
                            shownPrimary_.trackId == shownComparison_.trackId;
    const int trackId = shownPrimary_.trackId >= 0 ? shownPrimary_.trackId : shownComparison_.trackId;
    map_->setLaps(shownPrimary_.lap, shownComparison_.lap, shownFixed_, trackId, compatible);
    deltaReadout_->setSectorsVisible(!circuitsMismatched() && compatible);
    requestMapLayers();
}

// Asks libtnrp (tnrp/AnalysisMap.h) for the map layer and the braking points
// of the laps on show, on the thread pool. A request is made only when what
// it asks for changes; while it runs the last answer stays up, and an answer
// to a request that has since been replaced is dropped.
void AnalysisPage::requestMapLayers() {
    const bool fixed = fixedMode();
    AnalysisLapRef current;
    AnalysisLapRef comparison;
    if (fixed) {
        current = lapASlot_->selection();
        comparison = lapBSlot_->selection();
    } else {
        comparison = compareSlot_->selection();
        if (shownPrimary_.lap) {
            const AnalysisDriverRef followed = followedDriver();
            current.driver = followed.valid ? followed : AnalysisDriverRef{true, false, -1};
            current.lapNum = shownPrimary_.lap->lapNum;
        }
    }
    const QString layer = layerMode();
    map_->setLayerMode(layer, comparison.isValid());
    const bool ready = playback_ && current.isValid() && primaryAccess_ && !circuitsMismatched();
    const bool mapShown = effectiveView() != View::Graphs;
    const bool active = ready && mapShown;
    // Zones are found for the map's highlights and for the Braking Zones panel.
    const bool zonesWanted = ready && ((mapShown && brakingAction_->isChecked()) || zonesDock_->toggleViewAction()->isChecked());

    const auto describe = [this](const AnalysisLapRef& ref) {
        if (!ref.isValid()) return QStringLiteral("-");
        const AnalysisRecording* recording = recordingFor(ref.driver);
        return QStringLiteral("%1:%2:%3:%4").arg(ref.driver.secondary ? 2 : 1)
            .arg(recording ? recording->generation : 0).arg(ref.driver.driverIndex).arg(ref.lapNum);
    };
    const auto access = [this](const AnalysisLapRef& ref) {
        return ref.driver.secondary ? secondaryReader_->recordingAccess() : primaryAccess_;
    };
    const auto issue = [&](const QString& mode, QString& key, bool braking) {
        if (mode.isEmpty()) {
            key.clear();
            if (braking) {
                brakingResult_.reset();
                zones_->setResult(nullptr);
                map_->setBrakingResult(nullptr);
            } else {
                map_->setLayerResult(nullptr);
            }
            return;
        }
        const QString next = mode + QLatin1Char('|') + describe(current) + QLatin1Char('|') + describe(comparison);
        if (next == key) return;
        key = next;
        const tnrp::AnalysisMapLap currentLap{access(current), current.lapNum, current.driver.driverIndex};
        const std::optional<tnrp::AnalysisMapLap> comparisonLap = comparison.isValid()
            ? std::optional<tnrp::AnalysisMapLap>(tnrp::AnalysisMapLap{
                  access(comparison), comparison.lapNum, comparison.driver.driverIndex})
            : std::nullopt;
        QPointer<AnalysisPage> self(this);
        QThreadPool::globalInstance()->start([self, next, braking, mode = mode.toStdString(), currentLap,
                                              comparisonLap] {
            auto result = std::make_shared<const tnrp::AnalysisMapResult>(tnrp::buildAnalysisMap(
                mode, currentLap, comparisonLap ? &*comparisonLap : nullptr));
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, next, braking, result] {
                if (!self || (braking ? self->brakingKey_ : self->layerKey_) != next) return;
                if (braking) {
                    self->brakingResult_ = result;
                    self->zones_->setResult(result);
                    self->map_->setBrakingResult(self->brakingAction_->isChecked() ? result : nullptr);
                } else {
                    self->map_->setLayerResult(result);
                }
            }, Qt::QueuedConnection);
        });
    };
    issue(active && layer != QLatin1String("none") ? layer : QString(), layerKey_, false);
    issue(zonesWanted ? QStringLiteral("braking") : QString(), brakingKey_, true);
    // Braking Points only decides whether the map shows the zones.
    map_->setBrakingResult(brakingAction_->isChecked() ? brakingResult_ : nullptr);
}

QString AnalysisPage::layerMode() const {
    const QAction* checked = layerGroup_->checkedAction();
    return checked ? checked->data().toString() : QStringLiteral("none");
}

void AnalysisPage::showBrakingZones() {
    zonesDock_->show();
    zonesDock_->raise();
}

void AnalysisPage::setPrimaryRecordingAccess(tnrp::RecordingAccess access) {
    primaryAccess_ = std::move(access);
    layerKey_.clear();
    brakingKey_.clear();
    requestMapLayers();
}

void AnalysisPage::refreshDelta() {
    std::array<double, 4> values{qQNaN(), qQNaN(), qQNaN(), qQNaN()};
    const SessionData* primaryData = shownPrimary_.data;
    const SessionData* comparisonData = shownComparison_.data;
    const LapBlock* primary = shownPrimary_.lap;
    const LapBlock* comparison = shownComparison_.lap;
    const bool compatible = !primary || !comparison || shownPrimary_.trackId < 0 ||
                            shownComparison_.trackId < 0 ||
                            shownPrimary_.trackId == shownComparison_.trackId;
    const bool showSectors = !circuitsMismatched() && compatible;

    if (primary && comparison && primaryData && !primary->progress.isEmpty() &&
        !comparison->progress.isEmpty()) {
        const double fullDistance = qMin<double>(primary->progress.last().distanceM,
                                                 comparison->progress.last().distanceM);
        double cursorDistance = fullDistance;
        if (!shownFixed_) {
            // Playback: the delta so far, up to where the cursor is in this lap.
            cursorDistance = currentTime_ < primary->progress.first().t ||
                                     currentTime_ > primary->progress.last().t
                ? qQNaN()
                : primaryData->distanceAtTime(primary, currentTime_);
        }
        // NaN (cursor outside this lap) must stay NaN: qMin would replace it.
        if (std::isfinite(cursorDistance)) cursorDistance = qMin(cursorDistance, fullDistance);

        // Cumulative gap at a distance: positive when the primary lap is slower.
        auto gapAt = [&](double distance) {
            if (!std::isfinite(distance) || distance < 0) return qQNaN();
            const double primaryTime =
                primaryData->timeAtDistance(primary, distance) - primary->startSessionTime;
            const double comparisonTime = comparisonData
                ? comparisonData->timeAtDistance(comparison, distance) - comparison->startSessionTime
                : qQNaN();
            return std::isfinite(primaryTime) && std::isfinite(comparisonTime)
                ? primaryTime - comparisonTime : qQNaN();
        };

        if (std::isfinite(cursorDistance) && cursorDistance >= 0) {
            values[3] = gapAt(cursorDistance);
            std::array<double, 2> splits{qQNaN(), qQNaN()};
            auto mergeSplits = [&](const SessionData* owner, const LapBlock* lap) {
                if (!owner || !lap) return;
                for (const LapProgressSample& split : owner->sectorSplits(lap)) {
                    const int index = split.sector - 1;
                    if (index >= 0 && index < 2) splits[static_cast<size_t>(index)] = split.distanceM;
                }
            };
            // The primary lap's own splits win where both laps have them.
            mergeSplits(comparisonData, comparison);
            mergeSplits(primaryData, primary);

            if (showSectors && std::isfinite(splits[0])) {
                values[0] = gapAt(qMin(cursorDistance, splits[0]));
                if (cursorDistance >= splits[0] && std::isfinite(splits[1])) {
                    const double start = gapAt(splits[0]);
                    const double end = gapAt(qMin(cursorDistance, splits[1]));
                    if (std::isfinite(start) && std::isfinite(end)) values[1] = end - start;
                }
                if (std::isfinite(splits[1]) && cursorDistance >= splits[1]) {
                    const double start = gapAt(splits[1]);
                    const double end = gapAt(cursorDistance);
                    if (std::isfinite(start) && std::isfinite(end)) values[2] = end - start;
                }
                if (sectorDeltaAction_->isChecked()) {
                    // Sector Delta: the lap figure is the sum of the sector gaps shown.
                    double total = 0;
                    bool any = false;
                    for (size_t i = 0; i < 3; ++i) {
                        if (!std::isfinite(values[i])) continue;
                        total += values[i];
                        any = true;
                    }
                    values[3] = any ? total : qQNaN();
                }
            }
        }
    }

    QColor slower = AnalysisSeriesModel::kDeltaPositive;
    QColor faster = AnalysisSeriesModel::kDeltaNegative;
    const int deltaRow = seriesModel_->rowOf(QStringLiteral("delta"));
    if (deltaRow >= 0) {
        const AnalyzeSeriesSetting& delta = seriesModel_->series()[deltaRow];
        if (delta.color.isValid()) slower = delta.color;
        if (delta.negativeColor.isValid()) faster = delta.negativeColor;
    }
    deltaReadout_->setColors(slower, faster);
    deltaReadout_->setValues(values);
}

void AnalysisPage::setPrimaryColor(const QColor& color) {
    primaryColor_ = color;
    for (AnalysisLapSlot* slot : {currentSlot_, lapASlot_}) slot->setColor(color);
    saveSettings();
    applyState();
}

void AnalysisPage::setComparisonColor(const QColor& color) {
    comparisonColor_ = color;
    for (AnalysisLapSlot* slot : {compareSlot_, lapBSlot_}) slot->setColor(color);
    saveSettings();
    applyState();
}

void AnalysisPage::chooseSeriesColor(int row, const QString& part) {
    const QModelIndex index = seriesModel_->index(row);
    if (!index.isValid()) return;
    const QString name = index.data(Qt::DisplayRole).toString();
    const bool delta = index.data(AnalysisSeriesModel::NegativeColorRole).isValid();
    const bool negative = part == AnalysisSeriesDelegate::negativePart();
    const bool corner = !part.isEmpty() && !negative;

    QColor initial;
    QString title;
    if (corner) {
        const QVariantList colors = index.data(AnalysisSeriesModel::CornerColorsRole).toList();
        const auto& corners = analyzeTyreCorners();
        for (int i = 0; i < corners.size() && i < colors.size(); ++i) {
            if (corners[i].key != part) continue;
            initial = colors[i].value<QColor>();
            title = QStringLiteral("%1 %2 Colour").arg(name, corners[i].label);
        }
    } else {
        initial = index.data(negative ? AnalysisSeriesModel::NegativeColorRole
                                      : AnalysisSeriesModel::ColorRole).value<QColor>();
        title = !delta ? QStringLiteral("%1 Colour").arg(name)
              : negative ? QStringLiteral("Delta Colour When Faster")
                         : QStringLiteral("Delta Colour When Slower");
    }
    const QColor color = QColorDialog::getColor(initial, this, title,
                                                QColorDialog::DontUseNativeDialog);
    if (!color.isValid()) return;
    if (corner) seriesModel_->setCornerColor(row, part, color);
    else seriesModel_->setColor(row, color, negative);
}

void AnalysisPage::showMessage(const QString& text) {
    messageText_->setText(text);
    messageBar_->setVisible(!text.isEmpty());
    fitLapPanel();
}

// ── Public API ──────────────────────────────────────────────────────────────

void AnalysisPage::setPrimaryRecording(int trackId, const QString& trackName) {
    primaryTrackId_ = trackId;
    primaryTrackName_ = trackName;
    currentDriverIndex_ = -1;
    ++primaryGeneration_;
    primaryCatalogReady_ = false;
    primary_ = std::make_unique<AnalysisRecording>();
    primary_->trackId = trackId;
    primary_->trackName = trackName;
    primary_->generation = primaryGeneration_;
    refreshDriverChoices();
    refreshLapChoices();
}

void AnalysisPage::setPrimaryCatalog(const tnrp::PlaybackLapBlocksRow& catalog) {
    if (!primary_) primary_ = std::make_unique<AnalysisRecording>();
    if (!primaryCatalogReady_) {
        primary_->setCatalog(catalog);
        primaryCatalogReady_ = true;
    }
    // Follow the playback driver; otherwise the player; otherwise the first driver.
    int current = catalog.playbackDriverIndex;
    if (!primary_->driver(current)) {
        for (int index : primary_->driverOrder()) {
            const auto* driver = primary_->driver(index);
            if (driver && driver->isPlayer) {
                current = index;
                break;
            }
        }
    }
    if (!primary_->driver(current) && !primary_->driverOrder().isEmpty())
        current = primary_->driverOrder().first();
    currentDriverIndex_ = current;
    refreshDriverChoices();
    refreshLapChoices();
}

void AnalysisPage::installPrimaryLap(uint64_t generation, int driverIndex, int lapNum,
                                     uint32_t rowTypeMask,
                                     const std::shared_ptr<PlaybackHistoryBatch>& batch) {
    if (primary_ && primary_->generation == generation)
        installLap(*primary_, driverIndex, lapNum, rowTypeMask, batch);
}

void AnalysisPage::setPlaybackMode(bool on, float currentTime) {
    playback_ = on;
    currentTime_ = currentTime;
    chart_->setPlaybackMode(on);
    chart_->setCurrentTime(currentTime);
    map_->setCurrentTime(currentTime);
    if (!on) {
        clearSecondaryFile(true);
        primary_.reset();
        primaryCatalogReady_ = false;
        currentDriverIndex_ = -1;
        primaryTrackId_ = -1;
        primaryTrackName_.clear();
        resetPlaybackSelections();
        return;
    }
    refreshDriverChoices();
    refreshLapChoices();
}

void AnalysisPage::setCurrentTime(float t) {
    currentTime_ = t;
    chart_->setCurrentTime(t);
    map_->setCurrentTime(t);
    if (!shownFixed_) {
        const LapBlock* lap = model_->chartPrimaryLap(t);
        if (lap != shownPrimary_.lap) {
            shownPrimary_.data = &model_->data();
            shownPrimary_.trackId = primaryTrackId_;
            chart_->setSelectedLaps(false, shownPrimary_.data, lap, shownComparison_.data,
                                    shownComparison_.lap);
            showFollowedLap(lap);
        }
        refreshFollowedSlot();
    }
    refreshDelta();
}

void AnalysisPage::setMapAppearance(bool sectorColors, int opacityPercent) {
    map_->setMapAppearance(sectorColors, opacityPercent);
}

void AnalysisPage::setAeroMode(bool slm) {
    map_->setAeroMode(slm);
}

void AnalysisPage::resetPlaybackSelections() {
    modeTabs_->setCurrentIndex(kPlaybackTab);
    showSlotPage(kPlaybackTab);
    for (AnalysisLapSlot* slot : {compareSlot_, lapASlot_, lapBSlot_}) slot->clearLap();
    refreshDriverChoices();
    refreshLapChoices();
}

void AnalysisPage::zoomIn() { chart_->zoomIn(); }
void AnalysisPage::zoomOut() { chart_->zoomOut(); }
void AnalysisPage::panLeft() { chart_->panLeft(); }
void AnalysisPage::panRight() { chart_->panRight(); }
void AnalysisPage::resetZoom() { chart_->resetZoom(); }

// ── Second recording ────────────────────────────────────────────────────────

void AnalysisPage::loadSecondaryFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Second Recording"),
        settings_.value("outputDirectory").toString(),
        QStringLiteral("Track N Race Recordings (*.tnrd *.trnd)"));
    if (path.isEmpty()) return;
    secondaryLoading_ = true;
    showMessage({});
    refreshSecondaryRow();
    secondaryReader_->load(path);
}

void AnalysisPage::onSecondaryCatalog(const std::shared_ptr<AnalysisFileCatalog>& catalog) {
    secondaryLoading_ = false;
    if (primaryTrackId_ >= 0 && catalog->trackId >= 0 && catalog->trackId != primaryTrackId_) {
        QMessageBox warning(QMessageBox::Warning, QStringLiteral("Different Circuit"),
                            QStringLiteral("The second recording was made on a different circuit."),
                            QMessageBox::NoButton, this);
        const QString loaded = primaryTrackName_.isEmpty()
            ? QStringLiteral("Circuit %1").arg(primaryTrackId_) : primaryTrackName_;
        const QString selected = catalog->trackName.isEmpty()
            ? QStringLiteral("Circuit %1").arg(catalog->trackId) : catalog->trackName;
        warning.setInformativeText(
            QStringLiteral("Laps will not line up, so only Graphs are available and sector "
                           "figures are hidden.\n\nThis recording: %1\nSecond recording: %2")
                .arg(loaded, selected));
        QPushButton* loadAnyway = warning.addButton(QStringLiteral("Load Anyway"),
                                                    QMessageBox::AcceptRole);
        warning.addButton(QMessageBox::Cancel);
        warning.setDefaultButton(QMessageBox::Cancel);
        warning.exec();
        if (warning.clickedButton() != loadAnyway) {
            secondaryReader_->rejectLoaded(catalog->generation);
            refreshSecondaryRow();
            return;
        }
    }
    secondaryReader_->acceptLoaded(catalog->generation);

    resetSecondarySelections();
    auto recording = std::make_unique<AnalysisRecording>();
    recording->filename = catalog->filename;
    recording->path = catalog->path;
    recording->trackName = catalog->trackName;
    recording->trackId = catalog->trackId;
    recording->generation = catalog->generation;
    recording->setCatalog(catalog->laps);
    secondary_ = std::move(recording);
    showMessage({});
    refreshDriverChoices();
    refreshLapChoices();
}

void AnalysisPage::clearSecondaryFile(bool clearMessage) {
    resetSecondarySelections();
    secondary_.reset();
    secondaryReader_->close();
    secondaryLoading_ = false;
    if (clearMessage) showMessage({});
    refreshDriverChoices();
    refreshLapChoices();
}

void AnalysisPage::resetSecondarySelections() {
    for (AnalysisLapSlot* slot : {compareSlot_, lapASlot_, lapBSlot_})
        if (slot->driver().secondary) slot->resetSelection();
}

// ── Lap data ────────────────────────────────────────────────────────────────

void AnalysisPage::installLap(AnalysisRecording& recording, int driverIndex, int lapNum,
                              uint32_t rowTypeMask,
                              const std::shared_ptr<PlaybackHistoryBatch>& batch) {
    if (recording.install(driverIndex, lapNum, rowTypeMask, batch)) applyState();
}

void AnalysisPage::requestAnalysisLaps() {
    if (!playback_) return;
    const QVector<AnalysisLapRef> wanted = fixedMode()
        ? QVector<AnalysisLapRef>{lapASlot_->selection(), lapBSlot_->selection()}
        : QVector<AnalysisLapRef>{compareSlot_->selection()};
    const uint32_t rows = playbackRowMask();
    QVector<AnalysisLapRef> requested;
    for (const AnalysisLapRef& ref : wanted) {
        if (!ref.isValid() || requested.contains(ref)) continue;
        requested.push_back(ref);
        AnalysisRecording* recording = recordingFor(ref.driver);
        if (!recording || (ref.driver.secondary && secondaryLoading_)) continue;
        const uint32_t missing = recording->claimMissingRows(ref.driver.driverIndex, ref.lapNum, rows);
        if (!missing) continue;
        if (ref.driver.secondary)
            secondaryReader_->requestLapData(ref.driver.driverIndex, ref.lapNum, missing);
        else
            emit primaryLapDataRequested(primaryGeneration_, ref.driver.driverIndex, ref.lapNum,
                                         missing);
    }
}

void AnalysisPage::inspectMap(double coordinate, bool distanceCoordinate, bool showMap) {
    if (!playback_ || circuitsMismatched()) return;
    const SessionData* owner = &model_->data();
    const LapBlock* lap = nullptr;
    if (fixedMode()) {
        const AnalysisLapRef ref = lapASlot_->selection();
        const AnalysisRecording* recording = ref.isValid() ? recordingFor(ref.driver) : nullptr;
        if (const auto* driver = recording ? recording->driver(ref.driver.driverIndex) : nullptr) {
            owner = &driver->catalog;
            lap = recording->loadedLap(ref.driver.driverIndex, ref.lapNum);
        }
    } else {
        lap = model_->chartPrimaryLap(currentTime_);
    }
    if (!lap) return;
    const double elapsed = distanceCoordinate
        ? owner->timeAtDistance(lap, coordinate) - lap->startSessionTime
        : coordinate;
    if (!std::isfinite(elapsed)) return;
    if (showMap && preferredView_ == View::Graphs) {
        preferredView_ = View::Split;
        saveSettings();
        applyState();
    }
    map_->focusElapsed(elapsed);
}

// ── Help ────────────────────────────────────────────────────────────────────

void AnalysisPage::showControlsHelp() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Analysis Help"));
    auto* layout = new QVBoxLayout(&dialog);
    const int iconSize = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);

    auto section = [&](const QString& title) {
        auto* box = new QGroupBox(title, &dialog);
        auto* form = new QFormLayout(box);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
        form->setHorizontalSpacing(12);
        layout->addWidget(box);
        return form;
    };
    auto row = [&](QFormLayout* form, const QIcon& icon, const QString& name, const QString& text) {
        auto* label = new QWidget;
        auto* labelLayout = new QHBoxLayout(label);
        labelLayout->setContentsMargins(0, 0, 0, 0);
        labelLayout->setSpacing(6);
        auto* iconLabel = new QLabel;
        if (!icon.isNull()) iconLabel->setPixmap(icon.pixmap(iconSize, iconSize));
        iconLabel->setFixedWidth(iconSize);
        auto* nameLabel = new QLabel(QStringLiteral("<b>%1</b>").arg(name.toHtmlEscaped()));
        labelLayout->addWidget(iconLabel);
        labelLayout->addWidget(nameLabel);
        auto* description = new QLabel(text);
        description->setWordWrap(true);
        form->addRow(label, description);
    };

    QFormLayout* views = section(QStringLiteral("Tool Bar"));
    for (QAction* action : {graphsAction_, splitAction_, mapAction_, stackedAction_,
                            syncedTooltipAction_, sectorBoundariesAction_, sectorDeltaAction_,
                            splitCursorsAction_, brakingAction_, inputsAction_})
        row(views, action->icon(), action->text(), action->whatsThis());
    row(views, {}, QStringLiteral("Layer"),
        QStringLiteral("Paints the map: Track Dominance shows the faster lap per mini-sector; "
                       "Driver Inputs, Speed, ERS Usage and ERS Harvest give each lap its own map."));

    QFormLayout* panels = section(QStringLiteral("Panels"));
    row(panels, {}, QStringLiteral("Panels"),
        QStringLiteral("Laps, Metrics and Braking Zones can be floated, moved, tabbed together or "
                       "closed. A closed panel leaves a tab on the window edge; the Panels menu "
                       "(also on right-click of a panel title) reopens it or resets the layout."));
    row(panels, {}, QStringLiteral("Braking Zones"),
        QStringLiteral("Every braking zone with the time gap through it. Select one to see both "
                       "laps head to head and to move the cursors there."));

    QFormLayout* laps = section(QStringLiteral("Laps"));
    row(laps, {}, QStringLiteral("Follow Playback"),
        QStringLiteral("The current lap follows the playback cursor; pick any lap to compare it with."));
    row(laps, {}, QStringLiteral("Fixed Laps"),
        QStringLiteral("Pick both laps yourself. Graphs can then be zoomed and panned, and the map "
                       "gets its own replay controls."));
    row(laps, {}, QStringLiteral("Second Recording"),
        QStringLiteral("Adds the drivers of another recording to the driver lists."));

    QFormLayout* chart = section(QStringLiteral("Graph Interactions"));
    row(chart, {}, QStringLiteral("Zoom"), QStringLiteral("Ctrl + mouse wheel (fixed laps)."));
    row(chart, {}, QStringLiteral("Pan"), QStringLiteral("Mouse wheel, or click and drag."));
    row(chart, {}, QStringLiteral("Reset"), QStringLiteral("Double-click with the left button."));
    row(chart, {}, QStringLiteral("Show on Map"),
        QStringLiteral("Double-click with the right button to put the map cursor at that point."));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(560, dialog.sizeHint().height());
    dialog.exec();
}
