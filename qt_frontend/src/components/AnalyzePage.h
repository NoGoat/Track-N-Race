#pragma once

#include "AnalyzeMetrics.h"
#include <QSettings>
#include <QWidget>
#include <cstdint>
#include <memory>

class AnalysisFileReader;
class AnalyzeChart;
class AnalyzeMapComparison;
class SessionModel;
class QCheckBox;
class QComboBox;
class QFrame;
class QListWidget;
class QLabel;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QToolButton;
struct AnalysisFileCatalog;
struct PlaybackHistoryBatch;

class AnalyzePage : public QWidget {
    Q_OBJECT
public:
    explicit AnalyzePage(SessionModel* model, QWidget* parent = nullptr);
    ~AnalyzePage() override;
    QWidget* toolbarControls() const { return toolbarControls_; }
    void setPrimaryRecording(int trackId, const QString& trackName);
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
    QComboBox* compareLap_ = nullptr;
    QComboBox* lapA_ = nullptr;
    QComboBox* lapB_ = nullptr;
    QListWidget* seriesList_ = nullptr;
    QPushButton* collapse_ = nullptr;
    QToolButton* inspectorButton_ = nullptr;
    QSplitter* contentSplitter_ = nullptr;
    QWidget* toolbarControls_ = nullptr;
    QPushButton* compareClear_ = nullptr;
    QWidget* secondaryFileRow_ = nullptr;
    QLabel* secondaryFileLabel_ = nullptr;
    QLabel* secondaryErrorLabel_ = nullptr;
    QLabel* deltaStatus_ = nullptr;
    QPushButton* secondaryOpen_ = nullptr;
    QPushButton* secondaryClear_ = nullptr;
    AnalysisFileReader* secondaryReader_ = nullptr;
    AnalyzeMapComparison* map_ = nullptr;
    QStackedWidget* viewStack_ = nullptr;
    QPushButton* mapCurrentColor_ = nullptr;
    QPushButton* mapComparisonColor_ = nullptr;
    struct SecondaryState;
    std::unique_ptr<SecondaryState> secondary_;
    QVector<AnalyzeSeriesSetting> series_;
    QSettings settings_{"TrackNRace","NativeRecorder"};
    bool playback_ = false;
    bool collapsed_ = false;
    bool secondaryLoading_ = false;
    int primaryTrackId_ = -1;
    QString primaryTrackName_;
    QString preferredView_ = "graph";
    float currentTime_ = 0;
    QColor mapCurrent_{"#5794F2"};
    QColor mapComparison_{"#C4162A"};

    void loadSettings();
    void saveSettings();
    void rebuildMetricPicker();
    void rebuildSeriesList();
    void refreshLapSelectors();
    void applyState();
    void moveSeries(int from, int to);
    void loadSecondaryFile();
    void applySecondaryFile(const std::shared_ptr<AnalysisFileCatalog>& catalog);
    void clearSecondaryFile(bool clearFixedSelections = true);
    void installSecondaryLap(uint64_t generation, int lapNum, uint32_t rowTypeMask,
                             const std::shared_ptr<PlaybackHistoryBatch>& batch);
    void requestSecondaryLaps();
    void inspectMap(double coordinate, bool distanceCoordinate);
};
