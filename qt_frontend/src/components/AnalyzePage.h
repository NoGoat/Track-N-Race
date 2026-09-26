#pragma once

#include "AnalyzeMetrics.h"
#include <QSettings>
#include <QWidget>
#include <cstdint>
#include <memory>

class AnalysisFileReader;
class AnalyzeChart;
class AnalyzeMapComparison;
class AnalyzeSelectionGroup;
class ClearableComboBox;
class SessionModel;
class QCheckBox;
class QComboBox;
class QFrame;
class QListWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class QSplitter;
class QToolButton;
struct AnalysisFileCatalog;
struct LapBlock;
struct PlaybackHistoryBatch;
struct SessionData;
namespace tnrp { struct PlaybackLapBlocksRow; }

class AnalyzePage : public QWidget {
    Q_OBJECT
public:
    explicit AnalyzePage(SessionModel* model, QWidget* parent = nullptr);
    ~AnalyzePage() override;
    QWidget* toolbarControls() const { return toolbarControls_; }
    void setPrimaryRecording(int trackId, const QString& trackName);
    void setPrimaryCatalog(const tnrp::PlaybackLapBlocksRow& catalog);
    void installPrimaryLap(uint64_t generation, int driverIndex, int lapNum,
                           uint32_t rowTypeMask,
                           const std::shared_ptr<PlaybackHistoryBatch>& batch);
    void setPlaybackMode(bool on, float currentTime = 0);
    void setCurrentTime(float t);
    void setMapAppearance(bool sectorColors, int opacityPercent);
    void resetPlaybackSelections();
    uint32_t playbackRowMask() const;
    QVector<int> requestedPlaybackLaps() const;

public slots:
    void zoomIn();
    void zoomOut();
    void panLeft();
    void panRight();
    void resetZoom();

signals:
    void navigationEnabledChanged(bool enabled);
    void dataRequirementsChanged();
    void primaryLapDataRequested(uint64_t generation, int driverIndex,
                                 int lapNum, uint32_t rowTypeMask);

private:
    SessionModel* model_ = nullptr;
    AnalyzeChart* chart_ = nullptr;
    QFrame* sidebar_ = nullptr;
    QComboBox* addMetric_ = nullptr;
    QComboBox* viewMode_ = nullptr;
    QCheckBox* fixedMode_ = nullptr;
    QCheckBox* showYAxis_ = nullptr;
    QCheckBox* individualGraphs_ = nullptr;
    QCheckBox* syncedTooltip_ = nullptr;
    QCheckBox* sectorBoundaries_ = nullptr;
    QCheckBox* sectorDelta_ = nullptr;
    QComboBox* currentDriver_ = nullptr;
    QComboBox* compareDriver_ = nullptr;
    QComboBox* lapADriver_ = nullptr;
    QComboBox* lapBDriver_ = nullptr;
    ClearableComboBox* compareLap_ = nullptr;
    ClearableComboBox* lapA_ = nullptr;
    ClearableComboBox* lapB_ = nullptr;
    AnalyzeSelectionGroup* currentGroup_ = nullptr;
    AnalyzeSelectionGroup* compareGroup_ = nullptr;
    AnalyzeSelectionGroup* lapAGroup_ = nullptr;
    AnalyzeSelectionGroup* lapBGroup_ = nullptr;
    QLabel* currentLapDisplay_ = nullptr;
    QLineEdit* currentLabelEdit_ = nullptr;
    QLineEdit* compareLabelEdit_ = nullptr;
    QLineEdit* lapALabelEdit_ = nullptr;
    QLineEdit* lapBLabelEdit_ = nullptr;
    QListWidget* seriesList_ = nullptr;
    QPushButton* collapse_ = nullptr;
    QToolButton* inspectorButton_ = nullptr;
    QSplitter* contentSplitter_ = nullptr;
    QWidget* toolbarControls_ = nullptr;
    QWidget* secondaryFileRow_ = nullptr;
    QLabel* secondaryFileLabel_ = nullptr;
    QLabel* secondaryErrorLabel_ = nullptr;
    QLabel* deltaStatus_ = nullptr;
    QPushButton* secondaryOpen_ = nullptr;
    QPushButton* secondaryClear_ = nullptr;
    AnalysisFileReader* secondaryReader_ = nullptr;
    AnalyzeMapComparison* map_ = nullptr;
    QWidget* viewContainer_ = nullptr;
    QWidget* viewDivider_ = nullptr;
    QPushButton* mapCurrentColor_ = nullptr;
    QPushButton* mapComparisonColor_ = nullptr;
    QPushButton* mapLapAColor_ = nullptr;
    QPushButton* mapLapBColor_ = nullptr;
    QWidget* deltaSummary_ = nullptr;
    QVector<QWidget*> deltaSectorItems_;
    QVector<QLabel*> deltaValues_;
    QToolButton* helpButton_ = nullptr;
    struct FileState;
    std::unique_ptr<FileState> primary_;
    std::unique_ptr<FileState> secondary_;
    QVector<AnalyzeSeriesSetting> series_;
    QSettings settings_{"TrackNRace","NativeRecorder"};
    bool playback_ = false;
    bool collapsed_ = false;
    bool secondaryLoading_ = false;
    int primaryTrackId_ = -1;
    QString primaryTrackName_;
    int currentPrimaryDriverIndex_ = -1;
    uint64_t primaryGeneration_ = 0;
    bool primaryCatalogReady_ = false;
    QString preferredView_ = "graph";
    float currentTime_ = 0;
    QColor mapCurrent_{"#5794F2"};
    QColor mapComparison_{"#C4162A"};
    QString currentLabel_;
    QString compareLabel_;
    QString lapALabel_;
    QString lapBLabel_;
    const SessionData* summaryPrimaryData_ = nullptr;
    const SessionData* summaryComparisonData_ = nullptr;
    const LapBlock* summaryPrimary_ = nullptr;
    const LapBlock* summaryComparison_ = nullptr;
    bool summaryFixed_ = false;
    bool summaryCompatible_ = true;

    void loadSettings();
    void saveSettings();
    void rebuildMetricPicker();
    void rebuildSeriesList();
    void refreshDriverSelectors();
    void refreshLapSelectors();
    void applyState();
    void moveSeries(int from, int to);
    void loadSecondaryFile();
    void applySecondaryFile(const std::shared_ptr<AnalysisFileCatalog>& catalog);
    void clearSecondaryFile(bool clearFixedSelections = true);
    void installSecondaryLap(uint64_t generation, int driverIndex, int lapNum,
                             uint32_t rowTypeMask,
                             const std::shared_ptr<PlaybackHistoryBatch>& batch);
    void installAnalysisLap(FileState& file, int driverIndex, int lapNum,
                            uint32_t rowTypeMask,
                            const std::shared_ptr<PlaybackHistoryBatch>& batch);
    void requestAnalysisLaps();
    void inspectMap(double coordinate, bool distanceCoordinate);
    bool circuitsMismatched() const;
    void refreshDeltaSummary();
    void showControlsHelp();
};
