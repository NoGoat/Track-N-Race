#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>

#include <tnrp/control_rows.h>

struct PlaybackHistoryBatch;

struct AnalysisFileCatalog {
    uint64_t generation = 0;
    QString path;
    QString filename;
    QString trackName;
    int trackId = 0;
    tnrp::PlaybackLapBlocksRow laps;
};

// Independent, lazy TNRD reader used only by Analysis. All blocking archive
// work stays on one worker thread; results are returned on the GUI thread.
class AnalysisFileReader : public QObject {
    Q_OBJECT
public:
    explicit AnalysisFileReader(QObject* parent = nullptr);
    ~AnalysisFileReader() override;

    void load(const QString& path);
    void acceptLoaded(uint64_t generation);
    void rejectLoaded(uint64_t generation);
    void requestLapData(int driverIndex, int lapNum, uint32_t rowTypeMask);
    void close();

signals:
    void catalogLoaded(const std::shared_ptr<AnalysisFileCatalog>& catalog);
    void loadFailed(const QString& reason);
    void lapDataReady(uint64_t generation, int driverIndex, int lapNum,
                      uint32_t rowTypeMask,
                      const std::shared_ptr<PlaybackHistoryBatch>& batch);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
