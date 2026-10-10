#pragma once

#include "AnalysisRecording.h"

#include <QColor>
#include <QSettings>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <memory>

class AnalysisDeltaReadout;
class AnalysisFileReader;
class AnalysisLapSlot;
class AnalysisMapView;
class AnalysisMetricPicker;
class AnalysisSeriesDelegate;
class AnalysisSeriesModel;
class AnalysisSplitter;
class UnderlineTabBar;
class AnalyzeChart;
class SessionModel;
class QAction;
class QActionGroup;
class QFrame;
class QGroupBox;
class QLabel;
class QListView;
class QMenu;
class QScrollArea;
class QSplitter;
class QToolBar;
class QToolButton;
struct AnalysisFileCatalog;
struct PlaybackHistoryBatch;

// Lap analysis: telemetry of one lap plotted against another, on graphs, on
// the circuit map, or both side by side.
//
//  ┌ page tool bar: sidebar · Graphs|Split|Map · graph options · delta readout ┐
//  ├──────────────┬─────────────────────────────────────────────────────────────┤
//  │ Recordings   │                                                             │
//  │ Laps         │   graphs   ║   map + Data Comparison card                  │
//  │  (Playback | │            ║   [replay transport for fixed laps]            │
//  │   Fixed)     │                                                             │
//  │ Metrics      │                                                             │
//  └──────────────┴─────────────────────────────────────────────────────────────┘
//
// In Playback mode the primary lap follows the playback cursor and one lap is
// chosen to compare with it. In Fixed mode two arbitrary laps are compared,
// from any driver of the primary recording or of a second recording.
class AnalysisPage : public QWidget {
    Q_OBJECT
public:
    explicit AnalysisPage(SessionModel* model, QWidget* parent = nullptr);
    ~AnalysisPage() override;

    void setPrimaryRecording(int trackId, const QString& trackName);
    void setPrimaryCatalog(const tnrp::PlaybackLapBlocksRow& catalog);
    void installPrimaryLap(uint64_t generation, int driverIndex, int lapNum,
                           uint32_t rowTypeMask,
                           const std::shared_ptr<PlaybackHistoryBatch>& batch);
    void setPlaybackMode(bool on, float currentTime = 0);
    void setCurrentTime(float t);
    void setMapAppearance(bool sectorColors, int opacityPercent);
    void setAeroMode(bool slm);   // false = DRS (F1 24/25), true = SLM (F1 26)
    void resetPlaybackSelections();
    uint32_t playbackRowMask() const;

    // Screenshot tour: Fixed Laps comparing two laps of the open recording in
    // the Split view, charting exactly `metricIds` in that order. Returns an
    // empty string on success, otherwise why it could not be set up. The laps
    // load asynchronously; focusMapElapsed places the map/chart cursor once
    // they have arrived.
    QString stageComparison(const QString& driverA, int lapA,
                            const QString& driverB, int lapB,
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
    QToolBar* toolBar_ = nullptr;
    QAction* sidebarAction_ = nullptr;
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
    QAction* helpAction_ = nullptr;
    AnalysisDeltaReadout* deltaReadout_ = nullptr;

    // ── Sidebar ──
    QSplitter* splitter_ = nullptr;
    QWidget* sidebar_ = nullptr;
    QScrollArea* lapScroll_ = nullptr;
    QGroupBox* recordingGroup_ = nullptr;
    QLabel* secondaryName_ = nullptr;
    QToolButton* openSecondary_ = nullptr;
    QToolButton* removeSecondary_ = nullptr;
    QFrame* messageBar_ = nullptr;
    QLabel* messageText_ = nullptr;
    UnderlineTabBar* modeTabs_ = nullptr;
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

    void buildToolBar();
    QWidget* buildSidebar();
    QWidget* buildRecordingGroup();
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
    void applyState();
    void showFollowedLap(const LapBlock* lap);
    void refreshDelta();

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
    void inspectMap(double coordinate, bool distanceCoordinate);
};
