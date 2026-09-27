#include "AnalysisPage.h"

#include "AnalysisLapSlot.h"
#include "AnalysisMapView.h"
#include "AnalysisMetricPicker.h"
#include "AnalysisSeriesModel.h"
#include "AnalysisWidgets.h"

#include "../AnalyzeChart.h"
#include "../TyreHelpers.h"
#include "../../AnalysisFileReader.h"
#include "../../IconUtils.h"
#include "../../SessionModel.h"
#include "../../TnrdPlayer.h"

#include <QAction>
#include <QActionGroup>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

namespace {

constexpr int kSettingsVersion = 10;
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

    contentSplitter_ = new QSplitter(Qt::Horizontal, this);
    contentSplitter_->setChildrenCollapsible(false);
    contentSplitter_->addWidget(chart_);
    contentSplitter_->addWidget(map_);
    contentSplitter_->setStretchFactor(0, 1);
    contentSplitter_->setStretchFactor(1, 1);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setChildrenCollapsible(false);
    splitter_->addWidget(buildSidebar());
    splitter_->addWidget(contentSplitter_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(toolBar_);
    root->addWidget(splitter_, 1);

    loadSettings();
    restoreLayout();

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

    connect(modeTabs_, &QTabBar::currentChanged, this, [this](int index) {
        slotPages_->setCurrentIndex(index);
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
    for (AnalysisLapSlot* slot : {currentSlot_, lapASlot_})
        connect(slot, &AnalysisLapSlot::colorPicked, this, &AnalysisPage::setPrimaryColor);
    for (AnalysisLapSlot* slot : {compareSlot_, lapBSlot_})
        connect(slot, &AnalysisLapSlot::colorPicked, this, &AnalysisPage::setComparisonColor);

    connect(openSecondary_, &QToolButton::clicked, this, &AnalysisPage::loadSecondaryFile);
    connect(removeSecondary_, &QToolButton::clicked, this, [this] { clearSecondaryFile(true); });
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

    connect(chart_, &ChartView::inspectionRequested, this, &AnalysisPage::inspectMap);
    connect(map_, &AnalysisMapView::cursorElapsedChanged, chart_, &AnalyzeChart::setMapCursorElapsed);
    connect(splitter_, &QSplitter::splitterMoved, this, &AnalysisPage::saveSettings);
    connect(contentSplitter_, &QSplitter::splitterMoved, this, &AnalysisPage::saveSettings);
    connect(model_, &SessionModel::lapsChanged, this, &AnalysisPage::refreshLapChoices);
    connect(model_, &SessionModel::chartConfigurationChanged, this, &AnalysisPage::applyState);

    refreshSecondaryRow();
    refreshMetricActions();
    refreshDriverChoices();
    refreshLapChoices();
    fitLapPanel();
}

// The lap panel is exactly as tall as what it shows: the stack measures only
// its visible page (the Fixed Laps page is taller), and the panel may not grow
// past its content, so it scrolls only when something is actually cut off.
void AnalysisPage::fitLapPanel() {
    for (int index = 0; index < slotPages_->count(); ++index) {
        const bool current = index == slotPages_->currentIndex();
        slotPages_->widget(index)->setSizePolicy(
            QSizePolicy::Preferred, current ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    }
    slotPages_->updateGeometry();
    QWidget* content = lapScroll_->widget();
    if (QLayout* layout = content->layout()) layout->activate();
    lapScroll_->setMaximumHeight(content->sizeHint().height() + 2 * lapScroll_->frameWidth());
}

AnalysisPage::~AnalysisPage() = default;

// ── Construction ────────────────────────────────────────────────────────────

void AnalysisPage::buildToolBar() {
    toolBar_ = new QToolBar(QStringLiteral("Analysis"), this);
    toolBar_->setMovable(false);
    toolBar_->setFloatable(false);
    toolBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    const int iconSize = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    toolBar_->setIconSize(QSize(iconSize, iconSize));

    sidebarAction_ = makeToggle(this, themed(this, {"sidebar-show", "view-sidetree"},
                                             QStyle::SP_FileDialogDetailedView),
                                QStringLiteral("Show Sidebar"),
                                QStringLiteral("Shows or hides the lap and metric settings."));
    toolBar_->addAction(sidebarAction_);
    toolBar_->addSeparator();

    viewGroup_ = new QActionGroup(this);
    viewGroup_->setExclusive(true);
    auto addView = [this](QAction*& action, std::initializer_list<const char*> icons,
                          QStyle::StandardPixmap fallback, const QString& text,
                          const QString& help, View view) {
        action = makeToggle(this, themed(this, icons, fallback), text, help);
        action->setData(static_cast<int>(view));
        viewGroup_->addAction(action);
        toolBar_->addAction(action);
        if (auto* button = qobject_cast<QToolButton*>(toolBar_->widgetForAction(action)))
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    };
    addView(graphsAction_, {"office-chart-line", "labplot-xy-curve"}, QStyle::SP_FileDialogContentsView,
            QStringLiteral("Graphs"), QStringLiteral("Plots the selected metrics of both laps."),
            View::Graphs);
    addView(splitAction_, {"view-split-left-right"}, QStyle::SP_FileDialogListView,
            QStringLiteral("Split"), QStringLiteral("Graphs and the circuit map side by side."),
            View::Split);
    addView(mapAction_, {"map-flat", "globe"}, QStyle::SP_DriveNetIcon, QStringLiteral("Map"),
            QStringLiteral("Both laps as cars on the circuit map."), View::Map);
    toolBar_->addSeparator();

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
    inputsAction_ = makeToggle(this, themed(this, {"input-gaming", "view-list-details"},
                                            QStyle::SP_FileDialogDetailedView),
                               QStringLiteral("Data Comparison"),
                               QStringLiteral("Shows a card on the map with both laps' steering, "
                                              "pedals, ERS, speed and gear at the map cursor."));
    for (QAction* action : {stackedAction_, syncedTooltipAction_, sectorBoundariesAction_,
                            sectorDeltaAction_}) {
        toolBar_->addAction(action);
        if (action == syncedTooltipAction_) toolBar_->addSeparator();
    }
    toolBar_->addSeparator();
    toolBar_->addAction(inputsAction_);

    auto* spacer = new QWidget(toolBar_);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolBar_->addWidget(spacer);
    deltaReadout_ = new AnalysisDeltaReadout(toolBar_);
    toolBar_->addWidget(deltaReadout_);

    helpAction_ = new QAction(themed(this, {"help-contextual", "help-about"}, QStyle::SP_MessageBoxQuestion),
                              QStringLiteral("Analysis Help"), this);
    helpAction_->setToolTip(QStringLiteral("What the Analysis controls do"));
    toolBar_->addAction(helpAction_);

    connect(sidebarAction_, &QAction::toggled, this, [this](bool on) {
        sidebar_->setVisible(on);
        saveSettings();
    });
    connect(viewGroup_, &QActionGroup::triggered, this, [this](QAction* action) {
        preferredView_ = static_cast<View>(action->data().toInt());
        saveSettings();
        applyState();
    });
    connect(sectorBoundariesAction_, &QAction::toggled, this, [this](bool on) {
        if (!on) sectorDeltaAction_->setChecked(false);
    });
    for (QAction* action : {stackedAction_, syncedTooltipAction_, sectorBoundariesAction_,
                            sectorDeltaAction_, inputsAction_})
        connect(action, &QAction::triggered, this, [this] {
            saveSettings();
            applyState();
        });
    connect(helpAction_, &QAction::triggered, this, &AnalysisPage::showControlsHelp);
}

QWidget* AnalysisPage::buildSidebar() {
    sidebar_ = new QWidget(this);
    auto* layout = new QVBoxLayout(sidebar_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Laps scroll when the window is short; the metric list takes what remains.
    auto* scroll = new QScrollArea(sidebar_);
    lapScroll_ = scroll;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* upper = new QWidget(scroll);
    auto* upperLayout = new QVBoxLayout(upper);
    upperLayout->setContentsMargins(8, 8, 8, 4);
    upperLayout->setSpacing(8);
    upperLayout->addWidget(buildRecordingGroup());
    upperLayout->addWidget(buildLapGroup());
    scroll->setWidget(upper);

    auto* sections = new QSplitter(Qt::Vertical, sidebar_);
    sections->setChildrenCollapsible(false);
    sections->setObjectName(QStringLiteral("analysisSidebarSections"));
    sections->addWidget(scroll);
    sections->addWidget(buildMetricGroup());
    sections->setStretchFactor(0, 0);
    sections->setStretchFactor(1, 1);
    layout->addWidget(sections, 1);

    sidebar_->setMinimumWidth(280);
    return sidebar_;
}

QWidget* AnalysisPage::buildRecordingGroup() {
    recordingGroup_ = new QGroupBox(sidebar_);
    auto* layout = new QVBoxLayout(recordingGroup_);
    layout->setSpacing(6);

    auto* row = new QHBoxLayout;
    row->setSpacing(4);
    secondaryName_ = new QLabel(recordingGroup_);
    secondaryName_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    secondaryName_->setTextFormat(Qt::PlainText);
    openSecondary_ = new QToolButton(recordingGroup_);
    openSecondary_->setIcon(themed(this, {"document-open"}, QStyle::SP_DialogOpenButton));
    openSecondary_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    removeSecondary_ = new QToolButton(recordingGroup_);
    removeSecondary_->setIcon(themed(this, {"edit-delete-remove", "list-remove"},
                                     QStyle::SP_DialogCloseButton));
    removeSecondary_->setToolTip(QStringLiteral("Close the second recording"));
    removeSecondary_->setAutoRaise(true);
    row->addWidget(secondaryName_, 1);
    row->addWidget(openSecondary_);
    row->addWidget(removeSecondary_);
    layout->addLayout(row);

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

    return recordingGroup_;
}

QWidget* AnalysisPage::buildLapGroup() {
    auto* group = new QWidget(sidebar_);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    modeTabs_ = new QTabBar(group);
    modeTabs_->setDocumentMode(true);
    modeTabs_->setExpanding(true);
    modeTabs_->setDrawBase(false);
    modeTabs_->addTab(QStringLiteral("Follow Playback"));
    modeTabs_->addTab(QStringLiteral("Fixed Laps"));
    modeTabs_->setTabToolTip(kPlaybackTab,
                             QStringLiteral("Compare the lap under the playback cursor with a chosen lap"));
    modeTabs_->setTabToolTip(kFixedTab, QStringLiteral("Compare any two laps, from either recording"));
    layout->addWidget(modeTabs_);

    slotPages_ = new QStackedWidget(group);
    auto makePage = [this](AnalysisLapSlot* first, AnalysisLapSlot* second) {
        auto* page = new QWidget(slotPages_);
        auto* pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        pageLayout->setSpacing(8);
        pageLayout->addWidget(first);
        pageLayout->addWidget(second);
        pageLayout->addStretch(1);
        slotPages_->addWidget(page);
    };
    currentSlot_ = new AnalysisLapSlot(QStringLiteral("Current Lap"), QStringLiteral("Current"),
                                       AnalysisLapSlot::Mode::FollowsPlayback);
    compareSlot_ = new AnalysisLapSlot(QStringLiteral("Compare With"), QStringLiteral("Compare"),
                                       AnalysisLapSlot::Mode::Selectable);
    lapASlot_ = new AnalysisLapSlot(QStringLiteral("Lap A"), QStringLiteral("Lap A"),
                                    AnalysisLapSlot::Mode::Selectable);
    lapBSlot_ = new AnalysisLapSlot(QStringLiteral("Lap B"), QStringLiteral("Lap B"),
                                    AnalysisLapSlot::Mode::Selectable);
    makePage(currentSlot_, compareSlot_);
    makePage(lapASlot_, lapBSlot_);
    layout->addWidget(slotPages_);
    connect(slotPages_, &QStackedWidget::currentChanged, this, &AnalysisPage::fitLapPanel);
    return group;
}

QWidget* AnalysisPage::buildMetricGroup() {
    auto* group = new QGroupBox(QStringLiteral("Metrics"), sidebar_);
    auto* layout = new QVBoxLayout(group);
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
    addMetricsAction_ = new QAction(themed(this, {"list-add"}, QStyle::SP_FileDialogNewFolder),
                                    QStringLiteral("Add Metrics…"), this);
    addMetricsAction_->setToolTip(QStringLiteral("Choose the metrics to chart"));

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    auto* add = new QToolButton(group);
    add->setIcon(themed(this, {"list-add"}, QStyle::SP_FileDialogNewFolder));
    add->setText(QStringLiteral("Add"));
    add->setToolTip(QStringLiteral("Choose the metrics to chart"));
    add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(add, &QToolButton::clicked, this, [this, add] { picker_->popup(add); });
    connect(addMetricsAction_, &QAction::triggered, this, [this] { picker_->popup(seriesView_); });
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
        menu.addAction(addMetricsAction_);
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
    setChecked(sidebarAction_, !settings_.value("analyze/collapsed", false).toBool());
    setChecked(stackedAction_, settings_.value("analyze/individualGraphs", false).toBool());
    setChecked(syncedTooltipAction_, settings_.value("analyze/syncedTooltip", false).toBool());
    const bool boundaries = settings_.value("analyze/sectorBoundaries", false).toBool();
    setChecked(sectorBoundariesAction_, boundaries);
    setChecked(sectorDeltaAction_, boundaries && settings_.value("analyze/sectorDelta", false).toBool());
    setChecked(inputsAction_, settings_.value("analyze/inputsPanel", true).toBool());
    sidebar_->setVisible(sidebarAction_->isChecked());

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
    settings_.setValue("analyze/version", kSettingsVersion);
    settings_.setValue("analyze/collapsed", !sidebarAction_->isChecked());
    settings_.setValue("analyze/view", viewKey(static_cast<int>(preferredView_)));
    settings_.setValue("analyze/individualGraphs", stackedAction_->isChecked());
    settings_.setValue("analyze/syncedTooltip", syncedTooltipAction_->isChecked());
    settings_.setValue("analyze/sectorBoundaries", sectorBoundariesAction_->isChecked());
    settings_.setValue("analyze/sectorDelta", sectorDeltaAction_->isChecked());
    settings_.setValue("analyze/inputsPanel", inputsAction_->isChecked());
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

    if (splitter_) settings_.setValue("analyze/splitter", splitter_->saveState());
    if (contentSplitter_) settings_.setValue("analyze/contentSplitter", contentSplitter_->saveState());
    if (auto* sections = sidebar_->findChild<QSplitter*>(QStringLiteral("analysisSidebarSections")))
        settings_.setValue("analyze/sidebarSections", sections->saveState());
}

void AnalysisPage::restoreLayout() {
    if (!splitter_->restoreState(settings_.value("analyze/splitter").toByteArray()))
        splitter_->setSizes({320, 960});
    contentSplitter_->restoreState(settings_.value("analyze/contentSplitter").toByteArray());
    if (auto* sections = sidebar_->findChild<QSplitter*>(QStringLiteral("analysisSidebarSections"))) {
        connect(sections, &QSplitter::splitterMoved, this, &AnalysisPage::saveSettings);
        if (!sections->restoreState(settings_.value("analyze/sidebarSections").toByteArray()))
            sections->setSizes({420, 300});
    }
    // restoreState() also restores visibility; the view and sidebar actions own that.
    sidebar_->setVisible(sidebarAction_->isChecked());
    chart_->show();
    map_->show();
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
    if (secondaryLoading_) {
        secondaryName_->setText(QStringLiteral("Loading…"));
        secondaryName_->setToolTip({});
    } else if (secondary_) {
        secondaryName_->setText(secondary_->filename);
        secondaryName_->setToolTip(secondary_->path);
    } else {
        secondaryName_->setText(QStringLiteral("No second recording"));
        secondaryName_->setToolTip({});
    }
    secondaryName_->setForegroundRole(secondary_ && !secondaryLoading_ ? QPalette::WindowText
                                                                       : QPalette::PlaceholderText);
    openSecondary_->setText(secondary_ ? QStringLiteral("Replace…") : QStringLiteral("Open…"));
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
    inputsAction_->setEnabled(showMap);

    // ── Sidebar ──
    const bool haveLaps = playback_ && !model_->data().laps.isEmpty();
    modeTabs_->setTabEnabled(kFixedTab, haveLaps);
    if (!haveLaps && modeTabs_->currentIndex() == kFixedTab) {
        QSignalBlocker guard(modeTabs_);
        modeTabs_->setCurrentIndex(kPlaybackTab);
        slotPages_->setCurrentIndex(kPlaybackTab);
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
    chart_->setMapCursors(showMap && showChart && fixed, primaryColor_, comparisonColor_);

    // ── Map ──
    showFollowedLap(primary.lap);
    map_->setColors(primaryColor_, comparisonColor_);
    map_->setLabels(primaryLabel, comparisonLabel);
    map_->setReadoutVisible(inputsAction_->isChecked());
    map_->setCurrentTime(currentTime_);
    chart_->setVisible(showChart);
    map_->setVisible(showMap);

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

void AnalysisPage::resetPlaybackSelections() {
    {
        QSignalBlocker guard(modeTabs_);
        modeTabs_->setCurrentIndex(kPlaybackTab);
        slotPages_->setCurrentIndex(kPlaybackTab);
    }
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

void AnalysisPage::inspectMap(double coordinate, bool distanceCoordinate) {
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
    if (preferredView_ == View::Graphs) {
        preferredView_ = View::Split;
        saveSettings();
    }
    applyState();
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
                            inputsAction_, sidebarAction_})
        row(views, action->icon(), action->text(), action->whatsThis());

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
