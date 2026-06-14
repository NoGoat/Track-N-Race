#include "MainWindow.h"
#include "TelemetryChart.h"
#include "TnrdPlayer.h"

#include <QApplication>
#include <QToolBar>
#include <QStackedWidget>
#include <QComboBox>
#include <QLabel>
#include <QFrame>
#include <QWidget>
#include <QHBoxLayout>
#include <QSlider>
#include <QFileDialog>
#include <QMessageBox>
#include <QSizePolicy>
#include <QStyleHints>
#include <QCoreApplication>
#include <QUdpSocket>
#include <QTimer>
#include <QSvgRenderer>
#include <QPainter>
#include <QImage>
#include <QPixmap>
#include <QResizeEvent>
#include <QProgressBar>

#include <chrono>
#include <ctime>
#include <cstdio>
#include <algorithm>
#include <cctype>

#include "protocols/protocol.h"
#include "protocols/f1_24.h"
#include "protocols/f1_25.h"

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

// ── Damage value helper (used in constructor lambda) ───────────────────────

// Renders an SVG resource and tints it with the given colour.
static QIcon paletteIcon(const QString& resource, const QColor& tint) {
    QSvgRenderer renderer(resource);
    QImage img(24, 24, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    renderer.render(&p);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(img.rect(), tint);
    p.end();
    return QIcon(QPixmap::fromImage(img));
}

static void setDmgValue(QLabel* lbl, int val) {
    if (val < 0) { lbl->setText("—"); lbl->setStyleSheet(""); return; }
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

    outputDirectory = settings.value("outputDirectory").toString();
    wantRecord      = settings.value("autoRecord", false).toBool();

    const QString theme = settings.value("theme", "system").toString();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (theme == "light")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
    else if (theme == "dark")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif

    QToolBar* toolbar = new QToolBar(this);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    addToolBar(Qt::TopToolBarArea, toolbar);

    QComboBox* pageCombo = new QComboBox;
    pageCombo->addItem("Overview");   // index 0
    pageCombo->addItem("Standings");  // index 1
    pageCombo->addItem("Session");    // index 2
    pageCombo->addItem("Tyres");      // index 3
    pageCombo->addItem("Settings");   // index 4
    toolbar->addWidget(pageCombo);

    QWidget* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QAction* spacerAct = toolbar->addWidget(spacer);

    QComboBox* windowCombo = new QComboBox;
    const struct { const char* label; float secs; } windows[] = {
        {"15s", 15}, {"30s", 30}, {"1m", 60},
        {"2m", 120}, {"5m", 300}, {"10m", 600}
    };
    for (int i = 0; i < 6; ++i)
        windowCombo->addItem(windows[i].label, windows[i].secs);
    windowCombo->setCurrentIndex(1);
    toolbar->addWidget(windowCombo);

    QStackedWidget* stack = new QStackedWidget(this);
    stack->addWidget(buildOverviewTab());   // index 0
    stack->addWidget(buildStandingsPage()); // index 1
    stack->addWidget(buildSessionPage());   // index 2
    stack->addWidget(buildTyresPage());     // index 3
    stack->addWidget(buildSettingsTab());   // index 4

    // Coalesces panel rebuilds to ~30 Hz so bursts of packets can't lock the UI.
    uiRefreshTimer_ = new QTimer(this);
    uiRefreshTimer_->setSingleShot(true);
    uiRefreshTimer_->setInterval(33);
    connect(uiRefreshTimer_, &QTimer::timeout, this, &MainWindow::flushUiRefresh);

    // ── Bottom playback bar ────────────────────────────────────────────────────
    player_ = new TnrdPlayer(this);

    pb_bar_ = new QWidget(this);
    pb_bar_->setAutoFillBackground(true);
    {
        QPalette pal = pb_bar_->palette();
        pal.setColor(QPalette::Window, palette().color(QPalette::Window));
        pb_bar_->setPalette(pal);
    }
    pb_bar_->setFixedHeight(48);
    auto* pbLayout = new QHBoxLayout(pb_bar_);
    pbLayout->setContentsMargins(12, 0, 12, 0);
    pbLayout->setSpacing(10);

    const QColor iconTint = palette().color(QPalette::Text);
    pb_playBtn_ = new QPushButton(pb_bar_);
    pb_playBtn_->setIcon(paletteIcon(":/play.svg", iconTint));
    pb_playBtn_->setIconSize(QSize(20, 20));
    pb_playBtn_->setFixedSize(34, 34);
    pb_playBtn_->setFlat(true);
    pbLayout->addWidget(pb_playBtn_);

    pb_slider_ = new QSlider(Qt::Horizontal, pb_bar_);
    pb_slider_->setRange(0, 1000);
    pb_slider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pbLayout->addWidget(pb_slider_);

    pb_timeLabel_ = new QLabel("0:00 / 0:00", pb_bar_);
    pbLayout->addWidget(pb_timeLabel_);

    pb_speedCombo_ = new QComboBox(pb_bar_);
    pb_speedCombo_->addItem("0.25×", 0.25f);
    pb_speedCombo_->addItem("0.5×",  0.5f);
    pb_speedCombo_->addItem("1×",    1.0f);
    pb_speedCombo_->addItem("2×",    2.0f);
    pb_speedCombo_->addItem("4×",    4.0f);
    pb_speedCombo_->setCurrentIndex(2);
    pbLayout->addWidget(pb_speedCombo_);

    auto* closeRecBtn = new QPushButton("✕ Close", pb_bar_);
    pbLayout->addWidget(closeRecBtn);

    pb_bar_->hide();

    pb_sep_ = new QFrame(this);
    pb_sep_->setFrameShape(QFrame::HLine);
    pb_sep_->setFrameShadow(QFrame::Sunken);
    pb_sep_->hide();

    // Stack + separator + playback bar stacked vertically as the central widget
    container_ = new QWidget(this);
    auto* vbox = new QVBoxLayout(container_);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);
    vbox->addWidget(stack);
    vbox->addWidget(pb_sep_);
    vbox->addWidget(pb_bar_);
    setCentralWidget(container_);

    // Loading overlay (shown over the entire central area while decompressing/indexing)
    loadingOverlay_ = new QWidget(container_);
    loadingOverlay_->setAutoFillBackground(true);
    {
        QPalette pal = loadingOverlay_->palette();
        pal.setColor(QPalette::Window, palette().color(QPalette::Window));
        loadingOverlay_->setPalette(pal);
    }
    auto* ol = new QVBoxLayout(loadingOverlay_);
    ol->setAlignment(Qt::AlignCenter);
    ol->setSpacing(12);
    auto* loadingLabel = new QLabel("Loading recording…", loadingOverlay_);
    loadingLabel->setAlignment(Qt::AlignCenter);
    ol->addWidget(loadingLabel);
    auto* spinner = new QProgressBar(loadingOverlay_);
    spinner->setRange(0, 0);
    spinner->setFixedWidth(300);
    spinner->setTextVisible(false);
    ol->addWidget(spinner);
    loadingOverlay_->hide();

    connect(pageCombo, &QComboBox::currentIndexChanged, stack, &QStackedWidget::setCurrentIndex);
    connect(windowCombo, &QComboBox::currentIndexChanged, this, [this, windowCombo](int idx) {
        float secs = windowCombo->itemData(idx).toFloat();
        if (chart) chart->setWindowSeconds(secs);
    });

    // Open Recording stays in the toolbar
    auto* openBtn = new QPushButton("Open Recording", this);
    toolbar->insertSeparator(spacerAct);
    toolbar->insertWidget(spacerAct, openBtn);

    // Helper for formatting session time as M:SS
    auto fmtTime = [](float s) -> QString {
        int m   = (int)s / 60;
        int sec = (int)s % 60;
        return QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
    };

    connect(openBtn, &QPushButton::clicked, this, [this] {
        QString path = QFileDialog::getOpenFileName(
            this, "Open Recording", outputDirectory,
            "TNRD Recordings (*.tnrd *.trnd)");
        if (!path.isEmpty()) player_->load(path);
    });

    connect(player_, &TnrdPlayer::loadingStarted, this, [this] {
        loadingOverlay_->setGeometry(container_->rect());
        loadingOverlay_->raise();
        loadingOverlay_->show();
    });

    connect(player_, &TnrdPlayer::loadFailed, this, [this] {
        loadingOverlay_->hide();
        QMessageBox::warning(this, "Load Failed", "Could not open the recording file.");
    });

    connect(player_, &TnrdPlayer::loaded, this, [this](const nlohmann::json& hdr) {
        loadingOverlay_->hide();
        inPlayback_ = true;
        if (chart) chart->reset();
        lastErs_ = 0.0f;
        closeActiveStream();
        pb_sep_->show();
        pb_bar_->show();
        pb_playBtn_->setIcon(paletteIcon(":/play.svg", palette().color(QPalette::Text)));
        pb_slider_->setValue(0);
        pb_speedCombo_->setCurrentIndex(2); // reset to 1×
        player_->setSpeed(1.0f);
        QString trackName = QString::fromStdString(hdr.value("track_name", "Unknown"));
        QString sessName  = QString::fromStdString(hdr.value("session_name", "Unknown"));
        setWindowTitle(QString("Track N Race — %1 %2 [Playback]").arg(trackName, sessName));
    });

    connect(player_, &TnrdPlayer::packetReady, this, [this](const nlohmann::json& j) {
        emitLiveData(j);
        // Chart update: telemetry uses lastErs_ tracked across status packets
        const std::string type = j.value("type", std::string{});
        if (type == "status")
            lastErs_ = j.value("ers_pct", 0.0f);
        else if (type == "telemetry" && chart)
            chart->addPoint(j.value("session_time", 0.0f),
                            j.value("speed_kph", 0.0f),
                            j.value("rpm", 0),
                            lastErs_);
    });

    connect(player_, &TnrdPlayer::seeked, this, [this] {
        // Drop the live chart's history so it refills from the seek point's window.
        if (chart) chart->reset();
        lastErs_ = 0.0f;
    });

    connect(player_, &TnrdPlayer::chartHistory, this,
            [this](const QVector<ChartSample>& samples) {
        if (!chart) return;
        // Bulk refill: hand each series its full point list in one replace().
        QList<QPointF> speed, rpm, ers;
        speed.reserve(samples.size());
        rpm.reserve(samples.size());
        ers.reserve(samples.size());
        float latest = 0.0f;
        for (const ChartSample& s : samples) {
            speed.append({ s.t, s.speed });
            rpm.append({ s.t, s.rpm });
            ers.append({ s.t, s.ers });
            latest = s.t;
        }
        chart->replaceAll(speed, rpm, ers, latest);
    });

    connect(player_, &TnrdPlayer::stateChanged, this,
            [this, fmtTime](bool playing, float cur, float total, float /*speed*/) {
        pb_playBtn_->setIcon(paletteIcon(
            playing ? ":/pause.svg" : ":/play.svg", palette().color(QPalette::Text)));
        if (total > 0.0f) {
            seekerUpdating_ = true;
            pb_slider_->setValue((int)(cur / total * 1000.0f));
            seekerUpdating_ = false;
        }
        pb_timeLabel_->setText(fmtTime(cur) + " / " + fmtTime(total));
    });

    connect(player_, &TnrdPlayer::finished, this, [this] {
        pb_playBtn_->setIcon(paletteIcon(":/play.svg", palette().color(QPalette::Text)));
    });

    connect(pb_playBtn_, &QPushButton::clicked, this, [this] {
        if (player_->isPlaying()) player_->pause();
        else player_->play();
    });

    connect(pb_slider_, &QSlider::valueChanged, this, [this](int val) {
        if (!seekerUpdating_)
            player_->seek(val / 1000.0f);
    });

    connect(pb_speedCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        player_->setSpeed(pb_speedCombo_->itemData(idx).toFloat());
    });

    connect(closeRecBtn, &QPushButton::clicked, this, [this] {
        player_->close();
        inPlayback_ = false;
        pb_sep_->hide();
        pb_bar_->hide();
        setWindowTitle("Track N Race Background Recorder");
    });

    udpSocket = new QUdpSocket(this);
    bool bound = udpSocket->bind(QHostAddress::AnyIPv4, 20777,
                                  QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound)
        QMessageBox::critical(this, "UDP Error",
            "Failed to bind to UDP port 20777.\n"
            "Is another telemetry tool or Track-N-Race already open?");

    connect(udpSocket, &QUdpSocket::readyRead, this, &MainWindow::onDatagramReady);

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
            this, [this](float ersPct, int, float, float, int, int) {
        cardErs->setText(QString::number((int)ersPct));
    });

    connect(this, &MainWindow::lapUpdated,
            this, [this](int pos, int) {
        cardPos->setText("P" + QString::number(pos));
    });

    connect(this, &MainWindow::telemetryUpdated,
            chart, [](float, int, int, float, float, bool, int) {
        // chart is updated directly from processPacket
    });

    connect(this, &MainWindow::damageUpdated, this,
        [this](int tfl, int tfr, int trl, int trr,
               int bfl, int bfr, int brl, int brr,
               int wfl, int wfr, int wr,
               int fl, int sp, int diff, int gb, int eng) {
            setDmgValue(dmgTyreFl,   tfl); setDmgValue(dmgTyreFr,   tfr);
            setDmgValue(dmgTyreRl,   trl); setDmgValue(dmgTyreRr,   trr);
            setDmgValue(dmgBrakeFl,  bfl); setDmgValue(dmgBrakeFr,  bfr);
            setDmgValue(dmgBrakeRl,  brl); setDmgValue(dmgBrakeRr,  brr);
            setDmgValue(dmgWingFl,   wfl); setDmgValue(dmgWingFr,   wfr);
            setDmgValue(dmgWingRear,  wr); setDmgValue(dmgFloor,     fl);
            setDmgValue(dmgSidepod,   sp); setDmgValue(dmgDiffuser, diff);
            setDmgValue(dmgGearbox,   gb); setDmgValue(dmgEngine,   eng);
        });
}

MainWindow::~MainWindow() {
    closeActiveStream();
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    if (loadingOverlay_ && loadingOverlay_->isVisible() && container_)
        loadingOverlay_->setGeometry(container_->rect());
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
    if (inPlayback_) {
        while (udpSocket->hasPendingDatagrams())
            udpSocket->readDatagram(nullptr, 0);
        return;
    }
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
    float cutoff = lastSessionTime - BUFFER_WINDOW_S;
    size_t flush = 0;
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
    std::string line = row.dump() + "\n";
    float entryTime  = (sessionTime >= 0.0f) ? sessionTime : lastSessionTime;
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
        lastPlayerTelemetryData = row;
        dirtyTyres_ = true; scheduleUiRefresh();
    } else if (type == "status") {
        emit statusUpdated(
            row["ers_pct"].get<float>(),
            row["ers_mode"].get<int>(),
            row["fuel_kg"].get<float>(),
            row["fuel_laps"].get<float>(),
            row["tyre_compound"].get<int>(),
            row["tyre_age_laps"].get<int>()
        );
        lastPlayerStatusData = row;
        dirtyRacePanel_ = true; scheduleUiRefresh();
    } else if (type == "damage") {
        emit damageUpdated(
            row.value("tyre_dmg_fl",   0), row.value("tyre_dmg_fr",   0),
            row.value("tyre_dmg_rl",   0), row.value("tyre_dmg_rr",   0),
            row.value("brake_dmg_fl",  0), row.value("brake_dmg_fr",  0),
            row.value("brake_dmg_rl",  0), row.value("brake_dmg_rr",  0),
            row.value("wing_fl",           0), row.value("wing_fr",           0),
            row.value("wing_rear",         0), row.value("floor_damage",      0),
            row.value("sidepod_damage",    0), row.value("diffuser_damage",   0),
            row.value("gearbox_damage",    0), row.value("engine_damage",     0)
        );
        lastPlayerDamageData = row;
        dirtyTyres_ = true; scheduleUiRefresh();
    } else if (type == "tyre_sets") {
        lastTyreSetsData = row;
        dirtyTyreSets_ = true; scheduleUiRefresh();
    } else if (type == "lap") {
        emit lapUpdated(row["position"].get<int>(), row["lap_num"].get<int>());
        lastPlayerLapData = row;
        dirtyRacePanel_ = true; scheduleUiRefresh();
    } else if (type == "positions") {
        lastPositionsData = row;
        dirtyTrackMap_ = true; scheduleUiRefresh();
    } else if (type == "session") {
        lastSessionData = row;
        dirtySession_ = true; dirtyTrackMap_ = true; scheduleUiRefresh();
    } else if (type == "race_event") {
        if (row.value("code", "") == "SSTA") sessionEventLog.clear();
        sessionEventLog.push_back(row);
        dirtyEvents_ = true; scheduleUiRefresh();
    } else if (type == "timing") {
        lastTimingData = row;
        dirtyTiming_ = true; dirtyProximity_ = true; scheduleUiRefresh();
    } else if (type == "participants") {
        lastParticipantsData = row;
        dirtyTiming_ = true; dirtyTrackMap_ = true; scheduleUiRefresh();
    } else if (type == "all_status") {
        lastAllStatusData = row;
        dirtyTiming_ = true; scheduleUiRefresh();
    }
}

void MainWindow::scheduleUiRefresh() {
    if (!uiRefreshTimer_->isActive()) uiRefreshTimer_->start();
}

void MainWindow::flushUiRefresh() {
    if (dirtyTiming_)    { updateTimingTable();     dirtyTiming_    = false; }
    if (dirtyProximity_) { updateProximityWidget(); dirtyProximity_ = false; }
    if (dirtyRacePanel_) { updateRacePanel();       dirtyRacePanel_ = false; }
    if (dirtyTyres_)     { updateTyresPage();        dirtyTyres_     = false; }
    if (dirtyTyreSets_)  { updateTyreSetsTable();    dirtyTyreSets_  = false; }
    if (dirtySession_)   { updateSessionPage();      dirtySession_   = false; }
    if (dirtyEvents_)    { updateSessionEvents();    dirtyEvents_    = false; }
    if (dirtyTrackMap_)  { updateTrackMapPage();     dirtyTrackMap_  = false; }
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

    if (activeGzip && lastSessionTime >= 0.0f && hdr.sessionTime < lastSessionTime - 0.2f)
        truncateTimeline(hdr.sessionTime);
    else if (hdr.sessionTime > lastSessionTime)
        lastSessionTime = hdr.sessionTime;

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

    for (const auto& row : rows) {
        recordRow(row, hdr.sessionTime);
        emitLiveData(row);

        const std::string rtype = row["type"].get<std::string>();
        if (rtype == "status")
            lastErs_ = row["ers_pct"].get<float>();
        else if (rtype == "telemetry" && chart)
            chart->addPoint(hdr.sessionTime,
                            row["speed_kph"].get<float>(),
                            row["rpm"].get<int>(),
                            lastErs_);
    }
}
