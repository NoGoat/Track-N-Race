#include "MainWindow.h"
#include "TelemetryChart.h"

#include <QApplication>
#include <QToolBar>
#include <QStackedWidget>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QProgressBar>
#include <QScrollArea>
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

    // Toolbar with page selector (left) and time-window selector (right)
    QToolBar* toolbar = new QToolBar(this);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    addToolBar(Qt::TopToolBarArea, toolbar);

    QComboBox* pageCombo = new QComboBox;
    pageCombo->addItem("Overview");   // index 0
    pageCombo->addItem("Standings");  // index 1
    pageCombo->addItem("Tyres");      // index 2
    pageCombo->addItem("Settings");   // index 3
    toolbar->addWidget(pageCombo);

    QWidget* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    QComboBox* windowCombo = new QComboBox;
    const struct { const char* label; float secs; } windows[] = {
        {"15s", 15}, {"30s", 30}, {"1m", 60},
        {"2m", 120}, {"5m", 300}, {"10m", 600}
    };
    for (int i = 0; i < 6; ++i)
        windowCombo->addItem(windows[i].label, windows[i].secs);
    windowCombo->setCurrentIndex(1); // 30s default
    toolbar->addWidget(windowCombo);

    // Stacked content area
    QStackedWidget* stack = new QStackedWidget(this);
    stack->addWidget(buildOverviewTab());   // index 0
    stack->addWidget(buildStandingsPage()); // index 1
    stack->addWidget(buildTyresPage());     // index 2
    stack->addWidget(buildSettingsTab());   // index 3
    setCentralWidget(stack);

    connect(pageCombo, &QComboBox::currentIndexChanged, stack, &QStackedWidget::setCurrentIndex);
    connect(windowCombo, &QComboBox::currentIndexChanged, this, [this, windowCombo](int idx) {
        float secs = windowCombo->itemData(idx).toFloat();
        if (chart) chart->setWindowSeconds(secs);
    });

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

// ── Shared tyre compound helpers ──────────────────────────────────────────

static QString tyreLabel(int compound) {
    switch (compound) {
        case 22: return "C6";
        case 16: return "C5";
        case 17: return "C4";
        case 18: return "C3";
        case 19: return "C2";
        case 20: return "C1";
        case 21: return "C0";
        case  7: return "INT";
        case  8: return "WET";
        default: return "—";
    }
}

// Text color keyed on visual_compound (16=soft, 17=medium, 18=hard, 7=int, 8=wet)
// NOT tyre_compound — visual_compound is fixed; tyre_compound varies by weekend
// Returns invalid QColor for hard → caller leaves text at the OS default color
static QColor tyreTextColor(int visualCompound) {
    switch (visualCompound) {
        case 16: return QColor("#e8002d"); // Soft   — red
        case 17: return QColor("#ffd700"); // Medium — yellow
        case 18: return {};                // Hard   — OS default
        case  7: return QColor("#39b54a"); // INT    — green
        case  8: return QColor("#4488ff"); // WET    — blue
        default: return {};
    }
}

// ── Tyres page color helpers ───────────────────────────────────────────────

static QColor tyreTempColor(int c) {
    if (c < 60)  return QColor("#5794F2");
    if (c < 80)  return QColor("#d4ad04");
    if (c <= 110) return QColor("#37872D");
    if (c <= 130) return QColor("#c47d0e");
    return QColor("#C4162A");
}

static QColor brakeTempColor(int c) {
    if (c < 200)  return QColor("#37872D");
    if (c < 400)  return QColor("#d4ad04");
    if (c < 600)  return QColor("#c47d0e");
    return QColor("#C4162A");
}

static QColor wearPctColor(int pct) {
    if (pct < 20) return QColor("#73BF69");
    if (pct < 40) return QColor("#A8D436");
    if (pct < 60) return QColor("#FADE2A");
    if (pct < 80) return QColor("#FF9830");
    return QColor("#C4162A");
}

static QString setStatusText(const nlohmann::json& s) {
    if (s.value("fitted",    false)) return "FITTED";
    if (s.value("available", false)) return s.value("wear", 0) == 0 ? "NEW" : "USED";
    if (s.value("recommended_session", 0) > 0) return "RESERVED";
    return "RETURNED";
}

static QColor setStatusColor(const nlohmann::json& s) {
    const std::string st = setStatusText(s).toStdString();
    if (st == "FITTED")   return QColor("#5794F2");
    if (st == "NEW")      return QColor("#37872D");
    if (st == "USED")     return QColor("#d4ad04");
    if (st == "RESERVED") return QColor("#a78bfa");
    return QColor("#484c62"); // RETURNED
}

// ── Tyres page builder ─────────────────────────────────────────────────────

QWidget* MainWindow::buildTyresPage() {
    QWidget* w = new QWidget;
    QHBoxLayout* hbox = new QHBoxLayout(w);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    // ── Tyre sets table (left) ───────────────────────────────────
    tp_setsTable = new QTableWidget;
    tp_setsTable->setColumnCount(7);
    tp_setsTable->setHorizontalHeaderLabels({"#", "COMPOUND", "STATUS", "WEAR", "LIFE", "SESSION", "DELTA"});
    tp_setsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tp_setsTable->setSelectionMode(QAbstractItemView::NoSelection);
    tp_setsTable->setShowGrid(false);
    tp_setsTable->setAlternatingRowColors(false);
    tp_setsTable->verticalHeader()->setVisible(false);
    tp_setsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tp_setsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    QFont hf; hf.setPointSize(7);
    tp_setsTable->horizontalHeader()->setFont(hf);
    hbox->addWidget(tp_setsTable, 1);

    // Vertical divider
    QFrame* vdiv = new QFrame;
    vdiv->setFrameShape(QFrame::VLine);
    vdiv->setFrameShadow(QFrame::Sunken);
    hbox->addWidget(vdiv);

    // ── Right panel: WheelCards (1×4 vertical, fills height) ────────
    QWidget* right = new QWidget;
    right->setFixedWidth(240);
    QVBoxLayout* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(0);

    static const char* cornerNames[] = { "FRONT LEFT", "FRONT RIGHT", "REAR LEFT", "REAR RIGHT" };

    for (int i = 0; i < 4; ++i) {
        QWidget* card = new QWidget;
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        QVBoxLayout* cv = new QVBoxLayout(card);
        cv->setContentsMargins(10, 8, 10, 8);
        cv->setSpacing(2);

        QLabel* title = new QLabel(cornerNames[i]);
        QFont tf; tf.setPointSize(7); tf.setBold(true);
        title->setFont(tf);
        title->setForegroundRole(QPalette::PlaceholderText);
        cv->addWidget(title);

        auto makeRow = [&](const QString& label, QLabel*& valueOut) {
            QWidget* row = new QWidget;
            QHBoxLayout* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            QLabel* lbl = new QLabel(label);
            QFont lf; lf.setPointSize(8); lbl->setFont(lf);
            lbl->setForegroundRole(QPalette::PlaceholderText);
            valueOut = new QLabel("—");
            QFont vf; vf.setPointSize(8); vf.setBold(true);
            valueOut->setFont(vf);
            valueOut->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            h->addWidget(lbl);
            h->addStretch();
            h->addWidget(valueOut);
            return row;
        };

        cv->addWidget(makeRow("Surface",  tp_surfaceTemp[i]));
        cv->addWidget(makeRow("Inner",    tp_innerTemp[i]));
        cv->addWidget(makeRow("Brake",    tp_brakeTemp[i]));
        cv->addWidget(makeRow("Wear",     tp_wearLabel[i]));

        auto* wearBar = new QProgressBar;
        wearBar->setRange(0, 100);
        wearBar->setValue(0);
        wearBar->setTextVisible(false);
        wearBar->setFixedHeight(6);
        wearBar->setStyleSheet(
            "QProgressBar { border: none; background: palette(mid); border-radius: 3px; }"
            "QProgressBar::chunk { background: #73BF69; border-radius: 3px; }"
        );
        tp_wear[i] = wearBar;
        cv->addWidget(wearBar);

        tp_blisters[i] = new QLabel;
        tp_blisters[i]->setVisible(false);
        QFont bf; bf.setPointSize(7);
        tp_blisters[i]->setFont(bf);
        tp_blisters[i]->setForegroundRole(QPalette::PlaceholderText);
        cv->addWidget(tp_blisters[i]);

        rv->addWidget(card, 1);  // stretch=1 so all four share height equally

        if (i < 3) {
            QFrame* hdiv = new QFrame;
            hdiv->setFrameShape(QFrame::HLine);
            hdiv->setFrameShadow(QFrame::Sunken);
            rv->addWidget(hdiv);
        }
    }

    hbox->addWidget(right);
    return w;
}

// ── Tyres page updater ─────────────────────────────────────────────────────

void MainWindow::updateTyresPage() {
    if (!tp_surfaceTemp[0]) return;

    static const char* surfKeys[] = {
        "tyre_temp_surface_fl", "tyre_temp_surface_fr",
        "tyre_temp_surface_rl", "tyre_temp_surface_rr"
    };
    static const char* innerKeys[] = {
        "tyre_temp_inner_fl", "tyre_temp_inner_fr",
        "tyre_temp_inner_rl", "tyre_temp_inner_rr"
    };
    static const char* brakeKeys[] = {
        "brake_temp_fl", "brake_temp_fr",
        "brake_temp_rl", "brake_temp_rr"
    };
    static const char* wearKeys[] = {
        "tyre_wear_fl", "tyre_wear_fr",
        "tyre_wear_rl", "tyre_wear_rr"
    };
    static const char* blisterKeys[] = {
        "blisters_fl", "blisters_fr",
        "blisters_rl", "blisters_rr"
    };

    for (int i = 0; i < 4; ++i) {
        // Surface temp
        if (!lastPlayerTelemetryData.empty()) {
            int surf = lastPlayerTelemetryData.value(surfKeys[i], -1);
            if (surf >= 0) {
                tp_surfaceTemp[i]->setText(QString::number(surf) + "°C");
                tp_surfaceTemp[i]->setStyleSheet(
                    "color: " + tyreTempColor(surf).name() + "; font-weight: bold;");
            }

            int inner = lastPlayerTelemetryData.value(innerKeys[i], -1);
            if (inner >= 0) {
                tp_innerTemp[i]->setText(QString::number(inner) + "°C");
                tp_innerTemp[i]->setStyleSheet(
                    "color: " + tyreTempColor(inner).name() + "; font-weight: bold;");
            }

            int brk = lastPlayerTelemetryData.value(brakeKeys[i], -1);
            if (brk >= 0) {
                tp_brakeTemp[i]->setText(QString::number(brk) + "°C");
                tp_brakeTemp[i]->setStyleSheet(
                    "color: " + brakeTempColor(brk).name() + "; font-weight: bold;");
            }
        }

        // Wear + blisters
        if (!lastPlayerDamageData.empty()) {
            int wear = lastPlayerDamageData.value(wearKeys[i], -1);
            if (wear >= 0) {
                const QString wearCol = wearPctColor(wear).name();
                tp_wearLabel[i]->setText(QString::number(wear) + "%");
                tp_wearLabel[i]->setStyleSheet("color: " + wearCol + "; font-weight: bold;");
                tp_wear[i]->setValue(wear);
                tp_wear[i]->setStyleSheet(QString(
                    "QProgressBar { border: none; background: palette(mid); border-radius: 3px; }"
                    "QProgressBar::chunk { background: %1; border-radius: 3px; }"
                ).arg(wearCol));
            }

            int blisters = lastPlayerDamageData.value(blisterKeys[i], 0);
            if (blisters > 0) {
                tp_blisters[i]->setText(QString("· %1% blisters").arg(blisters));
                tp_blisters[i]->setVisible(true);
            } else {
                tp_blisters[i]->setVisible(false);
            }
        }
    }
}

// ── Tyre sets table updater ────────────────────────────────────────────────

void MainWindow::updateTyreSetsTable() {
    if (!tp_setsTable || lastTyreSetsData.empty() || !lastTyreSetsData.contains("sets")) return;

    // Collect non-empty sets
    std::vector<nlohmann::json> sets;
    for (const auto& s : lastTyreSetsData["sets"]) {
        if (s.value("actual_compound", 0) != 0)
            sets.push_back(s);
    }

    std::sort(sets.begin(), sets.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("idx", 99) < b.value("idx", 99);
    });

    static const char* sessionLabels[] = {
        "—", "FP1", "FP2", "FP3", "Short P",
        "Q1", "Q2", "Q3", "Short Q", "1-Shot Q",
        "SS1", "SS2", "SS3", "SS Short", "SS 1-Shot",
        "Race", "Race 2", "Race 3", "Time Trial"
    };

    tp_setsTable->setRowCount((int)sets.size());
    for (int row = 0; row < (int)sets.size(); ++row) {
        const auto& s = sets[row];
        int  idx       = s.value("idx", 0);
        int  compound  = s.value("actual_compound",  0);
        int  visual    = s.value("visual_compound",  0);
        int  wear      = s.value("wear",             0);
        int  lifeSpan  = s.value("life_span",        0);
        int  usable    = s.value("usable_life",      0);
        int  recSess   = s.value("recommended_session", 0);
        int  deltaMs   = s.value("lap_delta_ms",     0);

        QString status = setStatusText(s);
        QColor  statusCol = setStatusColor(s);
        QColor  cmpFg  = tyreTextColor(visual);

        auto makeItem = [](const QString& text) {
            return new QTableWidgetItem(text);
        };

        // Col 0: #
        tp_setsTable->setItem(row, 0, makeItem(QString::number(idx + 1)));

        // Col 1: Compound
        auto* cmpItem = makeItem(tyreLabel(compound));
        if (cmpFg.isValid()) cmpItem->setForeground(cmpFg);
        tp_setsTable->setItem(row, 1, cmpItem);

        // Col 2: Status
        auto* stItem = makeItem(status);
        stItem->setForeground(statusCol);
        tp_setsTable->setItem(row, 2, stItem);

        // Col 3: Wear — bar + percentage label
        {
            const QString wc = wearPctColor(wear).name();
            QWidget* cell = new QWidget;
            cell->setStyleSheet("background: transparent;");
            QHBoxLayout* wh = new QHBoxLayout(cell);
            wh->setContentsMargins(4, 0, 4, 0);
            wh->setSpacing(4);

            auto* bar = new QProgressBar;
            bar->setRange(0, 100);
            bar->setValue(wear);
            bar->setTextVisible(false);
            bar->setFixedHeight(6);
            bar->setStyleSheet(QString(
                "QProgressBar { border: none; background: palette(mid); border-radius: 3px; }"
                "QProgressBar::chunk { background: %1; border-radius: 3px; }"
            ).arg(wc));

            auto* wearLbl = new QLabel(QString::number(wear) + "%");
            wearLbl->setStyleSheet("color: " + wc + "; font-weight: bold; background: transparent;");
            QFont wf; wf.setPointSize(8);
            wearLbl->setFont(wf);
            wearLbl->setFixedWidth(36);
            wearLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

            wh->addWidget(bar, 1);
            wh->addWidget(wearLbl);
            tp_setsTable->setCellWidget(row, 3, cell);
        }

        // Col 4: Life
        QString lifeText = (lifeSpan > 0 || usable > 0)
            ? QString("%1/%2L").arg(lifeSpan).arg(usable) : "—";
        tp_setsTable->setItem(row, 4, makeItem(lifeText));

        // Col 5: Recommended session
        int rsIdx = (recSess >= 0 && recSess < 19) ? recSess : 0;
        tp_setsTable->setItem(row, 5, makeItem(sessionLabels[rsIdx]));

        // Col 6: Lap delta
        QString deltaText;
        if (deltaMs != 0) {
            deltaText = QString("%1%2").arg(deltaMs > 0 ? "+" : "").arg(deltaMs / 1000.0, 0, 'f', 3);
        }
        auto* deltaItem = makeItem(deltaText);
        if (deltaMs > 0) deltaItem->setForeground(QColor("#C4162A"));
        else if (deltaMs < 0) deltaItem->setForeground(QColor("#37872D"));
        tp_setsTable->setItem(row, 6, deltaItem);

        tp_setsTable->setRowHeight(row, 22);
    }
}

// ── Standings helpers ──────────────────────────────────────────────────────

static QString formatLapTime(int ms) {
    if (ms <= 0) return "—";
    int min  = ms / 60000;
    int sec  = (ms % 60000) / 1000;
    int msec = ms % 1000;
    return QString("%1:%2.%3")
        .arg(min).arg(sec, 2, 10, QChar('0')).arg(msec, 3, 10, QChar('0'));
}

static QString formatSector(int ms) {
    if (ms <= 0) return "—";
    int sec  = ms / 1000;
    int msec = ms % 1000;
    return QString("%1.%2").arg(sec).arg(msec, 3, 10, QChar('0'));
}

static QString formatGap(int ms, bool isLeader) {
    if (isLeader) return "LEADER";
    if (ms <= 0)  return "—";
    if (ms < 60000)
        return QString("+%1.%2").arg(ms / 1000).arg(ms % 1000, 3, 10, QChar('0'));
    int min  = ms / 60000;
    int sec  = (ms % 60000) / 1000;
    int msec = ms % 1000;
    return QString("+%1:%2.%3")
        .arg(min).arg(sec, 2, 10, QChar('0')).arg(msec, 3, 10, QChar('0'));
}

// ── Standings page builder ─────────────────────────────────────────────────

QWidget* MainWindow::buildStandingsPage() {
    QWidget* w = new QWidget;
    QHBoxLayout* hbox = new QHBoxLayout(w);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    timingTable = new QTableWidget;
    timingTable->setColumnCount(10);
    timingTable->setHorizontalHeaderLabels(
        {"POS", "DRIVER", "LAP", "LAST LAP", "GAP", "S1", "S2", "S3", "TYRE", "STATUS"});
    timingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timingTable->setSelectionMode(QAbstractItemView::NoSelection);
    timingTable->setShowGrid(false);
    timingTable->setAlternatingRowColors(false);
    timingTable->verticalHeader()->setVisible(false);
    timingTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    timingTable->horizontalHeader()->setSectionResizeMode(9, QHeaderView::Stretch);

    QFont hf; hf.setPointSize(7);
    timingTable->horizontalHeader()->setFont(hf);

    connect(timingTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        int clicked = (row >= 0 && row < (int)tableRowCarIdx.size())
                      ? tableRowCarIdx[row] : -1;
        selectedCarIdx = (clicked == selectedCarIdx) ? -1 : clicked; // toggle on re-click
        updateTimingTable();
        updateRacePanel();
    });

    hbox->addWidget(timingTable, 1);

    // Vertical divider
    QFrame* vdiv = new QFrame;
    vdiv->setFrameShape(QFrame::VLine);
    vdiv->setFrameShadow(QFrame::Sunken);
    hbox->addWidget(vdiv);

    hbox->addWidget(buildRacePanel());
    return w;
}

// ── Race panel ────────────────────────────────────────────────────────────

QWidget* MainWindow::buildRacePanel() {
    QScrollArea* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(220);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget* w = new QWidget;
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(8, 8, 8, 8);
    vbox->setSpacing(8);

    // Helper: key / value row inside a group
    auto makeRow = [&](const QString& label, QLabel*& valueOut) -> QWidget* {
        QWidget* row = new QWidget;
        QHBoxLayout* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 2, 0, 2);
        QLabel* lbl = new QLabel(label);
        QFont lf; lf.setPointSize(8); lbl->setFont(lf);
        lbl->setForegroundRole(QPalette::PlaceholderText);
        valueOut = new QLabel("—");
        QFont vf; vf.setPointSize(8); vf.setBold(true); valueOut->setFont(vf);
        h->addWidget(lbl);
        h->addStretch();
        h->addWidget(valueOut);
        return row;
    };

    // ── Driver header ────────────────────────────────────────────
    rp_driverName = new QLabel("—");
    QFont dnF; dnF.setPointSize(10); dnF.setBold(true);
    rp_driverName->setFont(dnF);
    rp_driverName->setAlignment(Qt::AlignCenter);
    vbox->addWidget(rp_driverName);

    // ── TIMING ───────────────────────────────────────────────────
    QGroupBox* timGrp = new QGroupBox("TIMING");
    QVBoxLayout* tv = new QVBoxLayout(timGrp);
    tv->setSpacing(0);
    tv->addWidget(makeRow("Lap",      rp_lapNum));
    tv->addWidget(makeRow("Position", rp_position));
    tv->addWidget(makeRow("Pit",      rp_pitStatus));
    tv->addWidget(makeRow("Current",  rp_currentLap));
    tv->addWidget(makeRow("Last Lap", rp_lastLap));

    // Sector row: S1 | S2
    QWidget* sectRow = new QWidget;
    QHBoxLayout* sh = new QHBoxLayout(sectRow);
    sh->setContentsMargins(0, 4, 0, 0);
    sh->setSpacing(8);
    auto makeSect = [&](const QString& lbl, QLabel*& out) {
        QWidget* sc = new QWidget;
        QVBoxLayout* sv = new QVBoxLayout(sc);
        sv->setContentsMargins(0,0,0,0); sv->setSpacing(1);
        QLabel* l = new QLabel(lbl); QFont lf; lf.setPointSize(7); l->setFont(lf);
        l->setForegroundRole(QPalette::PlaceholderText); l->setAlignment(Qt::AlignCenter);
        out = new QLabel("—"); QFont vf; vf.setPointSize(8); vf.setBold(true);
        out->setFont(vf); out->setAlignment(Qt::AlignCenter);
        sv->addWidget(l); sv->addWidget(out);
        return sc;
    };
    sh->addWidget(makeSect("S1", rp_s1));
    sh->addWidget(makeSect("S2", rp_s2));
    tv->addWidget(sectRow);
    vbox->addWidget(timGrp);

    // ── ENERGY ───────────────────────────────────────────────────
    QGroupBox* ersGrp = new QGroupBox("ENERGY");
    QVBoxLayout* ev = new QVBoxLayout(ersGrp);
    ev->setSpacing(4);

    rp_ersPct = new QLabel("—");
    QFont bigF; bigF.setPointSize(18); bigF.setBold(true);
    rp_ersPct->setFont(bigF);
    rp_ersPct->setAlignment(Qt::AlignCenter);
    ev->addWidget(rp_ersPct);

    rp_ersBar = new QProgressBar;
    rp_ersBar->setRange(0, 100);
    rp_ersBar->setValue(0);
    rp_ersBar->setTextVisible(false);
    rp_ersBar->setFixedHeight(6);
    ev->addWidget(rp_ersBar);

    ev->addWidget(makeRow("Mode", rp_ersMode));
    ev->addWidget(makeRow("DRS",  rp_drs));
    vbox->addWidget(ersGrp);

    // ── STRATEGY ─────────────────────────────────────────────────
    QGroupBox* stratGrp = new QGroupBox("STRATEGY");
    QVBoxLayout* stv = new QVBoxLayout(stratGrp);
    stv->setSpacing(0);
    stv->addWidget(makeRow("Fuel",       rp_fuelKg));
    stv->addWidget(makeRow("Fuel Laps",  rp_fuelLaps));
    stv->addWidget(makeRow("Mix",        rp_fuelMix));
    stv->addWidget(makeRow("Tyre",       rp_tyre));
    stv->addWidget(makeRow("Tyre Age",   rp_tyreAge));
    stv->addWidget(makeRow("Brake Bias", rp_brakeBias));
    vbox->addWidget(stratGrp);

    vbox->addStretch();
    scroll->setWidget(w);
    return scroll;
}

// ── Race panel updater ────────────────────────────────────────────────────

void MainWindow::updateRacePanel() {
    if (!rp_lapNum) return;

    int playerIdx = lastTimingData.empty() ? -1 : lastTimingData.value("player_idx", -1);
    bool viewingOther = (selectedCarIdx != -1 && selectedCarIdx != playerIdx);

    // ── Driver name header ────────────────────────────────────────
    if (viewingOther && !lastParticipantsData.empty() && lastParticipantsData.contains("drivers")) {
        QString name;
        for (const auto& d : lastParticipantsData["drivers"]) {
            if (d.value("idx", -1) == selectedCarIdx) {
                name = QString("#%1 %2")
                    .arg(d.value("race_number", 0))
                    .arg(QString::fromStdString(d.value("name", "")));
                break;
            }
        }
        rp_driverName->setText(name.isEmpty() ? QString("Car %1").arg(selectedCarIdx) : name);
    } else {
        rp_driverName->setText("Your Car");
    }

    // ── TIMING ───────────────────────────────────────────────────
    // For other cars: read from lastTimingData["cars"]
    // For player:     read from lastPlayerLapData (more precise dedicated packet)
    auto applyTiming = [&](const nlohmann::json& lap) {
        int lapNum    = lap.value("lap_num",        0);
        int pos       = lap.value("position",       0);
        int pitSt     = lap.value("pit_status",     0);
        int currentMs = lap.value("current_lap_ms", 0);
        int lastMs    = lap.value("last_lap_ms",    0);
        int s1Ms      = lap.value("s1_ms",          0);
        int s2Ms      = lap.value("s2_ms",          0);
        bool invalid  = lap.value("lap_invalid",    false);
        int  penS     = lap.value("penalties_s",    0);

        rp_lapNum->setText(lapNum > 0 ? QString::number(lapNum) : "—");
        rp_position->setText(pos > 0 ? "P" + QString::number(pos) : "—");

        QStringList flags;
        if (pitSt == 1)       flags << "In pit lane";
        else if (pitSt == 2)  flags << "In pit";
        if (invalid)          flags << "INVALID";
        if (penS > 0)         flags << ("+" + QString::number(penS) + "s");
        rp_pitStatus->setText(flags.isEmpty() ? "—" : flags.join(" · "));
        rp_pitStatus->setStyleSheet(flags.isEmpty() ? "" : "color: #C4162A; font-weight: bold;");

        rp_currentLap->setText(formatLapTime(currentMs));
        rp_lastLap->setText(formatLapTime(lastMs));
        rp_s1->setText(formatSector(s1Ms));
        rp_s2->setText(formatSector(s2Ms));
    };

    if (viewingOther && !lastTimingData.empty() && lastTimingData.contains("cars")) {
        for (const auto& car : lastTimingData["cars"]) {
            if (car.value("idx", -1) == selectedCarIdx) { applyTiming(car); break; }
        }
    } else if (!lastPlayerLapData.empty()) {
        applyTiming(lastPlayerLapData);
    }

    // ── ENERGY + STRATEGY ─────────────────────────────────────────
    // For other cars: read from lastAllStatusData["cars"]
    // For player:     read from lastPlayerStatusData
    auto applyStatus = [&](const nlohmann::json& st) {
        float ersPct    = st.value("ers_pct",          0.0f);
        int   ersMode   = st.value("ers_mode",         0);
        float fuelKg    = st.value("fuel_kg",          0.0f);
        float fuelLaps  = st.value("fuel_laps",        0.0f);
        int   fuelMix   = st.value("fuel_mix",         0);
        int   compound  = st.value("tyre_compound",    -1);
        int   visual    = st.value("visual_compound",  -1);
        int   tyreAge   = st.value("tyre_age_laps",    0);
        float brakeBias = st.value("front_brake_bias", 0.0f);
        bool  drsOk     = st.value("drs_allowed",      false);

        rp_ersPct->setText(QString::number((int)ersPct) + "%");
        rp_ersBar->setValue((int)ersPct);
        const char* ersColor = ersPct > 60 ? "#4488ff" : ersPct > 30 ? "#ffd700" : "#C4162A";
        rp_ersBar->setStyleSheet(
            QString("QProgressBar::chunk { background-color: %1; }").arg(ersColor));

        static const char* ersModes[] = {"None", "Auto", "Hotlap", "Overtake"};
        rp_ersMode->setText(ersMode >= 0 && ersMode < 4 ? ersModes[ersMode] : "—");

        rp_drs->setText(drsOk ? "AVAILABLE" : "LOCKED");
        rp_drs->setStyleSheet(drsOk ? "color: #37872D; font-weight: bold;" : "");

        rp_fuelKg->setText(QString::number(fuelKg, 'f', 1) + " kg");
        rp_fuelLaps->setText(QString::number(fuelLaps, 'f', 1) + "L");
        const char* fuelColor = fuelLaps > 1.0f ? "#37872D" : fuelLaps >= 0.0f ? "#ffd700" : "#C4162A";
        rp_fuelKg->setStyleSheet(QString("color: %1; font-weight: bold;").arg(fuelColor));

        static const char* mixes[] = {"Lean", "Standard", "Rich", "Max Power"};
        rp_fuelMix->setText(fuelMix >= 0 && fuelMix < 4 ? mixes[fuelMix] : "—");

        rp_tyre->setText(tyreLabel(compound));
        QColor tyreFg = tyreTextColor(visual);
        rp_tyre->setStyleSheet(tyreFg.isValid()
            ? QString("color: %1; font-weight: bold;").arg(tyreFg.name())
            : "font-weight: bold;");
        rp_tyreAge->setText(tyreAge > 0 ? QString::number(tyreAge) + "L" : "—");
        rp_brakeBias->setText(brakeBias > 0 ? QString::number(brakeBias, 'f', 1) + "% front" : "—");
    };

    if (viewingOther && !lastAllStatusData.empty() && lastAllStatusData.contains("cars")) {
        for (const auto& car : lastAllStatusData["cars"]) {
            if (car.value("idx", -1) == selectedCarIdx) { applyStatus(car); break; }
        }
    } else if (!lastPlayerStatusData.empty()) {
        applyStatus(lastPlayerStatusData);
    }
}

// ── Standings updater ──────────────────────────────────────────────────────

void MainWindow::updateTimingTable() {
    if (!timingTable || lastTimingData.empty()) return;

    // Build participant lookup: idx → {name, race_number, livery_color}
    struct DriverInfo { QString name; int raceNum; QColor color; };
    std::unordered_map<int, DriverInfo> driverMap;
    if (!lastParticipantsData.empty() && lastParticipantsData.contains("drivers")) {
        for (const auto& d : lastParticipantsData["drivers"]) {
            int idx = d.value("idx", -1);
            if (idx < 0) continue;
            QString name = QString::fromStdString(d.value("name", ""));
            int raceNum  = d.value("race_number", 0);
            QColor color = QColor(QString::fromStdString(d.value("livery_color", "#8e8e8e")));
            driverMap[idx] = { name, raceNum, color };
        }
    }

    // Build tyre lookup: idx → {tyre_compound (for label), visual_compound (for color)}
    struct TyreInfo { int compound; int visual; };
    std::unordered_map<int, TyreInfo> tyreMap;
    if (!lastAllStatusData.empty() && lastAllStatusData.contains("cars")) {
        for (const auto& c : lastAllStatusData["cars"]) {
            int idx = c.value("idx", -1);
            if (idx >= 0)
                tyreMap[idx] = { c.value("tyre_compound", -1), c.value("visual_compound", -1) };
        }
    }

    // Collect active cars and sort by position
    int playerIdx = lastTimingData.value("player_idx", -1);
    const auto& cars = lastTimingData["cars"];

    std::vector<nlohmann::json> active;
    for (const auto& car : cars) {
        int rs  = car.value("result_status", 0);
        int pos = car.value("position", 0);
        if (rs >= 2 && pos > 0) active.push_back(car);
    }
    std::sort(active.begin(), active.end(),
        [](const nlohmann::json& a, const nlohmann::json& b) {
            return a.value("position", 99) < b.value("position", 99);
        });

    timingTable->setRowCount((int)active.size());

    // Rebuild row→carIdx map for click handling
    tableRowCarIdx.resize(active.size());
    for (int i = 0; i < (int)active.size(); ++i)
        tableRowCarIdx[i] = active[i].value("idx", -1);

    for (int row = 0; row < (int)active.size(); ++row) {
        const auto& car = active[row];
        int  idx        = car.value("idx", -1);
        int  pos        = car.value("position", 0);
        int  lapNum     = car.value("lap_num", 0);
        int  lastLapMs  = car.value("last_lap_ms", 0);
        int  s1Ms       = car.value("s1_ms", 0);
        int  s2Ms       = car.value("s2_ms", 0);
        int  gapMs      = car.value("gap_ms", 0);
        int  pitStatus  = car.value("pit_status", 0);
        bool lapInvalid = car.value("lap_invalid", false);
        int  penaltiesS = car.value("penalties_s", 0);
        int  numDt      = car.value("num_dt_pens", 0);
        int  numSg      = car.value("num_sg_pens", 0);
        int  resultSt   = car.value("result_status", 0);
        bool isPlayer   = (idx == playerIdx);

        // S3 = last_lap - s1 - s2 (best effort)
        int s3Ms = (lastLapMs > 0 && s1Ms > 0 && s2Ms > 0)
                    ? lastLapMs - s1Ms - s2Ms : 0;

        // Driver info
        auto di = driverMap.find(idx);
        QString driverText = (di != driverMap.end())
            ? QString("#%1 %2").arg(di->second.raceNum).arg(di->second.name)
            : QString("Car %1").arg(idx);
        QColor driverColor = (di != driverMap.end())
            ? di->second.color : QColor("#8e8e8e");

        // Tyre
        int compound = tyreMap.count(idx) ? tyreMap.at(idx).compound : -1;
        int visual   = tyreMap.count(idx) ? tyreMap.at(idx).visual   : -1;

        // Status text
        QString statusText;
        if (resultSt == 4) statusText = "DNF";
        else if (resultSt == 5) statusText = "DSQ";
        else if (resultSt == 7) statusText = "RET";
        else if (pitStatus == 1) statusText = "PITLANE";
        else if (pitStatus == 2) statusText = "IN PIT";
        else if (numDt > 0) statusText = "DT";
        else if (numSg > 0) statusText = "SG";
        else if (penaltiesS > 0) statusText = QString("+%1s").arg(penaltiesS);
        else if (lapInvalid) statusText = "INV";

        // Position color
        QColor posColor;
        if      (pos == 1) posColor = QColor("#FFD700");
        else if (pos == 2) posColor = QColor("#C0C0C0");
        else if (pos == 3) posColor = QColor("#CD7F32");

        // Bold for player's car and for the selected row
        QFont cellFont;
        if (isPlayer || idx == selectedCarIdx) cellFont.setBold(true);

        auto makeItem = [&](const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setFont(cellFont);
            return item;
        };

        // Col 0: POS
        auto* posItem = makeItem(QString::number(pos));
        if (posColor.isValid()) posItem->setForeground(posColor);
        timingTable->setItem(row, 0, posItem);

        // Col 1: DRIVER
        auto* drvItem = makeItem(driverText);
        drvItem->setForeground(driverColor);
        timingTable->setItem(row, 1, drvItem);

        // Col 2: LAP
        timingTable->setItem(row, 2, makeItem(lapNum > 0 ? QString::number(lapNum) : "—"));

        // Col 3: LAST LAP
        timingTable->setItem(row, 3, makeItem(formatLapTime(lastLapMs)));

        // Col 4: GAP
        timingTable->setItem(row, 4, makeItem(formatGap(gapMs, pos == 1)));

        // Col 5: S1
        timingTable->setItem(row, 5, makeItem(formatSector(s1Ms)));

        // Col 6: S2
        timingTable->setItem(row, 6, makeItem(formatSector(s2Ms)));

        // Col 7: S3
        timingTable->setItem(row, 7, makeItem(formatSector(s3Ms)));

        // Col 8: TYRE — label from tyre_compound, color from visual_compound
        auto* tyreItem = makeItem(tyreLabel(compound));
        QColor tyreFg = tyreTextColor(visual);
        if (tyreFg.isValid()) tyreItem->setForeground(tyreFg);
        timingTable->setItem(row, 8, tyreItem);

        // Col 9: STATUS
        timingTable->setItem(row, 9, makeItem(statusText));

        // Row height
        timingTable->setRowHeight(row, 22);
    }
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
        lastPlayerTelemetryData = row;
        updateTyresPage();
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
        lastPlayerStatusData = row;
        updateRacePanel();
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
        lastPlayerDamageData = row;
        updateTyresPage();
    } else if (type == "tyre_sets") {
        lastTyreSetsData = row;
        updateTyreSetsTable();
    } else if (type == "lap") {
        emit lapUpdated(row["position"].get<int>(), row["lap_num"].get<int>());
        lastPlayerLapData = row;
        updateRacePanel();
    } else if (type == "timing") {
        lastTimingData = row;
        updateTimingTable();
    } else if (type == "participants") {
        lastParticipantsData = row;
        updateTimingTable();
    } else if (type == "all_status") {
        lastAllStatusData = row;
        updateTimingTable();
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
