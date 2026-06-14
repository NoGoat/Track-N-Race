#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QRadioButton>
#include <QTableWidget>
#include <QUdpSocket>
#include <QSettings>

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <limits>

#include <nlohmann/json.hpp>
#include <zlib.h>

class TelemetryChart;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    void telemetryUpdated(float speed, int rpm, int gear,
                          float throttle, float brake, bool drs, int engineTemp);
    void statusUpdated(float ersPct, int ersMode, float fuelKg,
                       float fuelLaps, int tyreCompound, int tyreAgeLaps);
    void damageUpdated(int tyreFl, int tyreFr, int tyreRl, int tyreRr,
                       int brakeFl, int brakeFr, int brakeRl, int brakeRr,
                       int wingFl, int wingFr, int wingRear,
                       int floor, int sidepod, int diffuser, int gearbox, int engine);
    void lapUpdated(int position, int lapNum);

private slots:
    void onBrowseDirectory();
    void onAutoRecordToggled(bool checked);
    void onThemeChanged();
    void onDatagramReady();

private:
    // ── Overview tab ──────────────────────────────────────────────
    QLabel*         cardSpeed    = nullptr;
    QLabel*         cardRpm      = nullptr;
    QLabel*         cardGear     = nullptr;
    QLabel*         cardThrottle = nullptr;
    QLabel*         cardBrake    = nullptr;
    QLabel*         cardDrs      = nullptr;
    QLabel*         cardErs      = nullptr;
    QLabel*         cardPos      = nullptr;
    TelemetryChart* chart        = nullptr;
    QLabel*         dmgTyreFl   = nullptr; QLabel* dmgTyreFr  = nullptr;
    QLabel*         dmgTyreRl   = nullptr; QLabel* dmgTyreRr  = nullptr;
    QLabel*         dmgBrakeFl  = nullptr; QLabel* dmgBrakeFr = nullptr;
    QLabel*         dmgBrakeRl  = nullptr; QLabel* dmgBrakeRr = nullptr;
    QLabel*         dmgWingFl   = nullptr; QLabel* dmgWingFr  = nullptr;
    QLabel*         dmgWingRear = nullptr; QLabel* dmgFloor   = nullptr;
    QLabel*         dmgSidepod  = nullptr; QLabel* dmgDiffuser = nullptr;
    QLabel*         dmgGearbox  = nullptr; QLabel* dmgEngine   = nullptr;

    // ── Standings page ────────────────────────────────────────────
    QTableWidget*    timingTable         = nullptr;
    nlohmann::json   lastTimingData;
    nlohmann::json   lastParticipantsData;
    nlohmann::json   lastAllStatusData;

    // ── Settings page ─────────────────────────────────────────────
    QLabel*       dirLabel    = nullptr;
    QCheckBox*    recordCheck = nullptr;
    QRadioButton* themeSystem = nullptr;
    QRadioButton* themeLight  = nullptr;
    QRadioButton* themeDark   = nullptr;

    // ── UDP / network ─────────────────────────────────────────────
    QUdpSocket* udpSocket = nullptr;

    // ── Persistence ───────────────────────────────────────────────
    QSettings settings{ "TrackNRace", "NativeRecorder" };

    // ── Recording state ───────────────────────────────────────────
    bool    wantRecord        = false;
    QString outputDirectory;
    gzFile  activeGzip        = nullptr;
    int     currentTrackId    = -1;
    int     currentSessionType = -1;
    QString activeGzipPath;
    float   lastSessionTime   = -1.0f;

    struct BufferEntry { std::string line; float sessionTime; };
    std::vector<BufferEntry> rollingBuffer;
    static constexpr float BUFFER_WINDOW_S = 30.0f;

    std::unordered_map<int, uint32_t>            lastFrameId;
    std::unordered_map<int, uint64_t>            lastSlowMs;
    std::unordered_map<std::string, std::string> dedupeCache;

    // ── Builders ──────────────────────────────────────────────────
    QWidget* buildOverviewTab();
    QWidget* buildStandingsPage();
    QWidget* buildSettingsTab();
    void     updateTimingTable();

    // ── Recording helpers ─────────────────────────────────────────
    void startNewStream(int trackId, int sessionType, int format);
    void closeActiveStream();
    void flushBufferToDisk(const std::vector<BufferEntry>& entries);
    void flushOldBufferEntries();
    void truncateTimeline(float newSessionTime);
    void processPacket(const uint8_t* data, int length);
    void recordRow(const nlohmann::json& row, float sessionTime);
    void emitLiveData(const nlohmann::json& row);
    bool isDuplicate(const std::string& type, const nlohmann::json& row);

    gzFile gzOpenPath(const QString& path, const char* mode);

    static std::string getISOTimestamp();
    static std::string getFilenameTimestamp();
    static std::string sanitizeName(const std::string& name);
};
