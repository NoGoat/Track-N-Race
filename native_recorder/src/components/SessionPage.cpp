#include "../MainWindow.h"
#include "TrackMapWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>
#include <QSizePolicy>
#include <QListWidget>
#include <QListWidgetItem>
#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <unordered_map>

// ── Marshal zones strip widget ────────────────────────────────────────────

class MarshalStripWidget : public QWidget {
public:
    std::vector<std::pair<float, int>> zones;

    explicit MarshalStripWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(4);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const int w = width(), h = height();
        
        if (zones.empty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 18));
            p.drawRoundedRect(0, 0, w, h, h / 2.0, h / 2.0);
            return;
        }

        const int gap = 2;
        p.setPen(Qt::NoPen);
        for (size_t i = 0; i < zones.size(); ++i) {
            float start = zones[i].first;
            float end   = (i + 1 < zones.size()) ? zones[i + 1].first : 1.0f;
            int x0 = (int)(start * w);
            int x1 = (int)(end   * w);
            if (x1 <= x0) x1 = x0 + 1;
            
            if (i + 1 < zones.size() && x1 - x0 > gap) {
                x1 -= gap;
            }

            QColor c;
            switch (zones[i].second) {
                case 1:  c = QColor("#00c853"); break; // green
                case 2:  c = QColor("#2196f3"); break; // blue
                case 3:  c = QColor("#fdd835"); break; // yellow
                case 4:  c = QColor("#e53935"); break; // red
                default: c = QColor(255, 255, 255, 18); break;
            }
            p.setBrush(c);
            p.drawRoundedRect(x0, 0, x1 - x0, h, h / 2.0, h / 2.0);
        }
    }
};

// ── Lookup tables ─────────────────────────────────────────────────────────

static const char* trackGpName(int id) {
    static const char* gp[] = {
        "Australian GP", "French GP", "Chinese GP", "Bahrain GP", "Spanish GP",
        "Monaco GP", "Canadian GP", "British GP", "German GP", "Hungarian GP",
        "Belgian GP", "Italian GP", "Singapore GP", "Japanese GP", "Abu Dhabi GP",
        "US GP", "Brazilian GP", "Austrian GP", "Russian GP", "Mexican GP",
        "Azerbaijan GP", "Bahrain Short", "British Short", "US Short", "Japanese Short",
        "Bahrain Short 2", "Vietnam GP", "Dutch GP", "Emilia Romagna GP", "Portuguese GP",
        "Saudi Arabian GP", "Miami GP", "Las Vegas GP", "Qatar GP",
    };
    if (id >= 0 && id < 34) return gp[id];
    return "Grand Prix";
}

static const char* trackCircuitName(int id) {
    static const char* cn[] = {
        "Albert Park", "Paul Ricard", "Shanghai", "Bahrain", "Catalunya",
        "Monte Carlo", "Circuit Gilles Villeneuve", "Silverstone", "Hockenheimring", "Hungaroring",
        "Spa-Francorchamps", "Monza", "Marina Bay", "Suzuka", "Yas Marina",
        "Circuit of the Americas", "Interlagos", "Red Bull Ring", "Sochi Autodrom", "Hermanos Rodriguez",
        "Baku City Circuit", "Bahrain Short", "Silverstone Short", "COTA Short", "Suzuka Short",
        "Bahrain Short 2", "Hanoi Circuit", "Zandvoort", "Imola", "Portimão",
        "Jeddah Corniche", "Miami International Autodrome", "Las Vegas Strip", "Losail International",
    };
    if (id >= 0 && id < 34) return cn[id];
    return "—";
}

static const char* sessionTypeName(int t) {
    static const char* names[] = {
        "Unknown", "Practice 1", "Practice 2", "Practice 3", "Short Practice",
        "Q1", "Q2", "Q3", "Short Qualifying", "One-Shot Qualifying",
        "Race", "Race 2", "Race 3", "Time Trial",
    };
    if (t >= 0 && t < 14) return names[t];
    return "—";
}

static const char* weatherLabel(int w) {
    static const char* l[] = { "Clear", "Light Cloud", "Overcast", "Light Rain", "Heavy Rain", "Storm" };
    if (w >= 0 && w < 6) return l[w];
    return "—";
}

static QString eventCodeLabel(const std::string& code) {
    if (code == "FTLP") return "Fastest Lap";
    if (code == "DRSE") return "DRS Enabled";
    if (code == "DRSD") return "DRS Disabled";
    if (code == "SCAR") return "Safety Car";
    if (code == "RTMT") return "Retirement";
    if (code == "RCWN") return "Race Winner";
    if (code == "PENA") return "Penalty";
    if (code == "DTSV") return "DT Served";
    if (code == "SGSV") return "SG Served";
    if (code == "SSTA") return "Session Start";
    if (code == "SEND") return "Session End";
    if (code == "RDFL") return "Red Flag";
    if (code == "CHQF") return "Chequered Flag";
    if (code == "LGOT") return "Lights Out";
    return QString::fromStdString(code);
}

static QColor eventCodeColor(const std::string& code) {
    if (code == "FTLP")                                      return QColor("#BF5FFF");
    if (code == "RCWN")                                      return QColor("#FFD700");
    if (code == "SCAR")                                      return QColor("#ffd700");
    if (code == "RDFL")                                      return QColor("#e10600");
    if (code == "DRSE" || code == "LGOT")                   return QColor("#37872D");
    if (code == "DRSD")                                     return QColor("#6e7177");
    if (code == "SSTA" || code == "SEND")                   return QColor("#5794F2");
    if (code == "RTMT" || code == "CHQF" ||
        code == "DTSV" || code == "SGSV")                   return QColor("#a0a8b8");
    return QColor();
}

// Maps F1 penalty_type → display label. Returns nullptr for unknown types
// (which are hidden, matching the Electron reference's null return).
static const char* penaltyTypeLabel(int pt) {
    switch (pt) {
        case 0: return "Drive Through";
        case 1: return "Stop-Go";
        case 2: return "Grid Penalty";
        case 4: return "Time Penalty";
        case 5: return "Warning";
        case 6: return "DSQ";
        default: return nullptr;
    }
}

static QColor penaltyTypeColor(int pt) {
    switch (pt) {
        case 2: case 4: return QColor("#c47d0e"); // Grid / Time — orange
        case 5:         return QColor("#ffd700"); // Warning — gold
        default:        return QColor("#e10600"); // Drive Through / Stop-Go / DSQ — red
    }
}

// F1 infringement_type → human-readable reason (ported from the Electron App.tsx table).
static QString infringementLabel(int id) {
    static const std::unordered_map<int, const char*> labels = {
        {0,  "Blocking by slowing"},        {1,  "Blocking wrong way"},
        {2,  "Reversing off start line"},   {3,  "Big collision"},
        {4,  "Small collision"},            {5,  "Collision — failed to hand back"},
        {6,  "Collision — attack from rear"},
        {7,  "SC delta exceeded"},          {8,  "SC illegal overtake"},
        {9,  "SC exceeding allowed pace"},  {10, "Cornering under SC"},
        {11, "SC must pit this lap"},       {12, "SC pit lane curfew"},
        {13, "Pit lane too fast"},          {14, "Unsafe release"},
        {15, "Pit re-entry too slow"},      {16, "In pit too fast"},
        {17, "Unsafe release"},             {18, "Escape from pit"},
        {19, "Ignoring blue flags"},        {20, "Ignoring yellow flags"},
        {21, "Ignoring drive through"},     {22, "Too many drive throughs"},
        {23, "DT — serve this lap"},        {24, "DT — serve next lap"},
        {25, "Pit stop failed to serve"},   {26, "Hanging around"},
        {27, "Hang around for SC"},         {28, "Return to pits"},
        {29, "Tyre regulations"},           {30, "Lap invalidated"},
        {31, "This + next lap invalid"},    {32, "Lap invalid (no reason)"},
        {33, "This + next invalid (no reason)"}, {34, "This + prev lap invalid"},
        {35, "This + prev invalid (no reason)"}, {36, "Retired"},
        {37, "Black flag timer"},           {38, "Unserved stop-go"},
        {39, "Unserved drive through"},     {40, "Engine change"},
        {41, "Gearbox change"},             {42, "Parc fermé change"},
        {43, "League grid penalty"},        {44, "Retry penalty"},
        {45, "Illegal time gain"},          {46, "Mandatory pit stop"},
        {47, "Attribute assigned"},         {48, "Corner cutting"},
    };
    auto it = labels.find(id);
    return it != labels.end() ? QString::fromUtf8(it->second) : QString();
}

// ── Color helpers ─────────────────────────────────────────────────────────

static QColor trackTempColor(int c) {
    if (c < 25) return QColor("#5794F2");
    if (c < 35) return QColor("#37872D");
    if (c < 45) return QColor("#FADE2A");
    if (c < 55) return QColor("#FF9830");
    return QColor("#C4162A");
}

static QColor airTempColor(int c) {
    if (c < 18) return QColor("#5794F2");
    if (c < 25) return QColor("#37872D");
    if (c < 31) return QColor("#FADE2A");
    return QColor("#FF9830");
}

// ── Session page builder ──────────────────────────────────────────────────

QWidget* MainWindow::buildSessionPage() {
    QWidget* w = new QWidget;
    QVBoxLayout* root = new QVBoxLayout(w);
    // No left padding here so the full-width separator lines reach the left edge;
    // the left inset is re-added to the inner text rows below instead.
    root->setContentsMargins(0, 8, 0, 0);
    root->setSpacing(0);

    // ── Header ───────────────────────────────────────────────────
    QWidget* hdr = new QWidget;
    hdr->setFixedHeight(58);
    QHBoxLayout* hh = new QHBoxLayout(hdr);
    hh->setContentsMargins(10, 0, 0, 0);   // left inset for the header text
    hh->setSpacing(16);

    QWidget* gpBlock = new QWidget;
    QVBoxLayout* gpv = new QVBoxLayout(gpBlock);
    gpv->setContentsMargins(0, 0, 0, 0);
    gpv->setSpacing(3);
    gpv->setAlignment(Qt::AlignVCenter);
    sp_gpName = new QLabel("—");
    QFont gnf; gnf.setPointSize(13); gnf.setBold(true);
    sp_gpName->setFont(gnf);
    sp_circuitName = new QLabel("—");
    QFont cnf; cnf.setPointSize(9);
    sp_circuitName->setFont(cnf);
    sp_circuitName->setForegroundRole(QPalette::PlaceholderText);
    gpv->addWidget(sp_gpName);
    gpv->addWidget(sp_circuitName);
    hh->addWidget(gpBlock);

    QWidget* zoneWrap = new QWidget;
    QVBoxLayout* zv = new QVBoxLayout(zoneWrap);
    zv->setContentsMargins(16, 6, 16, 6);
    zv->setSpacing(8);
    
    QWidget* headerWrap = new QWidget;
    QHBoxLayout* headerL = new QHBoxLayout(headerWrap);
    headerL->setContentsMargins(0, 0, 0, 0);
    
    QLabel* zonesLbl = new QLabel("ZONES");
    QFont zlf; zlf.setPointSize(7); zlf.setBold(true);
    zonesLbl->setFont(zlf);
    zonesLbl->setForegroundRole(QPalette::PlaceholderText);
    headerL->addWidget(zonesLbl);
    headerL->addStretch();
    
    QWidget* legend = new QWidget;
    QHBoxLayout* lh = new QHBoxLayout(legend);
    lh->setContentsMargins(0, 0, 0, 0);
    lh->setSpacing(12);
    struct LegItem { const char* col; const char* name; };
    LegItem legItems[] = {{"#fdd835","Yellow"},{"#00c853","Green"},{"#2196f3","Blue"},{"rgba(255,255,255,0.07)","Clear"}};
    for (auto& li : legItems) {
        QLabel* dot = new QLabel; dot->setFixedSize(12, 12);
        dot->setStyleSheet(QString("background:%1; border: 1px solid rgba(255,255,255,0.1); border-radius:2px;").arg(li.col));
        QLabel* txt = new QLabel(li.name);
        QFont ltf; ltf.setPointSize(7); txt->setFont(ltf);
        txt->setForegroundRole(QPalette::PlaceholderText);
        
        QWidget* wrap = new QWidget;
        QHBoxLayout* wh = new QHBoxLayout(wrap);
        wh->setContentsMargins(0,0,0,0);
        wh->setSpacing(4);
        wh->addWidget(dot); wh->addWidget(txt);
        lh->addWidget(wrap);
    }
    headerL->addWidget(legend);
    zv->addWidget(headerWrap);

    auto* strip = new MarshalStripWidget;
    sp_marshalStrip = strip;
    zv->addWidget(strip);
    hh->addWidget(zoneWrap, 1);

    QWidget* tmBlock = new QWidget;
    QVBoxLayout* tmv = new QVBoxLayout(tmBlock);
    tmv->setContentsMargins(0, 0, 0, 0);
    tmv->setSpacing(3);
    tmv->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sp_timeLeft = new QLabel("—:——");
    QFont tlf; tlf.setPointSize(22); tlf.setBold(true);
    sp_timeLeft->setFont(tlf);
    sp_timeLeft->setAlignment(Qt::AlignRight);
    sp_sessionType = new QLabel("—");
    QFont stf; stf.setPointSize(8);
    sp_sessionType->setFont(stf);
    sp_sessionType->setForegroundRole(QPalette::PlaceholderText);
    sp_sessionType->setAlignment(Qt::AlignRight);
    tmv->addWidget(sp_timeLeft);
    tmv->addWidget(sp_sessionType);
    hh->addWidget(tmBlock);

    root->addWidget(hdr);

    {   QFrame* f = new QFrame; f->setFrameShape(QFrame::HLine); f->setFrameShadow(QFrame::Sunken);
        root->addWidget(f); }

    // ── Stat cards ───────────────────────────────────────────────
    QWidget* statsRow = new QWidget;
    statsRow->setFixedHeight(58);
    QHBoxLayout* sh = new QHBoxLayout(statsRow);
    sh->setContentsMargins(10, 0, 0, 0);   // left inset for the first stat card
    sh->setSpacing(0);

    auto makeStatCard = [&](const QString& cap, QLabel*& out, const QString& accent = "") -> QWidget* {
        QWidget* card = new QWidget;
        QVBoxLayout* v = new QVBoxLayout(card);
        v->setContentsMargins(12, 6, 12, 6);
        v->setSpacing(2);
        QLabel* capLbl = new QLabel(cap);
        QFont capf; capf.setPointSize(7); capLbl->setFont(capf);
        capLbl->setForegroundRole(QPalette::PlaceholderText);
        out = new QLabel("—");
        QFont valf; valf.setPointSize(16); valf.setBold(true); out->setFont(valf);
        if (!accent.isEmpty()) out->setStyleSheet(QString("color:%1;").arg(accent));
        v->addWidget(capLbl); v->addWidget(out);
        return card;
    };

    auto addVSep = [&]() {
        QFrame* vf = new QFrame;
        vf->setFrameShape(QFrame::VLine); vf->setFrameShadow(QFrame::Sunken);
        sh->addWidget(vf);
    };

    sh->addWidget(makeStatCard("TOTAL LAPS", sp_statTotalLaps, ""),        1);
    addVSep();
    sh->addWidget(makeStatCard("REMAINING",  sp_statRemain,    ""),        1);
    addVSep();
    sh->addWidget(makeStatCard("PIT SPEED",  sp_statPitSpeed,  "#5794F2"), 1);
    addVSep();
    sh->addWidget(makeStatCard("PIT WINDOW", sp_statPitWin,    "#FADE2A"), 1);
    addVSep();
    sh->addWidget(makeStatCard("REJOIN",     sp_statRejoin,    "#37872D"), 1);
    addVSep();
    sh->addWidget(makeStatCard("TRACK TEMP",   sp_trackTemp,  ""), 1);
    addVSep();
    sh->addWidget(makeStatCard("AIR TEMP",     sp_airTemp,    ""), 1);
    addVSep();
    sh->addWidget(makeStatCard("TRACK LENGTH", sp_trackLen,   ""), 1);
    addVSep();
    sh->addWidget(makeStatCard("TIME OF DAY",  sp_timeOfDay,  ""), 1);

    root->addWidget(statsRow);

    QFrame* sep1 = new QFrame;
    sep1->setFrameShape(QFrame::HLine); sep1->setFrameShadow(QFrame::Sunken);
    root->addWidget(sep1);

    // ── Content ──────────────────────────────────────────────────
    QWidget* content = new QWidget;
    QHBoxLayout* ch = new QHBoxLayout(content);
    ch->setContentsMargins(0, 0, 0, 0);
    ch->setSpacing(0);

    // ── Left area: map placeholder + weather strip at bottom ──────
    QWidget* leftArea = new QWidget;
    QVBoxLayout* lv = new QVBoxLayout(leftArea);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(0);

    // Live track map fills the central area.
    trackMap_ = new TrackMapWidget;
    lv->addWidget(trackMap_, 1);

    // Weather strip pinned to bottom
    QFrame* wSep = new QFrame;
    wSep->setFrameShape(QFrame::HLine); wSep->setFrameShadow(QFrame::Sunken);
    lv->addWidget(wSep);

    QWidget* weatherStrip = new QWidget;
    weatherStrip->setFixedHeight(72);
    QHBoxLayout* wh = new QHBoxLayout(weatherStrip);
    wh->setContentsMargins(10, 8, 10, 8);
    wh->setSpacing(0);

    // NOW card
    QWidget* nowCard = new QWidget;
    nowCard->setMinimumWidth(80);
    QVBoxLayout* nv = new QVBoxLayout(nowCard);
    nv->setContentsMargins(0, 0, 14, 0);
    nv->setSpacing(3);
    QLabel* nowCap = new QLabel("NOW");
    QFont nowCapF; nowCapF.setPointSize(7); nowCapF.setBold(true);
    nowCap->setFont(nowCapF);
    nowCap->setForegroundRole(QPalette::PlaceholderText);
    sp_weatherNow = new QLabel("—");
    QFont wnf; wnf.setPointSize(11); wnf.setBold(true);
    sp_weatherNow->setFont(wnf);
    nv->addWidget(nowCap);
    nv->addWidget(sp_weatherNow);
    nv->addStretch();
    wh->addWidget(nowCard);

    QFrame* wvl = new QFrame;
    wvl->setFrameShape(QFrame::VLine); wvl->setFrameShadow(QFrame::Sunken);
    wh->addWidget(wvl);

    for (int i = 0; i < 5; ++i) {
        QWidget* fcCard = new QWidget;
        QVBoxLayout* fv = new QVBoxLayout(fcCard);
        fv->setContentsMargins(10, 0, 10, 0);
        fv->setSpacing(2);

        sp_fcTime[i] = new QLabel("");
        QFont ftf; ftf.setPointSize(7);
        sp_fcTime[i]->setFont(ftf);
        sp_fcTime[i]->setForegroundRole(QPalette::PlaceholderText);

        sp_fcWeather[i] = new QLabel("");
        QFont fwf; fwf.setPointSize(9); fwf.setBold(true);
        sp_fcWeather[i]->setFont(fwf);

        sp_fcRain[i] = new QLabel("");
        QFont frf; frf.setPointSize(8);
        sp_fcRain[i]->setFont(frf);
        sp_fcRain[i]->setStyleSheet("color:#5794F2;");

        fv->addStretch();
        fv->addWidget(sp_fcTime[i]);
        fv->addWidget(sp_fcWeather[i]);
        fv->addWidget(sp_fcRain[i]);
        fv->addStretch();
        wh->addWidget(fcCard, 1);
    }

    lv->addWidget(weatherStrip);
    ch->addWidget(leftArea, 1);

    // VLine between map and right panel
    QFrame* vdiv = new QFrame;
    vdiv->setFrameShape(QFrame::VLine); vdiv->setFrameShadow(QFrame::Sunken);
    ch->addWidget(vdiv);

    // ── Right panel: Proximity + Events ──────────────────────────
    QWidget* rightPanel = new QWidget;
    rightPanel->setFixedWidth(240);
    QVBoxLayout* rv = new QVBoxLayout(rightPanel);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(6);

    auto makeSection = [&](const QString& title) {
        QLabel* lbl = new QLabel(title);
        QFont f; f.setPointSize(8); f.setBold(true);
        lbl->setFont(f);
        lbl->setForegroundRole(QPalette::PlaceholderText);
        lbl->setContentsMargins(14, 0, 14, 0);
        rv->addWidget(lbl);
    };

    auto rpDivider = [&]() {
        QFrame* div = new QFrame;
        div->setFrameShape(QFrame::HLine); div->setFrameShadow(QFrame::Sunken);
        rv->addWidget(div);
    };

    // PROXIMITY
    makeSection("PROXIMITY");
    for (int i = 0; i < 3; ++i) {
        QWidget* row = new QWidget;
        sp_proxRow[i] = row;
        QHBoxLayout* ph = new QHBoxLayout(row);
        ph->setContentsMargins(14, 4, 14, 4);
        ph->setSpacing(6);

        sp_proxPos[i] = new QLabel("—");
        QFont pf; pf.setPointSize(8); pf.setBold(true); sp_proxPos[i]->setFont(pf);
        sp_proxPos[i]->setFixedWidth(28);
        sp_proxPos[i]->setForegroundRole(QPalette::PlaceholderText);

        sp_proxName[i] = new QLabel("—");
        QFont nmf; nmf.setPointSize(10); nmf.setBold(true); sp_proxName[i]->setFont(nmf);

        sp_proxGap[i] = new QLabel("—");
        QFont gf; gf.setPointSize(8); sp_proxGap[i]->setFont(gf);
        sp_proxGap[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sp_proxGap[i]->setForegroundRole(QPalette::PlaceholderText);

        ph->addWidget(sp_proxPos[i]);
        ph->addWidget(sp_proxName[i], 1);
        ph->addWidget(sp_proxGap[i]);
        rv->addWidget(row);
    }

    rpDivider();

    // EVENTS
    makeSection("EVENTS");
    sp_eventsList = new QListWidget;
    sp_eventsList->setSelectionMode(QAbstractItemView::NoSelection);
    sp_eventsList->setFocusPolicy(Qt::NoFocus);
    sp_eventsList->setAlternatingRowColors(false);
    sp_eventsList->setFrameShape(QFrame::NoFrame);
    sp_eventsList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sp_eventsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    sp_eventsList->setSpacing(0);
    sp_eventsList->setUniformItemSizes(false);
    sp_eventsList->setStyleSheet(
        "QListWidget{background:transparent;}"
        "QListWidget::item{border-bottom:1px solid rgba(255,255,255,0.06);}");
    sp_eventsList->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    rv->addWidget(sp_eventsList, 1);
    ch->addWidget(rightPanel);
    root->addWidget(content, 1);
    return w;
}

// ── Session page updater ──────────────────────────────────────────────────

void MainWindow::updateSessionPage() {
    if (!sp_gpName || lastSessionData.empty()) return;

    int trackId   = lastSessionData.value("track_id",        -1);
    int sessType  = lastSessionData.value("session_type",     0);
    int timeLeft  = lastSessionData.value("session_time_left", 0);
    int totalLaps = lastSessionData.value("total_laps",       0);
    int pitSpeed  = lastSessionData.value("pit_speed_limit",  0);
    int idealLap  = lastSessionData.value("pit_stop_window_ideal_lap",  0);
    int latestLap = lastSessionData.value("pit_stop_window_latest_lap", 0);
    int rejoin    = lastSessionData.value("pit_stop_rejoin_position",   0);
    int weather   = lastSessionData.value("weather",           0);
    int trackTemp = lastSessionData.value("track_temp",        0);
    int airTemp   = lastSessionData.value("air_temp",          0);
    int trackLenM = lastSessionData.value("track_length_m",    0);
    uint32_t todS = lastSessionData.value("time_of_day",       0u);

    sp_gpName->setText(trackGpName(trackId));
    sp_circuitName->setText(trackCircuitName(trackId));
    sp_sessionType->setText(sessionTypeName(sessType));

    sp_timeLeft->setText(QString("%1:%2")
        .arg(timeLeft / 60, 2, 10, QChar('0'))
        .arg(timeLeft % 60, 2, 10, QChar('0')));

    sp_statTotalLaps->setText(totalLaps > 0 ? QString::number(totalLaps) : "—");
    sp_statPitSpeed->setText(pitSpeed > 0 ? QString::number(pitSpeed) + " km/h" : "—");

    if (idealLap > 0 && latestLap > 0)
        sp_statPitWin->setText(QString("L%1–%2").arg(idealLap).arg(latestLap));
    else
        sp_statPitWin->setText("—");

    sp_statRejoin->setText(rejoin > 0 ? QString("P%1").arg(rejoin) : "—");

    sp_trackTemp->setText(QString::number(trackTemp) + "°C");
    sp_trackTemp->setStyleSheet("color:" + trackTempColor(trackTemp).name() + ";");

    sp_airTemp->setText(QString::number(airTemp) + "°C");
    sp_airTemp->setStyleSheet("color:" + airTempColor(airTemp).name() + ";");

    sp_trackLen->setText(trackLenM > 0 ? QString::number(trackLenM / 1000.0, 'f', 3) + " km" : "—");

    // time_of_day is minutes since midnight; show 12-hour clock like the app.
    const int tod  = (int)todS;
    const int h24  = (tod / 60) % 24;
    const int mins = tod % 60;
    const int h12  = (h24 % 12) ? (h24 % 12) : 12;
    sp_timeOfDay->setText(QString("%1:%2 %3")
        .arg(h12)
        .arg(mins, 2, 10, QChar('0'))
        .arg(h24 >= 12 ? "PM" : "AM"));

    sp_weatherNow->setText(weatherLabel(weather));

    if (lastSessionData.contains("weather_forecast_samples")) {
        const auto& fc = lastSessionData["weather_forecast_samples"];
        int count = std::min((int)fc.size(), 5);
        for (int i = 0; i < 5; ++i) {
            if (i < count) {
                sp_fcTime[i]->setText(QString("+%1m").arg(fc[i].value("time_offset", 0)));
                sp_fcWeather[i]->setText(weatherLabel(fc[i].value("weather", 0)));
                int rain = fc[i].value("rain_percentage", 0);
                sp_fcRain[i]->setText(rain > 0 ? QString("%1%").arg(rain) : "");
            } else {
                sp_fcTime[i]->setText("");
                sp_fcWeather[i]->setText("");
                sp_fcRain[i]->setText("");
            }
        }
    }

    if (lastSessionData.contains("marshal_zones")) {
        auto* ms = static_cast<MarshalStripWidget*>(sp_marshalStrip);
        ms->zones.clear();
        for (const auto& z : lastSessionData["marshal_zones"]) {
            int flag = z.value("flag", -1);
            if (flag != -1) {
                ms->zones.push_back({z.value("zone_start", 0.0f), flag});
            }
        }
        ms->update();
    }

    if (!lastTimingData.empty() && lastTimingData.contains("cars")) {
        int playerIdx = lastTimingData.value("player_idx", -1);
        for (const auto& car : lastTimingData["cars"]) {
            if (car.value("idx", -1) == playerIdx) {
                int lap = car.value("lap_num", 0);
                sp_statRemain->setText((totalLaps > 0 && lap > 0)
                    ? QString::number(totalLaps - lap + 1) : "—");
                break;
            }
        }
    }
}

// ── Session events updater ────────────────────────────────────────────────

void MainWindow::updateSessionEvents() {
    if (!sp_eventsList || sessionEventLog.empty()) return;

    sp_eventsList->clear();

    auto get3LetterCode = [&](int carIdx) -> QString {
        if (carIdx < 0 || !lastParticipantsData.contains("drivers")) return "—";
        for (const auto& d : lastParticipantsData["drivers"]) {
            if (d.value("idx", -1) == carIdx) {
                QString qn = QString::fromStdString(d.value("name", ""));
                QStringList parts = qn.split(' ');
                QString last = parts.size() > 1 ? parts.last() : qn;
                return last.left(3).toUpper();
            }
        }
        return "—";
    };

    for (int i = (int)sessionEventLog.size() - 1; i >= 0; --i) {
        const auto& ev = sessionEventLog[i];
        std::string code = ev.value("code", "");

        int totalSecs = (int)ev.value("session_time", 0.0f);
        QString timeStr = QString("%1:%2")
            .arg(totalSecs / 60, 2, 10, QChar('0'))
            .arg(totalSecs % 60, 2, 10, QChar('0'));

        QString eventType;
        QString text;
        QColor colorOverride;

        if (code == "FTLP") {
            eventType = "Fastest Lap";
            float lapS = ev.value("lap_time_s", 0.0f);
            int lapMs  = (int)(lapS * 1000.0f);
            QString lapTimeStr = QString("%1:%2.%3")
                .arg(lapMs / 60000)
                .arg((lapMs % 60000) / 1000, 2, 10, QChar('0'))
                .arg(lapMs % 1000, 3, 10, QChar('0'));
            QString nameCode = get3LetterCode(ev.value("car_idx", -1));
            text = nameCode + " - " + lapTimeStr;
            colorOverride = QColor("#BF5FFF"); // Purple
        } else if (code == "PENA") {
            int pt = ev.value("penalty_type", -1);
            const char* ptLabel = penaltyTypeLabel(pt);
            if (!ptLabel) continue;

            QString nameCode = get3LetterCode(ev.value("car_idx", -1));
            QString inf = infringementLabel(ev.value("infringement_type", -1));

            if (pt == 5) {
                eventType = "Warning";
                text = nameCode + " - " + (inf.isEmpty() ? "Warning" : inf);
                colorOverride = QColor("#ffd700"); // Yellow
            } else {
                eventType = "Penalty";
                QString penText = ptLabel;
                int timeS = ev.value("penalty_time_s", 0);
                if ((pt == 1 || pt == 4) && timeS > 0) penText += QString(" %1s").arg(timeS);
                text = nameCode + " - " + penText;
                if (!inf.isEmpty()) text += " (" + inf + ")";
                colorOverride = QColor("#e10600"); // Red
            }
        } else if (code == "SCAR") {
            int t = ev.value("safety_car_type", 0);
            eventType = (t == 1) ? "Safety Car" : (t == 2) ? "Virtual SC"
                         : (t == 3) ? "Formation Lap" : "SC";
            int a = ev.value("event_type", 0);
            const char* action = (a == 0) ? "Deployed" : (a == 1) ? "Returning"
                               : (a == 2) ? "Returned" : (a == 3) ? "Resume Race" : "";
            text = action;
        } else if (code == "RTMT" || code == "RCWN" || code == "DTSV" || code == "SGSV") {
            eventType = eventCodeLabel(code);
            QString nameCode = get3LetterCode(ev.value("car_idx", -1));
            text = nameCode;
        } else {
            eventType = eventCodeLabel(code);
            text = "";
        }

        QColor c = colorOverride.isValid() ? colorOverride : eventCodeColor(code);
        if (!c.isValid()) c = QColor("#c8ccd4");

        const int hPad = 8, vPad = 6, gap = 2;
        int avail = sp_eventsList->viewport()->width();
        if (avail <= 0) avail = sp_eventsList->width() - 4;
        if (avail <= 0) avail = 240;
        
        QWidget* rowW = new QWidget;
        rowW->setObjectName("eventRow");
        rowW->setStyleSheet(QString(
            "#eventRow {"
            "  border-left: 3px solid %1;"
            "}"
        ).arg(c.name()));

        QVBoxLayout* vl = new QVBoxLayout(rowW);
        vl->setContentsMargins(hPad, vPad, hPad, vPad);
        vl->setSpacing(gap);

        QHBoxLayout* topH = new QHBoxLayout;
        topH->setContentsMargins(0, 0, 0, 0);

        QLabel* timeLbl = new QLabel(timeStr);
        QFont tf; tf.setPointSize(7); tf.setBold(true);
        tf.setStyleHint(QFont::Monospace); tf.setFamily("monospace");
        timeLbl->setFont(tf);
        timeLbl->setStyleSheet("color: #a0a8b8;");

        QLabel* typeLbl = new QLabel(eventType);
        QFont typeF; typeF.setPointSize(7); typeF.setBold(true);
        typeLbl->setFont(typeF);
        typeLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        typeLbl->setStyleSheet("color: " + c.name() + ";");

        topH->addWidget(timeLbl);
        topH->addWidget(typeLbl, 1);
        vl->addLayout(topH);

        int timeH = std::max(QFontMetrics(tf).height(), QFontMetrics(typeF).height());
        int textH = 0;

        if (!text.isEmpty()) {
            QLabel* textLbl = new QLabel(text);
            QFont lf; lf.setPointSize(9); lf.setWeight(QFont::DemiBold);
            textLbl->setFont(lf);
            textLbl->setWordWrap(true);
            textLbl->setStyleSheet("color: #E5E7EB; background: transparent;");
            vl->addWidget(textLbl);
            
            textH = QFontMetrics(lf).boundingRect(
                QRect(0, 0, avail - (2 * hPad), 10000), Qt::TextWordWrap, text).height();
        }

        int rowH = (2 * vPad) + timeH;
        if (textH > 0) rowH += gap + textH;

        auto* item = new QListWidgetItem;
        item->setSizeHint(QSize(avail, rowH));
        sp_eventsList->addItem(item);
        sp_eventsList->setItemWidget(item, rowW);
    }
}

// ── Proximity widget updater ──────────────────────────────────────────────

void MainWindow::updateProximityWidget() {
    if (!sp_proxPos[0] || lastTimingData.empty()) return;

    int playerIdx = lastTimingData.value("player_idx", -1);
    if (playerIdx < 0 || !lastTimingData.contains("cars")) {
        for (int i = 0; i < 3; ++i) sp_proxRow[i]->setVisible(false);
        return;
    }

    struct CarEntry { int idx; int pos; int gapMs; };
    std::vector<CarEntry> cars;
    for (const auto& car : lastTimingData["cars"]) {
        int rs = car.value("result_status", 0);
        if (rs == 0 || rs == 3) continue;
        cars.push_back({car.value("idx",-1), car.value("position",0), car.value("gap_ms",0)});
    }
    std::sort(cars.begin(), cars.end(), [](const CarEntry& a, const CarEntry& b){ return a.pos < b.pos; });

    int playerSortIdx = -1;
    for (int i = 0; i < (int)cars.size(); ++i)
        if (cars[i].idx == playerIdx) { playerSortIdx = i; break; }

    if (playerSortIdx < 0) {
        for (int i = 0; i < 3; ++i) sp_proxRow[i]->setVisible(false);
        return;
    }

    // Always show 3 rows: P1→[0,1,2], last→[n-2,n-1,n], mid→[n-1,n,n+1]
    int n = (int)cars.size();
    int rowSlot[3];
    if (playerSortIdx == 0) {
        rowSlot[0] = 0; rowSlot[1] = 1; rowSlot[2] = 2;
    } else if (playerSortIdx == n - 1) {
        rowSlot[0] = n - 3; rowSlot[1] = n - 2; rowSlot[2] = n - 1;
    } else {
        rowSlot[0] = playerSortIdx - 1;
        rowSlot[1] = playerSortIdx;
        rowSlot[2] = playerSortIdx + 1;
    }

    auto driverName = [&](int carIdx) -> QString {
        if (carIdx < 0 || !lastParticipantsData.contains("drivers")) return "—";
        for (const auto& d : lastParticipantsData["drivers"]) {
            if (d.value("idx", -1) == carIdx) {
                QString qn = QString::fromStdString(d.value("name", ""));
                QStringList parts = qn.split(' ');
                return (parts.size() > 1 ? parts.last() : qn).left(3).toUpper();
            }
        }
        return QString("C%1").arg(carIdx);
    };

    for (int i = 0; i < 3; ++i) {
        int si = rowSlot[i];
        if (si < 0 || si >= n) { sp_proxRow[i]->setVisible(false); continue; }
        sp_proxRow[i]->setVisible(true);
        const CarEntry& ce = cars[si];
        bool isPlayer = (ce.idx == playerIdx);
        bool isLeader = (ce.pos == 1);

        sp_proxPos[i]->setText(QString("P%1").arg(ce.pos));
        sp_proxName[i]->setText(driverName(ce.idx));

        if (isPlayer) {
            sp_proxPos[i]->setStyleSheet("color:#5794F2;");
            sp_proxName[i]->setStyleSheet("color:#5794F2;");
            sp_proxGap[i]->setText("—");
        } else {
            sp_proxPos[i]->setStyleSheet("");
            sp_proxName[i]->setStyleSheet("");
            if (isLeader)
                sp_proxGap[i]->setText("LEAD");
            else if (ce.gapMs > 0)
                sp_proxGap[i]->setText(QString("+%1.%2").arg(ce.gapMs/1000).arg(ce.gapMs%1000, 3, 10, QChar('0')));
            else
                sp_proxGap[i]->setText("—");
        }
    }
}
