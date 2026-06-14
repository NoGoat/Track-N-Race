#pragma once

#include <QObject>
#include <QTimer>
#include <QFile>
#include <QElapsedTimer>
#include <QString>
#include <vector>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <zlib.h>

class TnrdPlayer : public QObject {
    Q_OBJECT

public:
    explicit TnrdPlayer(QObject* parent = nullptr);
    ~TnrdPlayer() override;

    bool load(const QString& path);
    void play();
    void pause();
    void close();
    void seek(float pct);
    void setSpeed(float mult);

    float totalTime()   const { return totalTime_; }
    float currentTime() const { return currentTime_; }
    bool  isPlaying()   const { return playing_; }

signals:
    void loaded(nlohmann::json header);
    void packetReady(nlohmann::json packet);
    void stateChanged(bool playing, float currentTime, float totalTime, float speed);
    void finished();

private slots:
    void tick();

private:
    struct IndexEntry {
        qint64  offset;
        float   sessionTime;
        uint8_t type;
    };

    std::vector<IndexEntry>                           index_;
    // Per sparse-type: sorted list of positions in index_ for seek reconstruction
    std::unordered_map<uint8_t, std::vector<size_t>> sparseIdx_;

    float        totalTime_   = 0.0f;
    float        currentTime_ = 0.0f;
    float        speed_       = 1.0f;
    bool         playing_     = false;
    size_t       playPos_     = 0;

    QString      tempPath_;
    QFile        tempFile_;
    QTimer*      timer_  = nullptr;
    QElapsedTimer elapsed_;

    static uint8_t typeId(const std::string& s);
    static bool    isSparse(uint8_t id);

    bool           decompress(const QString& srcPath);
    void           buildIndex();
    nlohmann::json readLineAt(qint64 offset);
    void           emitState();
    void           cleanup();
};
