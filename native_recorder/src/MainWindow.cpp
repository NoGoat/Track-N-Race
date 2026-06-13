#include "MainWindow.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QCoreApplication>

#include <chrono>
#include <ctime>
#include <cstdio>
#include <algorithm>
#include <cctype>

#include "protocol.h"
#include "f1_24.h"
#include "f1_25.h"

// Packet IDs
static constexpr int PID_MOTION           = 0;
static constexpr int PID_SESSION          = 1;
static constexpr int PID_LAP_DATA         = 2;
static constexpr int PID_EVENT            = 3;
static constexpr int PID_PARTICIPANTS     = 4;
static constexpr int PID_CAR_TEL         = 6;
static constexpr int PID_CAR_STATUS      = 7;
static constexpr int PID_CAR_DAMAGE      = 10;
static constexpr int PID_MOTION_EX       = 13;

static constexpr int HEADER_SIZE = 29;

static const std::unordered_set<int> FRAME_SAMPLED = {
    PID_MOTION, PID_CAR_TEL, PID_MOTION_EX
};

static const std::unordered_map<int, int> SLOW_RATE_MS = {
    { PID_SESSION,      0    },
    { PID_LAP_DATA,     500  },
    { PID_CAR_STATUS,   500  },
    { PID_CAR_DAMAGE,   500  },
    { PID_PARTICIPANTS, 5000 },
    { PID_EVENT,        0    }
};

static const std::unordered_set<std::string> DEDUPE_TYPES = {
    "session", "tyre_sets", "participants", "all_status", "status", "timing", "damage"
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Track N Race Background Recorder");
    setFixedSize(500, 260);

    QWidget* central = new QWidget(this);
    setCentralWidget(central);
    QVBoxLayout* vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(20, 20, 20, 20);
    vbox->setSpacing(10);

    statusLabel = new QLabel("Status: Idle", this);
    QFont boldFont = statusLabel->font();
    boldFont.setPointSize(11);
    boldFont.setBold(true);
    statusLabel->setFont(boldFont);

    directoryLabel = new QLabel("No directory selected.", this);
    directoryLabel->setWordWrap(true);

    selectDirBtn = new QPushButton("Select Directory",          this);
    toggleBtn    = new QPushButton("Start Recording",           this);
    switchBtn    = new QPushButton("Switch to Track N Race App", this);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);
    btnRow->addWidget(selectDirBtn);
    btnRow->addWidget(toggleBtn);

    vbox->addWidget(statusLabel);
    vbox->addWidget(directoryLabel);
    vbox->addSpacing(5);
    vbox->addLayout(btnRow);
    vbox->addWidget(switchBtn);

    connect(selectDirBtn, &QPushButton::clicked, this, &MainWindow::onSelectDirectory);
    connect(toggleBtn,    &QPushButton::clicked, this, &MainWindow::onToggleRecording);
    connect(switchBtn,    &QPushButton::clicked, this, &MainWindow::onSwitchToElectron);

    udpSocket = new QUdpSocket(this);
    bool bound = udpSocket->bind(QHostAddress::AnyIPv4, 20777,
                                  QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound) {
        QMessageBox::critical(this, "UDP Error",
            "Failed to bind to UDP port 20777.\n"
            "Is another telemetry tool or Track-N-Race already open?");
    }
    connect(udpSocket, &QUdpSocket::readyRead, this, &MainWindow::onDatagramReady);
}

MainWindow::~MainWindow() {
    closeActiveStream();
}

void MainWindow::setStatus(const QString& text) {
    if (statusLabel) statusLabel->setText(text);
}

// ---------------------------------------------------------------------------
// Slot: Select Directory
// ---------------------------------------------------------------------------

void MainWindow::onSelectDirectory() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory", outputDirectory);
    if (!dir.isEmpty()) {
        outputDirectory = dir;
        directoryLabel->setText(dir);
    }
}

// ---------------------------------------------------------------------------
// Slot: Toggle Recording
// ---------------------------------------------------------------------------

void MainWindow::onToggleRecording() {
    if (outputDirectory.isEmpty()) {
        QMessageBox::critical(this, "Error", "Please select an output directory first.");
        return;
    }
    isRecording = !isRecording;
    if (isRecording) {
        toggleBtn->setText("Stop Recording");
        setStatus("Status: Waiting for game data...");
    } else {
        closeActiveStream();
        toggleBtn->setText("Start Recording");
        setStatus("Status: Idle");
    }
}

// ---------------------------------------------------------------------------
// Slot: Switch to Electron app
// ---------------------------------------------------------------------------

void MainWindow::onSwitchToElectron() {
    QString appDir = QCoreApplication::applicationDirPath();

#ifdef _WIN32
    QString electronExe = appDir + "/Track N Race.exe";
#else
    QString electronExe = appDir + "/Track N Race";
#endif

    if (QFile::exists(electronExe)) {
        QProcess::startDetached(electronExe, {});
    } else {
        // Dev mode: package.json is 3 directories up from native_recorder/build/<config>
        QDir dir(appDir);
        for (int i = 0; i < 3; i++) dir.cdUp();
        if (QFile::exists(dir.filePath("package.json"))) {
            QProcess::startDetached("npm", {"run", "dev"}, dir.absolutePath());
        } else {
            QMessageBox::critical(this, "Error", "Track N Race executable not found.");
            return;
        }
    }
    QApplication::quit();
}

// ---------------------------------------------------------------------------
// Slot: UDP datagram received
// ---------------------------------------------------------------------------

void MainWindow::onDatagramReady() {
    while (udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize((int)udpSocket->pendingDatagramSize());
        udpSocket->readDatagram(datagram.data(), datagram.size());
        if (isRecording) {
            processPacket(reinterpret_cast<const uint8_t*>(datagram.constData()), datagram.size());
        }
    }
}

// ---------------------------------------------------------------------------
// Timestamp helpers
// ---------------------------------------------------------------------------

std::string MainWindow::getISOTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    time_t t = system_clock::to_time_t(now);
    struct tm tmInfo {};
#ifdef _WIN32
    gmtime_s(&tmInfo, &t);
#else
    gmtime_r(&t, &tmInfo);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmInfo);
    char result[80];
    snprintf(result, sizeof(result), "%s.%03dZ", buf, (int)ms.count());
    return std::string(result);
}

std::string MainWindow::getFilenameTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    time_t t = system_clock::to_time_t(now);
    struct tm tmInfo {};
#ifdef _WIN32
    gmtime_s(&tmInfo, &t);
#else
    gmtime_r(&t, &tmInfo);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H-%M-%S", &tmInfo);
    char result[80];
    snprintf(result, sizeof(result), "%s-%03dZ", buf, (int)ms.count());
    return std::string(result);
}

std::string MainWindow::sanitizeName(const std::string& name) {
    std::string result;
    for (unsigned char c : name)
        result += std::isalnum(c) ? (char)std::tolower(c) : '_';
    return result;
}

// ---------------------------------------------------------------------------
// gzopen abstraction (wide-char on Windows, UTF-8 on Linux/macOS)
// ---------------------------------------------------------------------------

gzFile MainWindow::gzOpenPath(const QString& path, const char* mode) {
#ifdef _WIN32
    return gzopen_w(path.toStdWString().c_str(), mode);
#else
    return gzopen(path.toUtf8().constData(), mode);
#endif
}

// ---------------------------------------------------------------------------
// Stream lifecycle
// ---------------------------------------------------------------------------

void MainWindow::closeActiveStream() {
    if (activeGzip) {
        flushBufferToDisk(rollingBuffer);
        gzclose(activeGzip);
        activeGzip = nullptr;
        setStatus("Status: Idle");
    }
    rollingBuffer.clear();
    currentTrackId     = -1;
    currentSessionType = -1;
    activeGzipPath.clear();
    lastSessionTime = -1.0f;
    dedupeCache.clear();
}

void MainWindow::startNewStream(int trackId, int sessionType, int format) {
    closeActiveStream();
    if (!isRecording || outputDirectory.isEmpty()) return;

    std::string protocolStr = (format == 2024) ? "f1_24" : "f1_25";

    auto itTrack = TRACK_NAMES.find(trackId);
    std::string trackName = (itTrack != TRACK_NAMES.end())
        ? sanitizeName(itTrack->second)
        : "track_" + std::to_string(trackId);

    auto itSess = SESSION_NAMES.find(sessionType);
    std::string sessionName = (itSess != SESSION_NAMES.end())
        ? sanitizeName(itSess->second)
        : "session_" + std::to_string(sessionType);

    std::string filename = protocolStr + "_" + std::to_string(trackId) + "_"
                         + trackName + "_" + sessionName + "_"
                         + getFilenameTimestamp() + ".tnrd";

    activeGzipPath = outputDirectory + "/" + QString::fromStdString(filename);
    activeGzip     = gzOpenPath(activeGzipPath, "wb");

    if (activeGzip) {
        nlohmann::json header;
        header["magic"]        = "TNRD_V1";
        header["protocol"]     = format;
        header["track_id"]     = trackId;
        header["track_name"]   = (itTrack != TRACK_NAMES.end()) ? itTrack->second : "Unknown";
        header["session_type"] = sessionType;
        header["session_name"] = (itSess != SESSION_NAMES.end()) ? itSess->second : "Unknown";
        header["start_time"]   = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string headerLine = header.dump() + "\n";
        gzwrite(activeGzip, headerLine.c_str(), (unsigned int)headerLine.size());

        currentTrackId     = trackId;
        currentSessionType = sessionType;
        lastSessionTime    = -1.0f;

        QString shortName = QString::fromStdString(filename);
        if (shortName.length() > 30) shortName = shortName.left(30) + "...";
        setStatus("Recording: " + shortName);
    }
}

// ---------------------------------------------------------------------------
// Buffer management
// ---------------------------------------------------------------------------

void MainWindow::flushBufferToDisk(const std::vector<BufferEntry>& entries) {
    if (!activeGzip || entries.empty()) return;
    for (const auto& entry : entries)
        gzwrite(activeGzip, entry.line.c_str(), (unsigned int)entry.line.size());
}

void MainWindow::flushOldBufferEntries() {
    if (lastSessionTime < 0.0f || rollingBuffer.empty()) return;
    float cutoff   = lastSessionTime - BUFFER_WINDOW_S;
    size_t flushIdx = 0;
    while (flushIdx < rollingBuffer.size() && rollingBuffer[flushIdx].sessionTime < cutoff)
        flushIdx++;
    if (flushIdx > 0) {
        flushBufferToDisk({rollingBuffer.begin(), rollingBuffer.begin() + (ptrdiff_t)flushIdx});
        rollingBuffer.erase(rollingBuffer.begin(), rollingBuffer.begin() + (ptrdiff_t)flushIdx);
    }
}

// ---------------------------------------------------------------------------
// Deduplication
// ---------------------------------------------------------------------------

bool MainWindow::isDuplicate(const std::string& type, const nlohmann::json& row) {
    if (!DEDUPE_TYPES.count(type)) return false;
    nlohmann::json clone = row;
    clone.erase("ts");
    clone.erase("session_time");
    std::string hash = clone.dump();
    auto it = dedupeCache.find(type);
    if (it != dedupeCache.end() && it->second == hash) return true;
    dedupeCache[type] = hash;
    return false;
}

// ---------------------------------------------------------------------------
// Flashback / rewind truncation
// ---------------------------------------------------------------------------

void MainWindow::truncateTimeline(float newSessionTime) {
    float bufferStart = rollingBuffer.empty()
        ? std::numeric_limits<float>::infinity()
        : rollingBuffer[0].sessionTime;

    if (newSessionTime >= bufferStart) {
        // Rewind is within the in-memory buffer — trim only
        rollingBuffer.erase(
            std::remove_if(rollingBuffer.begin(), rollingBuffer.end(),
                [newSessionTime](const BufferEntry& e) { return e.sessionTime > newSessionTime; }),
            rollingBuffer.end()
        );
    } else {
        // Rewind goes past the buffer — rewrite the on-disk gzip file
        rollingBuffer.clear();
        if (!activeGzipPath.isEmpty() && activeGzip) {
            gzclose(activeGzip);
            activeGzip = nullptr;

            std::vector<std::string> retainedLines;
            gzFile infile = gzOpenPath(activeGzipPath, "rb");
            if (infile) {
                char buf[16384];
                while (gzgets(infile, buf, sizeof(buf)) != nullptr) {
                    std::string line(buf);
                    try {
                        nlohmann::json j = nlohmann::json::parse(line);
                        if (j.contains("session_time")) {
                            if (j["session_time"].get<float>() <= newSessionTime)
                                retainedLines.push_back(line);
                        } else {
                            retainedLines.push_back(line);
                        }
                    } catch (...) {}
                }
                gzclose(infile);
            }

            gzFile outfile = gzOpenPath(activeGzipPath, "wb");
            if (outfile) {
                for (const auto& line : retainedLines)
                    gzwrite(outfile, line.c_str(), (unsigned int)line.size());
                gzclose(outfile);
            }

            activeGzip = gzOpenPath(activeGzipPath, "ab");
        }
    }

    dedupeCache.clear();
    lastSessionTime = newSessionTime;
    setStatus(QString("Flashback: Resuming from %1s...").arg((int)newSessionTime));
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

void MainWindow::recordRow(const nlohmann::json& row, float sessionTime) {
    if (!activeGzip) return;
    std::string type = row["type"];
    if (isDuplicate(type, row)) return;

    std::string line      = row.dump() + "\n";
    float       entryTime = (sessionTime >= 0.0f) ? sessionTime : lastSessionTime;
    rollingBuffer.push_back({line, entryTime});

    if (type == "race_event" && row["code"] == "SEND") {
        closeActiveStream();
        return;
    }
    flushOldBufferEntries();
}

// ---------------------------------------------------------------------------
// Central packet router
// ---------------------------------------------------------------------------

void MainWindow::processPacket(const uint8_t* data, int length) {
    if (length < HEADER_SIZE) return;

    uint16_t format = ReadUInt16(data, 0);
    if (format != 2024 && format != 2025) return;

    PacketHeader hdr;
    hdr.packetFormat   = format;
    hdr.packetId       = data[6];
    hdr.sessionTime    = ReadFloat(data, 15);
    hdr.overallFrameId = ReadUInt32(data, 23);
    hdr.playerCarIndex = data[27];

    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Rate limiting
    if (FRAME_SAMPLED.count(hdr.packetId)) {
        auto it = lastFrameId.find(hdr.packetId);
        if (it != lastFrameId.end() && it->second == hdr.overallFrameId) return;
        lastFrameId[hdr.packetId] = hdr.overallFrameId;
    } else {
        int rateMs = 500;
        auto itLimit = SLOW_RATE_MS.find(hdr.packetId);
        if (itLimit != SLOW_RATE_MS.end()) rateMs = itLimit->second;
        if (rateMs > 0) {
            auto itTime = lastSlowMs.find(hdr.packetId);
            if (itTime != lastSlowMs.end() && (nowMs - itTime->second) < (uint64_t)rateMs) return;
            lastSlowMs[hdr.packetId] = nowMs;
        }
    }

    // Flashback detection
    if (activeGzip && lastSessionTime >= 0.0f && hdr.sessionTime < lastSessionTime - 0.2f) {
        truncateTimeline(hdr.sessionTime);
    } else if (hdr.sessionTime > lastSessionTime) {
        lastSessionTime = hdr.sessionTime;
    }

    // Auto-start stream on new session
    if (hdr.packetId == PID_SESSION && length >= 708) {
        int8_t  trackId     = ReadInt8(data, 36);
        uint8_t sessionType = data[35];
        if (trackId != currentTrackId || sessionType != currentSessionType || !activeGzip)
            startNewStream(trackId, sessionType, format);
    }

    if (!activeGzip) return;

    std::vector<nlohmann::json> rows;
    std::string timestamp = getISOTimestamp();

    if (format == 2024)
        rows = F1_24::ParsePacket(data, length, hdr, timestamp);
    else
        rows = F1_25::ParsePacket(data, length, hdr, timestamp);

    for (const auto& row : rows)
        recordRow(row, hdr.sessionTime);
}
