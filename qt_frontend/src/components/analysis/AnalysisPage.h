#pragma once

#include "AnalysisRecording.h"

#include <QColor>
#include <QSettings>
#include <QSize>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <memory>

#include <tnrp/AnalysisMap.h>

class AnalysisBrakingZones;
class AnalysisDeltaReadout;
class AnalysisDockRail;
class AnalysisFileReader;
class AnalysisLapSlot;
class AnalysisMapView;
class AnalysisMetricPicker;
class AnalysisSeriesDelegate;
class AnalysisSeriesModel;
class AnalysisSplitter;
class AnalysisToolStrip;
class AnalysisWorkspace;
class UnderlineTabBar;
class AnalyzeChart;
class SessionModel;
class QAction;
class QActionGroup;
class QComboBox;
class QDockWidget;
class QFrame;
class QLineEdit;
class QLabel;
class QListView;
class QMenu;
class QScrollArea;
class QSplitter;
class QTimer;
class QToolBar;
class QToolButton;
struct AnalysisFileCatalog;
struct PlaybackHistoryBatch;

// Lap analysis: telemetry of one lap plotted against another, on graphs, on
// the circuit map, or both side by side.
//
//  ┌ page tool bar: Graphs|Split|Map · Panels ▾ · Help ──────────────────────────────┐
//  ├──────────┬──┬─────────────────────┬──┬──────────────────────┬──┬──────────────┤
//  │ Laps     │G │ S1 · S2 · S3 · Lap  │  │                      │M │ Braking      │
//  │ (dock)   │r │─────────────────────│  │  map + Data card     │a │ Zones        │
//  │──────────│a │                     │  │                      │p │ (dock)       │
//  │ Metrics  │p │   graphs            │  │  [lap replay]        │  │              │
//  │ (dock)   │h │                     │  │                      │  │              │
//  └──────────┴──┴─────────────────────┴──┴──────────────────────┴──┴──────────────┘
//
// The docks live in an inner QMainWindow (AnalysisWorkspace): each can be
// floated, moved, tabbed or closed, and a closed one leaves a tab on its window
// edge. Each pane has a vertical tool strip holding the options that change it.
//
// In Playback mode the primary lap follows the playback cursor and one lap is
// chosen to compare with it. In Fixed mode two arbitrary laps are compared,
// from any driver of the primary recording or of a second recording.
class AnalysisPage : public QWidget {
    Q_OBJECT
public:
    explicit AnalysisPage(SessionModel* model, QWidget* parent = nullptr);
    ~AnalysisPage() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

public:

    void setPrimaryRecording(int trackId, const QString& trackName);
    void setPrimaryCatalog(const tnrp::PlaybackLapBlocksRow& catalog);
    void installPrimaryLap(uint64_t generation, int driverIndex, int lapNum,
                           uint32_t rowTypeMask,
                           const std::shared_ptr<PlaybackHistoryBatch>& batch);
    void setPlaybackMode(bool on, float currentTime = 0);
    void setCurrentTime(float t);
    void setMapAppearance(bool sectorColors, int opacityPercent);
    void setAeroMode(bool slm);   // false = DRS (F1 24/25), true = SLM (F1 26)
    // Reads the playback recording for the map layers (tnrp/AnalysisMap.h).
    void setPrimaryRecordingAccess(tnrp::RecordingAccess access);
    // Shows the Braking Zones panel again if it was closed.
    void showBrakingZones();
    // The page's controls that live in the app tool bar: Graphs | Split | Map,
    // and Panels + Help (AppToolbar::setAnalyzePageWidgets).
    QWidget* viewControls() const { return viewControls_; }
    QWidget* panelControls() const { return panelControls_; }
    void resetPlaybackSelections();
    uint32_t playbackRowMask() const;

    // Screenshot tour: Fixed Laps comparing two laps of the open recording in
    // the Split view, labelled `labelA`/`labelB`, charting exactly `metricIds`
    // in that order. Returns an empty string on success, otherwise why it
    // could not be set up. The laps load asynchronously; focusMapElapsed
    // places the map/chart cursor once they have arrived.
    QString stageComparison(const QString& driverA, int lapA, const QString& labelA,
                            const QString& driverB, int lapB, const QString& labelB,
                            const QStringList& metricIds);
    void focusMapElapsed(double seconds);

public slots:
    void zoomIn();
    void zoomOut();
    void panLeft();
    void panRight();
    void resetZoom();

signals:
    void navigationEnabledChanged(bool enabled);
    void dataRequirementsChanged();
    void primaryLapDataRequested(uint64_t generation, int driverIndex, int lapNum,
                                 uint32_t rowTypeMask);

private:
    enum class View { Graphs, Split, Map };

    // A lap resolved to its owning data, as handed to the chart and map.
    struct ResolvedLap {
        const SessionData* data = nullptr;
        const LapBlock* lap = nullptr;
        int trackId = -1;
    };

    SessionModel* model_ = nullptr;
    QSettings settings_{QStringLiteral("TrackNRace"), QStringLiteral("NativeRecorder")};

    // ── Tool bar ──
    QWidget* viewControls_ = nullptr;
    QWidget* panelControls_ = nullptr;
    QActionGroup* viewGroup_ = nullptr;
    QAction* graphsAction_ = nullptr;
    QAction* splitAction_ = nullptr;
    QAction* mapAction_ = nullptr;
    QAction* stackedAction_ = nullptr;
    QAction* syncedTooltipAction_ = nullptr;
    QAction* sectorBoundariesAction_ = nullptr;
    QAction* sectorDeltaAction_ = nullptr;
    QAction* splitCursorsAction_ = nullptr;
    QAction* inputsAction_ = nullptr;
    QAction* brakingAction_ = nullptr;
    QActionGroup* layerGroup_ = nullptr;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QAction* resetZoomAction_ = nullptr;
    QAction* helpAction_ = nullptr;
    QToolButton* panelsButton_ = nullptr;
    QAction* edgeTabsAction_ = nullptr;
    QAction* lockPanelsAction_ = nullptr;
    QAction* resizePanelsAction_ = nullptr;
    QAction* resetLayoutAction_ = nullptr;
    AnalysisDeltaReadout* deltaReadout_ = nullptr;

    // ── Panels ──
    AnalysisWorkspace* workspace_ = nullptr;
    QDockWidget* lapsDock_ = nullptr;
    QDockWidget* metricsDock_ = nullptr;
    QDockWidget* zonesDock_ = nullptr;
    AnalysisDockRail* leftRail_ = nullptr;
    AnalysisDockRail* rightRail_ = nullptr;
    AnalysisBrakingZones* zones_ = nullptr;
    QScrollArea* lapScroll_ = nullptr;
    QWidget* recordingGroup_ = nullptr;
    QLineEdit* secondaryField_ = nullptr;
    QAction* openSecondary_ = nullptr;
    QAction* removeSecondary_ = nullptr;
    QFrame* messageBar_ = nullptr;
    QLabel* messageText_ = nullptr;
    UnderlineTabBar* modeTabs_ = nullptr;   // Follow Playback | Fixed Laps
    QVector<QWidget*> slotPages_;   // Follow Playback, Fixed Laps
    AnalysisLapSlot* currentSlot_ = nullptr;
    AnalysisLapSlot* compareSlot_ = nullptr;
    AnalysisLapSlot* lapASlot_ = nullptr;
    AnalysisLapSlot* lapBSlot_ = nullptr;
    AnalysisSeriesModel* seriesModel_ = nullptr;
    AnalysisSeriesDelegate* seriesDelegate_ = nullptr;
    QListView* seriesView_ = nullptr;
    AnalysisMetricPicker* picker_ = nullptr;
    QAction* removeMetricAction_ = nullptr;
    QAction* moveUpAction_ = nullptr;
    QAction* moveDownAction_ = nullptr;
    QAction* changeColorAction_ = nullptr;
    QAction* resetColorAction_ = nullptr;
    QAction* yAxisAction_ = nullptr;
    QAction* allYAxesAction_ = nullptr;

    // ── Content ──
    AnalysisSplitter* contentSplitter_ = nullptr;
    AnalyzeChart* chart_ = nullptr;
    AnalysisMapView* map_ = nullptr;
    QWidget* graphPane_ = nullptr;   // graph strip, result band, graphs
    QWidget* mapPane_ = nullptr;     // map, map strip
    AnalysisToolStrip* graphStrip_ = nullptr;
    AnalysisToolStrip* mapStrip_ = nullptr;

    // ── State ──
    std::unique_ptr<AnalysisRecording> primary_;
    std::unique_ptr<AnalysisRecording> secondary_;
    AnalysisFileReader* secondaryReader_ = nullptr;
    bool secondaryLoading_ = false;
    bool playback_ = false;
    float currentTime_ = 0;
    int primaryTrackId_ = -1;
    QString primaryTrackName_;
    int currentDriverIndex_ = -1;
    uint64_t primaryGeneration_ = 0;
    bool primaryCatalogReady_ = false;
    View preferredView_ = View::Graphs;
    QColor primaryColor_{"#5794F2"};
    QColor comparisonColor_{"#C4162A"};
    ResolvedLap shownPrimary_;
    ResolvedLap shownComparison_;
    bool shownFixed_ = false;
    // Map layers: the playback recording's reader, and what was last asked of
    // libtnrp for the layer and for braking points (empty: nothing).
    tnrp::RecordingAccess primaryAccess_;
    QString layerKey_;
    QString brakingKey_;
    std::shared_ptr<const tnrp::AnalysisMapResult> brakingResult_;
    // False until the saved settings are applied: the default dock layout is
    // built first and must not overwrite them.
    bool settingsReady_ = false;
    bool lapsFittedOnShow_ = false;
    // Panel sizes change by dragging separators, which Qt does not announce;
    // a short timer after the docks resize saves them.
    QTimer* layoutSaveTimer_ = nullptr;
    // Panel sizes per arrangement of one window side: the key names every
    // open, docked panel on that side, so a panel opening or closing on the
    // other side leaves these sizes alone. When an arrangement comes back (a
    // panel is reopened, re-docked or moved back) its panels return to these
    // sizes; an arrangement not seen before takes each panel's latest size.
    // Saved with the settings.
    QHash<QString, QHash<QString, QSize>> layoutMemory_;
    QHash<QString, QSize> latestPanelSizes_;

    void buildToolBar();
    QWidget* buildGraphPane();
    QWidget* buildMapPane();
    void buildDocks();
    QWidget* buildRecordingGroup();
    QWidget* buildModeTabs();
    QWidget* buildLapGroup();
    QWidget* buildMetricGroup();
    void loadSettings();
    void saveSettings();
    void restoreLayout();

    bool fixedMode() const;
    View effectiveView() const;
    bool circuitsMismatched() const;
    AnalysisRecording* recordingFor(const AnalysisDriverRef& driver) const;
    AnalysisDriverRef followedDriver() const;

    void refreshDriverChoices();
    void refreshLapChoices();
    void refreshFollowedSlot();
    void refreshSecondaryRow();
    void refreshMetricActions();
    void showSlotPage(int index);
    void fitLapPanel();
    void fitLapsDock();
    void applyState();
    void showFollowedLap(const LapBlock* lap);
    void refreshDelta();
    void requestMapLayers();
    QString layerMode() const;

    // Panels: the Panels menu (also the docks' right-click menu), edge tabs
    // for closed docks, Lock Panels and Reset Layout.
    void fillPanelsMenu(QMenu* menu);
    void refreshRails();
    void applyPanelLock();
    void resetPanels();
    // `closing`: a panel being closed, still counted as open.
    QString arrangementKey(Qt::DockWidgetArea side, QDockWidget* closing = nullptr) const;
    void recordArrangement(QDockWidget* closing = nullptr);
    void restoreArrangement();

    void setPrimaryColor(const QColor& color);
    void setComparisonColor(const QColor& color);
    void showMessage(const QString& text);
    // `part`: {} for the series colour, the delegate's negative part, or a corner key.
    void chooseSeriesColor(int row, const QString& part);
    int currentSeriesRow() const;
    void showControlsHelp();

    void loadSecondaryFile();
    void onSecondaryCatalog(const std::shared_ptr<AnalysisFileCatalog>& catalog);
    void clearSecondaryFile(bool clearMessage);
    void resetSecondarySelections();
    void installLap(AnalysisRecording& recording, int driverIndex, int lapNum,
                    uint32_t rowTypeMask, const std::shared_ptr<PlaybackHistoryBatch>& batch);
    void requestAnalysisLaps();
    // Puts the map cursor at a point of the primary lap; `showMap` switches
    // Graphs view to Split so the map is on screen.
    void inspectMap(double coordinate, bool distanceCoordinate, bool showMap = true);
};
