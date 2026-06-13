#include "MainWindow.h"
#include "TelemetryChart.h"

#include <QApplication>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QButtonGroup>
#include <QFrame>
#include <QWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QStyleHints>
#include <QCoreApplication>
#include <QSizePolicy>

#include <chrono>
#include <ctime>
#include <cstdio>
#include <algorithm>
#include <cctype>

#include "protocol.h"
#include "f1_24.h"
#include "f1_25.h"

// ── Packet IDs ─────────────────────────────────────────────────────────────

static constexpr int PID_MOTION       = 0;
static constexpr int PID_SESSION      = 1;
static constexpr int PID_LAP_DATA     = 2;
static constexpr int PID_EVENT        = 3;
static constexpr int PID_PARTICIPANTS = 4;
static constexpr int PID_CAR_TEL     = 6;
static constexpr int PID_CAR_STATUS  = 7;
static constexpr int PID_CAR_DAMAGE  = 10;
static constexpr int PID_MOTION_EX   = 13;

static constexpr int HEADER_SIZE = 29;

static const std::unordered_set<int> FRAME_SAMPLED = {
    PID_MOTION, PID_CAR_TEL, PID_MOTION_EX
};
static const std::unordered_map<int, int> SLOW_RATE_MS = {
    { PID_SESSION, 0 }, { PID_LAP_DATA, 500 }, { PID_CAR_STATUS, 500 },
    { PID_CAR_DAMAGE, 500 }, { PID_PARTICIPANTS, 5000 }, { PID_EVENT, 0 }
};
static const std::unordered_set<std::string> DEDUPE_TYPES = {
    "session", "tyre_sets", "participants", "all_status", "status", "timing", "damage"
};

// ── UI helpers ─────────────────────────────────────────────────────────────

static QFrame* vsep() {
    QFrame* f = new QFrame;
    f->setFrameShape(QFrame::VLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

// Creates a stat card (label / bold-value / unit). Returns card frame; sets valueOut.
static QFrame* makeStatCard(const QString& label, const QString& unit, QLabel*& valueOut) {
    QFrame* card = new QFrame;
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QVBoxLayout* cv = new QVBoxLayout(card);
    cv->setContentsMargins(8, 6, 8, 6);
    cv->setSpacing(1);

    QLabel* lbl = new QLabel(label.toUpper());
    QFont lf; lf.setPointSize(7);
    lbl->setFont(lf);
    lbl->setForegroundRole(QPalette::PlaceholderText);

    valueOut = new QLabel("—"); // em dash
    QFont vf; vf.setPointSize(15); vf.setBold(true);
    valueOut->setFont(vf);

    QLabel* ulbl = new QLabel(unit);
    QFont uf; uf.setPointSize(7);
    ulbl->setFont(uf);
    ulbl->setForegroundRole(QPalette::PlaceholderText);

    cv->addWidget(lbl);
    cv->addWidget(valueOut);
    cv->addWidget(ulbl);
    return card;
}

// Creates a damage card. Returns card frame; sets valueOut.
static QFrame* makeDmgCard(const QString& label, QLabel*& valueOut) {
    QFrame* card = new QFrame;
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QVBoxLayout* cv = new QVBoxLayout(card);
    cv->setContentsMargins(6, 4, 6, 4);
    cv->setSpacing(0);

    QLabel* lbl = new QLabel(label.toUpper());
    QFont lf; lf.setPointSize(6);
    lbl->setFont(lf);
    lbl->setForegroundRole(QPalette::PlaceholderText);

    valueOut = new QLabel("—");
    QFont vf; vf.setPointSize(12); vf.setBold(true);
    valueOut->setFont(vf);

    cv->addWidget(lbl);
    cv->addWidget(valueOut);
    return card;
}

// Sets damage value label with colour coding (0 = green, >0 = red, <0 = dash).
static void setDmgValue(QLabel* lbl, int val) {
    if (val < 0) {
        lbl->setText("—");
        lbl->setStyleSheet("");
        return;
    }
    lbl->setText(QString::number(val));
    lbl->setStyleSheet(val == 0 ? "color: #37872D;" : "color: #C4162A;");
}

// ── Construction ───────────────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Track N Race Background Recorder");
    setMinimumSize(720, 520);
    resize(780, 580);

    // Restore persisted settings
    outputDirectory = settings.value("outputDirectory").toString();
    wantRecord      = settings.value("autoRecord", false).toBool();

    // Apply persisted theme
    const QString theme = settings.value("theme", "system").toString();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (theme == "light")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
    else if (theme == "dark")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    // "system" → no call needed (Unknown is default)
#endif

    // Build tabbed UI
    QTabWidget* tabs = new QTabWidget(this);
    tabs->addTab(buildOverviewTab(), "Overview");
    tabs->addTab(buildSettingsTab(), "Settings");
    setCentralWidget(tabs);

    // Bind UDP socket
    udpSocket = new QUdpSocket(this);
    bool bound = udpSocket->bind(QHostAddress::AnyIPv4, 20777,
                                  QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound)
        QMessageBox::critical(this, "UDP Error",
            "Failed to bind to UDP port 20777.\n"
            "Is another telemetry tool or Track-N-Race already open?");

    connect(udpSocket, &QUdpSocket::readyRead, this, &MainWindow::onDatagramReady);

    // Wire live signals to Overview widgets
    connect(this, &MainWindow::telemetryUpdated,
            this, [this](float speed, int rpm, int gear,
                         float throttle, float brake, bool drs, int /*eng*/) {
        cardSpeed->setText(QString::number((int)speed));
        cardRpm->setText(QString::number(rpm / 1000.0, 'f', 1) + "k");
        cardGear->setText(gear <= 0 ? "N" : QString::number(gear));
        cardThrottle->setText(QString::number((int)(throttle * 100)));
        cardBrake->setText(QString::number((int)(brake * 100)));
        cardDrs->setText(drs ? "ON" : "OFF");
        cardDrs->setStyleSheet(drs ? "color: #37872D; font-weight: bold;"
                                   : "color: gray; font-weight: bold;");
    });

    connect(this, &MainWindow::statusUpdated,
            this, [this](float ersPct, int /*ersMode*/, float /*fuelKg*/,
                         float /*fuelLaps*/, int /*tyre*/, int /*age*/) {
        cardErs->setText(QString::number((int)ersPct));
    });

    connect(this, &MainWindow::lapUpdated,
            this, [this](int pos, int /*lap*/) {
        cardPos->setText("P" + QString::number(pos));
    });

    connect(this, &MainWindow::telemetryUpdated,
            chart, [this](float speed, int rpm, int /*gear*/,
                          float /*thr*/, float /*brk*/, bool /*drs*/, int /*eng*/) {
        // ERS is updated separately; chart holds the last ers value via statusUpdated
        // We store last ers in a lambda capture. Use a member or separate slot.
        // Simple approach: emit chart update from processPacket directly.
        (void)speed; (void)rpm; // chart updated from processPacket
    });

    connect(this, &MainWindow::damageUpdated, this,
        [this](int tfl, int tfr, int trl, int trr,
               int bfl, int bfr, int brl, int brr,
               int wfl, int wfr, int wr,
               int fl, int sp, int diff, int gb, int eng) {
            setDmgValue(dmgTyreFl,  tfl); setDmgValue(dmgTyreFr,  tfr);
            setDmgValue(dmgTyreRl,  trl); setDmgValue(dmgTyreRr,  trr);
            setDmgValue(dmgBrakeFl, bfl); setDmgValue(dmgBrakeFr, bfr);
            setDmgValue(dmgBrakeRl, brl); setDmgValue(dmgBrakeRr, brr);
            setDmgValue(dmgWingFl,  wfl); setDmgValue(dmgWingFr,  wfr);
            setDmgValue(dmgWingRear, wr); setDmgValue(dmgFloor,   fl);
            setDmgValue(dmgSidepod, sp);  setDmgValue(dmgDiffuser, diff);
            setDmgValue(dmgGearbox, gb);  setDmgValue(dmgEngine,  eng);
        });
}

MainWindow::~MainWindow() {
    closeActiveStream();
}

// ── Tab builders ───────────────────────────────────────────────────────────

QWidget* MainWindow::buildOverviewTab() {
    QWidget* w = new QWidget;
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(8, 8, 8, 8);
    vbox->setSpacing(6);

    // ── Stats row ────────────────────────────────────────────────
    QFrame* statsFrame = new QFrame;
    statsFrame->setFrameShape(QFrame::StyledPanel);
    statsFrame->setFixedHeight(80);
    QHBoxLayout* sh = new QHBoxLayout(statsFrame);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->setSpacing(0);

    sh->addWidget(makeStatCard("Speed",    "kph", cardSpeed));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("RPM",      "",    cardRpm));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("Gear",     "",    cardGear));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("Throttle", "%",   cardThrottle));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("Brake",    "%",   cardBrake));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("DRS",      "",    cardDrs));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("ERS",      "%",   cardErs));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("Pos",      "",    cardPos));

    vbox->addWidget(statsFrame);

    // ── Chart ────────────────────────────────────────────────────
    chart = new TelemetryChart;
    vbox->addWidget(chart, 1);

    // ── Damage rows ──────────────────────────────────────────────
    QFrame* dmgFrame = new QFrame;
    dmgFrame->setFrameShape(QFrame::StyledPanel);
    QVBoxLayout* dv = new QVBoxLayout(dmgFrame);
    dv->setContentsMargins(0, 0, 0, 0);
    dv->setSpacing(0);

    // Row A: Tyre × 4 + Brake × 4
    QFrame* rowA = new QFrame;
    rowA->setFixedHeight(60);
    QHBoxLayout* ah = new QHBoxLayout(rowA);
    ah->setContentsMargins(0, 0, 0, 0);
    ah->setSpacing(0);
    ah->addWidget(makeDmgCard("Tyre FL",  dmgTyreFl));   ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Tyre FR",  dmgTyreFr));   ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Tyre RL",  dmgTyreRl));   ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Tyre RR",  dmgTyreRr));   ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Brake FL", dmgBrakeFl));  ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Brake FR", dmgBrakeFr));  ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Brake RL", dmgBrakeRl));  ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Brake RR", dmgBrakeRr));

    // Horizontal divider
    QFrame* hdiv = new QFrame;
    hdiv->setFrameShape(QFrame::HLine);
    hdiv->setFrameShadow(QFrame::Sunken);

    // Row B: Wings × 3 + Floor + Diffuser + Gearbox + Engine
    QFrame* rowB = new QFrame;
    rowB->setFixedHeight(60);
    QHBoxLayout* bh = new QHBoxLayout(rowB);
    bh->setContentsMargins(0, 0, 0, 0);
    bh->setSpacing(0);
    bh->addWidget(makeDmgCard("Wing FL",   dmgWingFl));  bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Wing FR",   dmgWingFr));  bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Wing Rear", dmgWingRear));bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Floor",     dmgFloor));   bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Sidepod",   dmgSidepod)); bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Diffuser",  dmgDiffuser));bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Gearbox",   dmgGearbox)); bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Engine",    dmgEngine));

    dv->addWidget(rowA);
    dv->addWidget(hdiv);
    dv->addWidget(rowB);

    vbox->addWidget(dmgFrame);
    return w;
}

QWidget* MainWindow::buildSettingsTab() {
    QWidget* w = new QWidget;
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(16, 16, 16, 16);
    vbox->setSpacing(12);

    // ── Recording group ──────────────────────────────────────────
    QGroupBox* recGroup = new QGroupBox("Recording");
    QVBoxLayout* rv = new QVBoxLayout(recGroup);
    rv->setSpacing(8);

    recordCheck = new QCheckBox("Auto-record when a session starts");
    recordCheck->setChecked(wantRecord);
    rv->addWidget(recordCheck);

    QHBoxLayout* dirRow = new QHBoxLayout;
    dirLabel = new QLabel(outputDirectory.isEmpty() ? "No directory selected." : outputDirectory);
    dirLabel->setWordWrap(true);
    dirLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    QPushButton* browseBtn = new QPushButton("Browse…");
    browseBtn->setFixedWidth(90);
    dirRow->addWidget(dirLabel);
    dirRow->addWidget(browseBtn);
    rv->addLayout(dirRow);

    vbox->addWidget(recGroup);

    // ── Appearance group ─────────────────────────────────────────
    QGroupBox* appGroup = new QGroupBox("Appearance");
    QVBoxLayout* av = new QVBoxLayout(appGroup);
    av->setSpacing(6);

    themeSystem = new QRadioButton("System default");
    themeLight  = new QRadioButton("Light");
    themeDark   = new QRadioButton("Dark");

    const QString theme = settings.value("theme", "system").toString();
    if (theme == "light")       themeLight->setChecked(true);
    else if (theme == "dark")   themeDark->setChecked(true);
    else                        themeSystem->setChecked(true);

    QButtonGroup* bg = new QButtonGroup(this);
    bg->addButton(themeSystem);
    bg->addButton(themeLight);
    bg->addButton(themeDark);

    av->addWidget(themeSystem);
    av->addWidget(themeLight);
    av->addWidget(themeDark);

    vbox->addWidget(appGroup);
    vbox->addStretch();

    // ── Connections ──────────────────────────────────────────────
    connect(browseBtn,    &QPushButton::clicked,        this, &MainWindow::onBrowseDirectory);
    connect(recordCheck,  &QCheckBox::toggled,          this, &MainWindow::onAutoRecordToggled);
    connect(themeSystem,  &QRadioButton::toggled,       this, [this](bool) { onThemeChanged(); });
    connect(themeLight,   &QRadioButton::toggled,       this, [this](bool) { onThemeChanged(); });
    connect(themeDark,    &QRadioButton::toggled,       this, [this](bool) { onThemeChanged(); });

    return w;
}

// ── Slots ──────────────────────────────────────────────────────────────────

void MainWindow::onBrowseDirectory() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory", outputDirectory);
    if (!dir.isEmpty()) {
        outputDirectory = dir;
        dirLabel->setText(dir);
        settings.setValue("outputDirectory", dir);
    }
}

void MainWindow::onAutoRecordToggled(bool checked) {
    wantRecord = checked;
    settings.setValue("autoRecord", checked);
    if (!checked) closeActiveStream();
}

void MainWindow::onThemeChanged() {
    QString themeVal = "system";
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (themeLight->isChecked()) {
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
        themeVal = "light";
    } else if (themeDark->isChecked()) {
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
        themeVal = "dark";
    } else {
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Unknown);
    }
#endif
    settings.setValue("theme", themeVal);
}

void MainWindow::onDatagramReady() {
    while (udpSocket->hasPendingDatagrams()) {
        QByteArray dg;
        dg.resize((int)udpSocket->pendingDatagramSize());
        udpSocket->readDatagram(dg.data(), dg.size());
        processPacket(reinterpret_cast<const uint8_t*>(dg.constData()), dg.size());
    }
}

// ── Timestamps ─────────────────────────────────────────────────────────────

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
    char buf[64]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmInfo);
    char result[80]; snprintf(result, sizeof(result), "%s.%03dZ", buf, (int)ms.count());
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
    char buf[64]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H-%M-%S", &tmInfo);
    char result[80]; snprintf(result, sizeof(result), "%s-%03dZ", buf, (int)ms.count());
    return std::string(result);
}

std::string MainWindow::sanitizeName(const std::string& name) {
    std::string r;
    for (unsigned char c : name) r += std::isalnum(c) ? (char)std::tolower(c) : '_';
    return r;
}

// ── gzopen abstraction ─────────────────────────────────────────────────────

gzFile MainWindow::gzOpenPath(const QString& path, const char* mode) {
#ifdef _WIN32
    return gzopen_w(path.toStdWString().c_str(), mode);
#else
    return gzopen(path.toUtf8().constData(), mode);
#endif
}

// ── Stream lifecycle ───────────────────────────────────────────────────────

void MainWindow::closeActiveStream() {
    if (activeGzip) {
        flushBufferToDisk(rollingBuffer);
        gzclose(activeGzip);
        activeGzip = nullptr;
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
    if (!wantRecord || outputDirectory.isEmpty()) return;

    std::string proto = (format == 2024) ? "f1_24" : "f1_25";

    auto itTrack = TRACK_NAMES.find(trackId);
    std::string tName = (itTrack != TRACK_NAMES.end())
        ? sanitizeName(itTrack->second) : "track_" + std::to_string(trackId);

    auto itSess = SESSION_NAMES.find(sessionType);
    std::string sName = (itSess != SESSION_NAMES.end())
        ? sanitizeName(itSess->second) : "session_" + std::to_string(sessionType);

    std::string filename = proto + "_" + std::to_string(trackId) + "_"
                         + tName + "_" + sName + "_" + getFilenameTimestamp() + ".tnrd";

    activeGzipPath = outputDirectory + "/" + QString::fromStdString(filename);
    activeGzip     = gzOpenPath(activeGzipPath, "wb");

    if (activeGzip) {
        nlohmann::json hdr;
        hdr["magic"]        = "TNRD_V1";
        hdr["protocol"]     = format;
        hdr["track_id"]     = trackId;
        hdr["track_name"]   = (itTrack != TRACK_NAMES.end()) ? itTrack->second : "Unknown";
        hdr["session_type"] = sessionType;
        hdr["session_name"] = (itSess != SESSION_NAMES.end()) ? itSess->second : "Unknown";
        hdr["start_time"]   = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string hl = hdr.dump() + "\n";
        gzwrite(activeGzip, hl.c_str(), (unsigned int)hl.size());

        currentTrackId     = trackId;
        currentSessionType = sessionType;
        lastSessionTime    = -1.0f;
    }
}

// ── Buffer ─────────────────────────────────────────────────────────────────

void MainWindow::flushBufferToDisk(const std::vector<BufferEntry>& entries) {
    if (!activeGzip || entries.empty()) return;
    for (const auto& e : entries)
        gzwrite(activeGzip, e.line.c_str(), (unsigned int)e.line.size());
}

void MainWindow::flushOldBufferEntries() {
    if (lastSessionTime < 0.0f || rollingBuffer.empty()) return;
    float cutoff  = lastSessionTime - BUFFER_WINDOW_S;
    size_t flush  = 0;
    while (flush < rollingBuffer.size() && rollingBuffer[flush].sessionTime < cutoff)
        flush++;
    if (flush > 0) {
        flushBufferToDisk({rollingBuffer.begin(), rollingBuffer.begin() + (ptrdiff_t)flush});
        rollingBuffer.erase(rollingBuffer.begin(), rollingBuffer.begin() + (ptrdiff_t)flush);
    }
}

// ── Deduplication ──────────────────────────────────────────────────────────

bool MainWindow::isDuplicate(const std::string& type, const nlohmann::json& row) {
    if (!DEDUPE_TYPES.count(type)) return false;
    nlohmann::json clone = row;
    clone.erase("ts"); clone.erase("session_time");
    std::string hash = clone.dump();
    auto it = dedupeCache.find(type);
    if (it != dedupeCache.end() && it->second == hash) return true;
    dedupeCache[type] = hash;
    return false;
}

// ── Flashback / rewind ─────────────────────────────────────────────────────

void MainWindow::truncateTimeline(float newSessionTime) {
    float bufStart = rollingBuffer.empty()
        ? std::numeric_limits<float>::infinity() : rollingBuffer[0].sessionTime;

    if (newSessionTime >= bufStart) {
        rollingBuffer.erase(
            std::remove_if(rollingBuffer.begin(), rollingBuffer.end(),
                [newSessionTime](const BufferEntry& e) { return e.sessionTime > newSessionTime; }),
            rollingBuffer.end());
    } else {
        rollingBuffer.clear();
        if (!activeGzipPath.isEmpty() && activeGzip) {
            gzclose(activeGzip); activeGzip = nullptr;
            std::vector<std::string> kept;
            gzFile in = gzOpenPath(activeGzipPath, "rb");
            if (in) {
                char buf[16384];
                while (gzgets(in, buf, sizeof(buf)) != nullptr) {
                    std::string line(buf);
                    try {
                        nlohmann::json j = nlohmann::json::parse(line);
                        if (!j.contains("session_time") || j["session_time"].get<float>() <= newSessionTime)
                            kept.push_back(line);
                    } catch (...) {}
                }
                gzclose(in);
            }
            gzFile out = gzOpenPath(activeGzipPath, "wb");
            if (out) {
                for (const auto& l : kept) gzwrite(out, l.c_str(), (unsigned int)l.size());
                gzclose(out);
            }
            activeGzip = gzOpenPath(activeGzipPath, "ab");
        }
    }
    dedupeCache.clear();
    lastSessionTime = newSessionTime;
}

// ── Record a row to the rolling buffer ─────────────────────────────────────

void MainWindow::recordRow(const nlohmann::json& row, float sessionTime) {
    if (!activeGzip) return;
    std::string type = row["type"];
    if (isDuplicate(type, row)) return;
    std::string line  = row.dump() + "\n";
    float entryTime   = (sessionTime >= 0.0f) ? sessionTime : lastSessionTime;
    rollingBuffer.push_back({line, entryTime});
    if (type == "race_event" && row["code"] == "SEND") { closeActiveStream(); return; }
    flushOldBufferEntries();
}

// ── Live data extraction → signals ─────────────────────────────────────────

void MainWindow::emitLiveData(const nlohmann::json& row) {
    const std::string type = row["type"].get<std::string>();
    if (type == "telemetry") {
        emit telemetryUpdated(
            row["speed_kph"].get<float>(),
            row["rpm"].get<int>(),
            row["gear"].get<int>(),
            row["throttle"].get<float>(),
            row["brake"].get<float>(),
            row.value("drs", 0) != 0,
            row.value("engine_temp", 0)
        );
        // Also push to chart (ERS driven separately by statusUpdated connection)
    } else if (type == "status") {
        float ersPct = row["ers_pct"].get<float>();
        emit statusUpdated(
            ersPct,
            row["ers_mode"].get<int>(),
            row["fuel_kg"].get<float>(),
            row["fuel_laps"].get<float>(),
            row["tyre_compound"].get<int>(),
            row["tyre_age_laps"].get<int>()
        );
    } else if (type == "damage") {
        emit damageUpdated(
            row.value("tyre_dmg_fl",  0),
            row.value("tyre_dmg_fr",  0),
            row.value("tyre_dmg_rl",  0),
            row.value("tyre_dmg_rr",  0),
            row.value("brake_dmg_fl", 0),
            row.value("brake_dmg_fr", 0),
            row.value("brake_dmg_rl", 0),
            row.value("brake_dmg_rr", 0),
            row.value("wing_fl",          0),
            row.value("wing_fr",          0),
            row.value("wing_rear",        0),
            row.value("floor_damage",     0),
            row.value("sidepod_damage",   0),
            row.value("diffuser_damage",  0),
            row.value("gearbox_damage",   0),
            row.value("engine_damage",    0)
        );
    } else if (type == "lap") {
        emit lapUpdated(row["position"].get<int>(), row["lap_num"].get<int>());
    }
}

// ── Central packet router ──────────────────────────────────────────────────

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
        auto itL = SLOW_RATE_MS.find(hdr.packetId);
        if (itL != SLOW_RATE_MS.end()) rateMs = itL->second;
        if (rateMs > 0) {
            auto itT = lastSlowMs.find(hdr.packetId);
            if (itT != lastSlowMs.end() && (nowMs - itT->second) < (uint64_t)rateMs) return;
            lastSlowMs[hdr.packetId] = nowMs;
        }
    }

    // Flashback detection (only when recording)
    if (activeGzip && lastSessionTime >= 0.0f && hdr.sessionTime < lastSessionTime - 0.2f)
        truncateTimeline(hdr.sessionTime);
    else if (hdr.sessionTime > lastSessionTime)
        lastSessionTime = hdr.sessionTime;

    // Auto-start stream on new session
    if (hdr.packetId == PID_SESSION && length >= 708) {
        int8_t  trackId     = ReadInt8(data, 36);
        uint8_t sessionType = data[35];
        if (wantRecord && (trackId != currentTrackId || sessionType != currentSessionType || !activeGzip))
            startNewStream(trackId, sessionType, format);
    }

    std::vector<nlohmann::json> rows;
    std::string ts = getISOTimestamp();
    if (format == 2024) rows = F1_24::ParsePacket(data, length, hdr, ts);
    else                rows = F1_25::ParsePacket(data, length, hdr, ts);

    // Track last ERS for chart (needed to pair with telemetry points)
    static float lastErs = 0.0f;

    for (const auto& row : rows) {
        recordRow(row, hdr.sessionTime);
        emitLiveData(row);

        // Update chart: pair telemetry speed/rpm with latest ERS
        const std::string rtype = row["type"].get<std::string>();
        if (rtype == "status")
            lastErs = row["ers_pct"].get<float>();
        else if (rtype == "telemetry" && chart)
            chart->addPoint(hdr.sessionTime,
                            row["speed_kph"].get<float>(),
                            row["rpm"].get<int>(),
                            lastErs);
    }
}
