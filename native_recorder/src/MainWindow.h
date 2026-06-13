#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QUdpSocket>

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <limits>

#include <nlohmann/json.hpp>
#include <zlib.h>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onSelectDirectory();
    void onToggleRecording();
    void onSwitchToElectron();
    void onDatagramReady();

private:
    // Widgets
    QLabel*      statusLabel     = nullptr;
    QLabel*      directoryLabel  = nullptr;
    QPushButton* selectDirBtn    = nullptr;
    QPushButton* toggleBtn       = nullptr;
    QPushButton* switchBtn       = nullptr;
    QUdpSocket*  udpSocket       = nullptr;

    // Recording state
    QString      outputDirectory;
    bool         isRecording     = false;
    gzFile       activeGzip      = nullptr;
    int          currentTrackId   = -1;
    int          currentSessionType = -1;
    QString      activeGzipPath;
    float        lastSessionTime  = -1.0f;

    struct BufferEntry { std::string line; float sessionTime; };
    std::vector<BufferEntry> rollingBuffer;
    static constexpr float BUFFER_WINDOW_S = 30.0f;

    std::unordered_map<int, uint32_t>     lastFrameId;
    std::unordered_map<int, uint64_t>     lastSlowMs;
    std::unordered_map<std::string, std::string> dedupeCache;

    // Helpers
    void setStatus(const QString& text);

    void startNewStream(int trackId, int sessionType, int format);
    void closeActiveStream();
    void flushBufferToDisk(const std::vector<BufferEntry>& entries);
    void flushOldBufferEntries();
    void truncateTimeline(float newSessionTime);
    void processPacket(const uint8_t* data, int length);
    void recordRow(const nlohmann::json& row, float sessionTime);
    bool isDuplicate(const std::string& type, const nlohmann::json& row);

    gzFile gzOpenPath(const QString& path, const char* mode);

    static std::string getISOTimestamp();
    static std::string getFilenameTimestamp();
    static std::string sanitizeName(const std::string& name);
};
