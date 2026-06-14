#include "../MainWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>
#include <QSizePolicy>
#include <QScrollArea>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>

#include <algorithm>

// ── Marshal zones strip widget ────────────────────────────────────────────

class MarshalStripWidget : public QWidget {
public:
    std::vector<std::pair<float, int>> zones;

    explicit MarshalStripWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(14);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const int w = width(), h = height();
        p.fillRect(0, 0, w, h, QColor("#484c62"));
        for (size_t i = 0; i < zones.size(); ++i) {
            float start = zones[i].first;
            float end   = (i + 1 < zones.size()) ? zones[i + 1].first : 1.0f;
            int x0 = (int)(start * w);
            int x1 = (int)(end   * w);
            if (x1 <= x0) x1 = x0 + 1;
            QColor c;
            switch (zones[i].second) {
                case 1:  c = QColor("#fdd835"); break;
                case 2:  c = QColor("#00c853"); break;
                case 3:  c = QColor("#2196f3"); break;
                case 4:  c = QColor("#e53935"); break;
                default: c = QColor("#484c62"); break;
            }
            p.fillRect(x0, 0, x1 - x0, h, c);
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
    if (code == "DTSV") return "Drive-Through Served";
    if (code == "SGSV") return "Stop-Go Served";
    if (code == "SSTA") return "Session Start";
    if (code == "SEND") return "Session End";
    if (code == "RDFL") return "Red Flag";
    if (code == "CHQF") return "Chequered Flag";
    if (code == "LGOT") return "Lights Out";
    return QString::fromStdString(code);
}

static QColor eventCodeColor(const std::string& code) {
    if (code == "FTLP" || code == "CHQF" || code == "RCWN") return QColor("#FADE2A");
    if (code == "SCAR")                                       return QColor("#FF9830");
    if (code == "RDFL")                                       return QColor("#e53935");
    if (code == "RTMT" || code == "PENA")                    return QColor("#C4162A");
    if (code == "DRSE" || code == "DRSD")                    return QColor("#5794F2");
    if (code == "LGOT" || code == "SSTA" || code == "SEND")  return QColor("#37872D");
    return QColor();
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
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);

    // ── Header ───────────────────────────────────────────────────
    QWidget* hdr = new QWidget;
    hdr->setFixedHeight(58);
    QHBoxLayout* hh = new QHBoxLayout(hdr);
    hh->setContentsMargins(0, 0, 0, 0);
    hh->setSpacing(16);

    // Left: GP name + circuit
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

    // Center: ZONES label + strip + legend
    QWidget* zoneWrap = new QWidget;
    QVBoxLayout* zv = new QVBoxLayout(zoneWrap);
    zv->setContentsMargins(0, 6, 0, 6);
    zv->setSpacing(5);

    QLabel* zonesLbl = new QLabel("ZONES");
    QFont zlf; zlf.setPointSize(7); zlf.setBold(true);
    zonesLbl->setFont(zlf);
    zonesLbl->setForegroundRole(QPalette::PlaceholderText);
    zv->addWidget(zonesLbl);

    auto* strip = new MarshalStripWidget;
    sp_marshalStrip = strip;
    zv->addWidget(strip);

    QWidget* legend = new QWidget;
    QHBoxLayout* lh = new QHBoxLayout(legend);
    lh->setContentsMargins(0, 0, 0, 0);
    lh->setSpacing(8);
    struct LegItem { const char* col; const char* name; };
    LegItem legItems[] = {{"#fdd835","Yellow"},{"#00c853","Green"},{"#2196f3","Blue"},{"#484c62","Clear"}};
    for (auto& li : legItems) {
        QLabel* dot = new QLabel;
        dot->setFixedSize(8, 8);
        dot->setStyleSheet(QString("background:%1; border-radius:4px;").arg(li.col));
        QLabel* txt = new QLabel(li.name);
        QFont ltf; ltf.setPointSize(7);
        txt->setFont(ltf);
        txt->setForegroundRole(QPalette::PlaceholderText);
        lh->addWidget(dot);
        lh->addWidget(txt);
    }
    lh->addStretch();
    zv->addWidget(legend);
    hh->addWidget(zoneWrap, 1);

    // Right: time left + session type
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

    // ── Stat cards ───────────────────────────────────────────────
    QWidget* statsRow = new QWidget;
    statsRow->setFixedHeight(58);
    QHBoxLayout* sh = new QHBoxLayout(statsRow);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->setSpacing(0);

    auto makeStatCard = [&](const QString& cap, QLabel*& out, const QString& accent = "") -> QWidget* {
        QWidget* card = new QWidget;
        QVBoxLayout* v = new QVBoxLayout(card);
        v->setContentsMargins(12, 6, 12, 6);
        v->setSpacing(2);
        QLabel* capLbl = new QLabel(cap);
        QFont capf; capf.setPointSize(7);
        capLbl->setFont(capf);
        capLbl->setForegroundRole(QPalette::PlaceholderText);
        out = new QLabel("—");
        QFont valf; valf.setPointSize(16); valf.setBold(true);
        out->setFont(valf);
        if (!accent.isEmpty())
            out->setStyleSheet(QString("color:%1;").arg(accent));
        v->addWidget(capLbl);
        v->addWidget(out);
        return card;
    };

    auto addVSep = [&]() {
        QFrame* vf = new QFrame;
        vf->setFrameShape(QFrame::VLine);
        vf->setFrameShadow(QFrame::Sunken);
        sh->addWidget(vf);
    };

    sh->addWidget(makeStatCard("TOTAL LAPS",  sp_statTotalLaps, ""),        1);
    addVSep();
    sh->addWidget(makeStatCard("REMAINING",   sp_statRemain,    ""),        1);
    addVSep();
    sh->addWidget(makeStatCard("PIT SPEED",   sp_statPitSpeed,  "#5794F2"), 1);
    addVSep();
    sh->addWidget(makeStatCard("PIT WINDOW",  sp_statPitWin,    "#FADE2A"), 1);
    addVSep();
    sh->addWidget(makeStatCard("SAFETY CAR",  sp_statSafetyCar, ""),        1);

    root->addWidget(statsRow);

    QFrame* sep1 = new QFrame;
    sep1->setFrameShape(QFrame::HLine);
    sep1->setFrameShadow(QFrame::Sunken);
    root->addWidget(sep1);

    // ── Content ──────────────────────────────────────────────────
    QWidget* content = new QWidget;
    QHBoxLayout* ch = new QHBoxLayout(content);
    ch->setContentsMargins(0, 0, 0, 0);
    ch->setSpacing(0);

    // Left: map placeholder — events log sits here for now, map goes here later
    sp_eventsList = new QListWidget;
    sp_eventsList->setSelectionMode(QAbstractItemView::NoSelection);
    sp_eventsList->setFocusPolicy(Qt::NoFocus);
    sp_eventsList->setAlternatingRowColors(false);
    sp_eventsList->setFrameShape(QFrame::NoFrame);
    QFont evf; evf.setPointSize(8);
    sp_eventsList->setFont(evf);
    ch->addWidget(sp_eventsList, 1);

    // VLine
    QFrame* vdiv = new QFrame;
    vdiv->setFrameShape(QFrame::VLine);
    vdiv->setFrameShadow(QFrame::Sunken);
    ch->addWidget(vdiv);

    // ── Right panel ──────────────────────────────────────────────
    QScrollArea* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(240);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget* rp = new QWidget;
    QVBoxLayout* rv = new QVBoxLayout(rp);
    rv->setContentsMargins(14, 14, 14, 14);
    rv->setSpacing(6);

    auto makeRow = [&](const QString& label, QLabel*& valueOut) -> QWidget* {
        QWidget* row = new QWidget;
        QHBoxLayout* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 4, 0, 4);
        QLabel* lbl = new QLabel(label);
        QFont lf; lf.setPointSize(9); lbl->setFont(lf);
        lbl->setForegroundRole(QPalette::PlaceholderText);
        valueOut = new QLabel("—");
        QFont vf; vf.setPointSize(9); vf.setBold(true);
        valueOut->setFont(vf);
        valueOut->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        h->addWidget(lbl);
        h->addStretch();
        h->addWidget(valueOut);
        return row;
    };

    auto makeSection = [&](const QString& title) {
        QLabel* lbl = new QLabel(title);
        QFont f; f.setPointSize(8); f.setBold(true);
        lbl->setFont(f);
        lbl->setForegroundRole(QPalette::PlaceholderText);
        rv->addWidget(lbl);
    };

    auto addDivider = [&]() {
        QFrame* div = new QFrame;
        div->setFrameShape(QFrame::HLine);
        div->setFrameShadow(QFrame::Sunken);
        rv->addWidget(div);
    };

    // TRACK & WEATHER
    makeSection("TRACK & WEATHER");
    rv->addWidget(makeRow("Track Temp",   sp_trackTemp));
    rv->addWidget(makeRow("Air Temp",     sp_airTemp));
    rv->addWidget(makeRow("Track Length", sp_trackLen));
    rv->addWidget(makeRow("Time of Day",  sp_timeOfDay));

    addDivider();

    // Weather now
    QWidget* nowRow = new QWidget;
    QHBoxLayout* nh = new QHBoxLayout(nowRow);
    nh->setContentsMargins(0, 4, 0, 4);
    QLabel* nowCaption = new QLabel("Now");
    QFont nf; nf.setPointSize(9);
    nowCaption->setFont(nf);
    nowCaption->setForegroundRole(QPalette::PlaceholderText);
    sp_weatherNow = new QLabel("—");
    QFont wnf; wnf.setPointSize(9); wnf.setBold(true);
    sp_weatherNow->setFont(wnf);
    sp_weatherNow->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    nh->addWidget(nowCaption);
    nh->addStretch();
    nh->addWidget(sp_weatherNow);
    rv->addWidget(nowRow);

    // Forecast rows
    for (int i = 0; i < 5; ++i) {
        QWidget* fcRow = new QWidget;
        QHBoxLayout* fh = new QHBoxLayout(fcRow);
        fh->setContentsMargins(0, 2, 0, 2);
        fh->setSpacing(4);

        sp_fcTime[i] = new QLabel("");
        QFont ftf; ftf.setPointSize(8);
        sp_fcTime[i]->setFont(ftf);
        sp_fcTime[i]->setForegroundRole(QPalette::PlaceholderText);
        sp_fcTime[i]->setFixedWidth(32);

        sp_fcWeather[i] = new QLabel("");
        sp_fcWeather[i]->setFont(ftf);

        sp_fcRain[i] = new QLabel("");
        sp_fcRain[i]->setFont(ftf);
        sp_fcRain[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sp_fcRain[i]->setStyleSheet("color:#5794F2;");

        fh->addWidget(sp_fcTime[i]);
        fh->addWidget(sp_fcWeather[i], 1);
        fh->addWidget(sp_fcRain[i]);
        rv->addWidget(fcRow);
    }

    addDivider();

    // PROXIMITY
    makeSection("PROXIMITY");
    for (int i = 0; i < 3; ++i) {
        QWidget* row = new QWidget;
        sp_proxRow[i] = row;
        QHBoxLayout* ph = new QHBoxLayout(row);
        ph->setContentsMargins(0, 4, 0, 4);
        ph->setSpacing(6);

        sp_proxPos[i] = new QLabel("—");
        QFont pf; pf.setPointSize(8); pf.setBold(true);
        sp_proxPos[i]->setFont(pf);
        sp_proxPos[i]->setFixedWidth(28);
        sp_proxPos[i]->setForegroundRole(QPalette::PlaceholderText);

        sp_proxName[i] = new QLabel("—");
        QFont nmf; nmf.setPointSize(10); nmf.setBold(true);
        sp_proxName[i]->setFont(nmf);

        sp_proxGap[i] = new QLabel("—");
        QFont gf; gf.setPointSize(8);
        sp_proxGap[i]->setFont(gf);
        sp_proxGap[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sp_proxGap[i]->setForegroundRole(QPalette::PlaceholderText);

        ph->addWidget(sp_proxPos[i]);
        ph->addWidget(sp_proxName[i], 1);
        ph->addWidget(sp_proxGap[i]);
        rv->addWidget(row);
    }

    rv->addStretch();
    scroll->setWidget(rp);
    ch->addWidget(scroll);

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
    int scStatus  = lastSessionData.value("safety_car_status", 0);
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

    static const char* scLabels[] = { "—", "SC", "VSC", "Formation" };
    int sc = (scStatus >= 0 && scStatus < 4) ? scStatus : 0;
    sp_statSafetyCar->setText(scLabels[sc]);
    if (sc == 1 || sc == 2)
        sp_statSafetyCar->setStyleSheet("color:#FADE2A;");
    else if (sc == 3)
        sp_statSafetyCar->setStyleSheet("color:#5794F2;");
    else
        sp_statSafetyCar->setStyleSheet("");

    sp_trackTemp->setText(QString::number(trackTemp) + "°C");
    sp_trackTemp->setStyleSheet("color:" + trackTempColor(trackTemp).name() + ";");

    sp_airTemp->setText(QString::number(airTemp) + "°C");
    sp_airTemp->setStyleSheet("color:" + airTempColor(airTemp).name() + ";");

    sp_trackLen->setText(trackLenM > 0 ? QString::number(trackLenM / 1000.0, 'f', 3) + " km" : "—");

    sp_timeOfDay->setText(QString("%1:%2")
        .arg(todS / 3600, 2, 10, QChar('0'))
        .arg((todS % 3600) / 60, 2, 10, QChar('0')));

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
        for (const auto& z : lastSessionData["marshal_zones"])
            ms->zones.push_back({z.value("zone_start", 0.0f), z.value("flag", 0)});
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

    for (int i = (int)sessionEventLog.size() - 1; i >= 0; --i) {
        const auto& ev = sessionEventLog[i];
        std::string code = ev.value("code", "");

        int totalSecs = (int)ev.value("session_time", 0.0f);
        QString timeStr = QString("[%1:%2]")
            .arg(totalSecs / 60, 2, 10, QChar('0'))
            .arg(totalSecs % 60, 2, 10, QChar('0'));

        QString label  = eventCodeLabel(code);
        QString detail;

        if (code == "FTLP") {
            float lapS = ev.value("lap_time_s", 0.0f);
            int lapMs  = (int)(lapS * 1000.0f);
            detail = QString("%1:%2.%3")
                .arg(lapMs / 60000)
                .arg((lapMs % 60000) / 1000, 2, 10, QChar('0'))
                .arg(lapMs % 1000, 3, 10, QChar('0'));
            int carIdx = ev.value("car_idx", -1);
            if (carIdx >= 0 && lastParticipantsData.contains("drivers")) {
                for (const auto& d : lastParticipantsData["drivers"])
                    if (d.value("idx", -1) == carIdx) { detail = QString::fromStdString(d.value("name","")) + "  " + detail; break; }
            }
        } else if (code == "SCAR") {
            int t = ev.value("safety_car_type", 0);
            detail = (t == 1) ? "Full SC" : (t == 2) ? "VSC" : (t == 3) ? "Formation Lap" : "";
        } else if (code == "RTMT" || code == "RCWN" || code == "DTSV" || code == "SGSV") {
            int carIdx = ev.value("car_idx", -1);
            if (carIdx >= 0 && lastParticipantsData.contains("drivers"))
                for (const auto& d : lastParticipantsData["drivers"])
                    if (d.value("idx", -1) == carIdx) { detail = QString::fromStdString(d.value("name","")); break; }
        } else if (code == "PENA") {
            int carIdx = ev.value("car_idx", -1);
            int timeS  = ev.value("penalty_time_s", 0);
            if (carIdx >= 0 && lastParticipantsData.contains("drivers"))
                for (const auto& d : lastParticipantsData["drivers"])
                    if (d.value("idx", -1) == carIdx) { detail = QString::fromStdString(d.value("name","")); break; }
            if (timeS > 0) detail += (detail.isEmpty() ? "" : "  ") + QString("+%1s").arg(timeS);
        }

        QString text = timeStr + "  " + label;
        if (!detail.isEmpty()) text += "  —  " + detail;

        auto* item = new QListWidgetItem(text);
        QColor c = eventCodeColor(code);
        if (c.isValid()) item->setForeground(c);
        sp_eventsList->addItem(item);
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

    struct CarEntry { int idx; int pos; int gapMs; int rs; };
    std::vector<CarEntry> cars;
    for (const auto& car : lastTimingData["cars"]) {
        int rs = car.value("result_status", 0);
        if (rs == 0 || rs == 3) continue;
        cars.push_back({car.value("idx",-1), car.value("position",0), car.value("gap_ms",0), rs});
    }
    std::sort(cars.begin(), cars.end(), [](const CarEntry& a, const CarEntry& b){ return a.pos < b.pos; });

    int playerSortIdx = -1;
    for (int i = 0; i < (int)cars.size(); ++i)
        if (cars[i].idx == playerIdx) { playerSortIdx = i; break; }

    if (playerSortIdx < 0) {
        for (int i = 0; i < 3; ++i) sp_proxRow[i]->setVisible(false);
        return;
    }

    int rowSlot[3] = { playerSortIdx - 1, playerSortIdx, playerSortIdx + 1 };

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
        if (si < 0 || si >= (int)cars.size()) { sp_proxRow[i]->setVisible(false); continue; }
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
