#include "SessionPage.h"
#include "../Labels.h"
#include "CardColors.h"
#include "PageUiHelpers.h"
#include "TrackMapWidget.h"
#include "../IconUtils.h"

#include <QApplication>
#include <QIcon>
#include <QPixmap>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLayout>
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

namespace {

// Remove and delete every item in a layout so the stat-card row can be rebuilt in
// place when compact mode toggles at runtime.
void clearLayout(QLayout* lay) {
    if (!lay) return;
    while (QLayoutItem* item = lay->takeAt(0)) {
        if (QWidget* w = item->widget()) delete w;
        delete item;
    }
}

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

        // The first zone's raw zone_start is often > 0, which with absolute
        // positioning leaves a gap on the left. Normalise [firstStart, 1.0] → the
        // full width so the bar fills edge-to-edge (the first zone anchored left,
        // the last extending to the right), like the Electron MarshalStrip.
        const float base = zones.front().first;
        const float span = std::max(1.0f - base, 1e-4f);
        const int gap = 2;
        p.setPen(Qt::NoPen);
        for (size_t i = 0; i < zones.size(); ++i) {
            float start = (zones[i].first - base) / span;
            float end   = ((i + 1 < zones.size() ? zones[i + 1].first : 1.0f) - base) / span;
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

// Track id → { full GP name, full circuit name }. Ported verbatim from the
// Electron app's TRACK_INFO (SessionPanel.tsx) so the displayed strings match
// exactly. F1 track ids are sparse, so this is a keyed map — NOT a positional
// array (the old array was abbreviated and mis-indexed for ids ≥ 25).
const std::unordered_map<int, std::pair<const char*, const char*>>& trackInfo() {
    static const std::unordered_map<int, std::pair<const char*, const char*>> m = {
        { 0,  { "Australian Grand Prix",     "Albert Park Circuit" } },
        { 2,  { "Chinese Grand Prix",        "Shanghai International Circuit" } },
        { 3,  { "Bahrain Grand Prix",        "Bahrain International Circuit" } },
        { 4,  { "Spanish Grand Prix",        "Circuit de Barcelona-Catalunya" } },
        { 5,  { "Monaco Grand Prix",         "Circuit de Monaco" } },
        { 6,  { "Canadian Grand Prix",       "Circuit Gilles Villeneuve" } },
        { 7,  { "British Grand Prix",        "Silverstone Circuit" } },
        { 9,  { "Hungarian Grand Prix",      "Hungaroring" } },
        { 10, { "Belgian Grand Prix",        "Circuit de Spa-Francorchamps" } },
        { 11, { "Italian Grand Prix",        "Autodromo Nazionale Monza" } },
        { 12, { "Singapore Grand Prix",      "Marina Bay Street Circuit" } },
        { 13, { "Japanese Grand Prix",       "Suzuka International Racing Course" } },
        { 14, { "Abu Dhabi Grand Prix",      "Yas Marina Circuit" } },
        { 15, { "United States Grand Prix",  "Circuit of the Americas" } },
        { 16, { "São Paulo Grand Prix",      "Autódromo José Carlos Pace" } },
        { 17, { "Austrian Grand Prix",       "Red Bull Ring" } },
        { 19, { "Mexico City Grand Prix",    "Autódromo Hermanos Rodríguez" } },
        { 20, { "Azerbaijan Grand Prix",     "Baku City Circuit" } },
        { 26, { "Dutch Grand Prix",          "Circuit Zandvoort" } },
        { 27, { "Emilia Romagna Grand Prix", "Autodromo Enzo e Dino Ferrari" } },
        { 29, { "Saudi Arabian Grand Prix",  "Jeddah Corniche Circuit" } },
        { 30, { "Miami Grand Prix",          "Miami International Autodrome" } },
        { 31, { "Las Vegas Grand Prix",      "Las Vegas Street Circuit" } },
        { 32, { "Qatar Grand Prix",          "Losail International Circuit" } },
        { 39, { "British Grand Prix",        "Silverstone Circuit (Reverse)" } },
        { 40, { "Austrian Grand Prix",       "Red Bull Ring (Reverse)" } },
        { 41, { "Dutch Grand Prix",          "Circuit Zandvoort (Reverse)" } },
    };
    return m;
}

const char* trackGpName(int id) {
    auto it = trackInfo().find(id);
    return it != trackInfo().end() ? it->second.first : "Grand Prix";
}

const char* trackCircuitName(int id) {
    auto it = trackInfo().find(id);
    return it != trackInfo().end() ? it->second.second : "—";
}

const char* sessionTypeName(int t) {
    static const char* names[] = {
        "Unknown", "Practice 1", "Practice 2", "Practice 3", "Short Practice",
        "Q1", "Q2", "Q3", "Short Qualifying", "One-Shot Qualifying",
        "Race", "Race 2", "Race 3", "Time Trial",
    };
    if (t >= 0 && t < 14) return names[t];
    return "—";
}

const char* weatherLabel(int w) {
    static const char* l[] = { "Clear", "Light Cloud", "Overcast", "Light Rain", "Heavy Rain", "Storm" };
    if (w >= 0 && w < 6) return l[w];
    return "—";
}

// Bundled Breeze icon name + tint (dark / light theme) per F1 weather code
// (0..5), matching the Electron weather panel's icon + colour choices.
struct WeatherVis { const char* icon; const char* dark; const char* light; };
const WeatherVis kWeatherVis[] = {
    { "weather-clear-symbolic",             "#fde047", "#ca8a04" },  // 0 Clear
    { "weather-clouds-symbolic",            "#fb923c", "#c2410c" },  // 1 Light Cloud
    { "weather-overcast-symbolic",          "#94a3b8", "#475569" },  // 2 Overcast
    { "weather-showers-scattered-symbolic", "#7dd3fc", "#0284c7" },  // 3 Light Rain
    { "weather-showers-symbolic",           "#2563eb", "#1d4ed8" },  // 4 Heavy Rain
    { "weather-storm-symbolic",             "#c084fc", "#7c3aed" },  // 5 Storm
};

// Sets a tinted Breeze weather icon on `lbl` for weather code `w` at `px` logical
// px, device-pixel-ratio aware for crisp HiDPI output. Clears the label if the
// icon can't be resolved (e.g. Breeze not bundled), so text-only builds degrade.
void applyWeatherIcon(QLabel* lbl, int w, int px) {
    if (!lbl) return;
    if (w < 0 || w > 5) w = 2;
    const WeatherVis& v = kWeatherVis[w];
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    QIcon ic = breezeIcon(v.icon, QColor(dark ? v.dark : v.light));
    QPixmap pm = ic.isNull() ? QPixmap() : ic.pixmap(QSize(px, px), lbl->devicePixelRatioF());
    if (pm.isNull()) lbl->clear();
    else             lbl->setPixmap(pm);
}

QString eventCodeLabel(const std::string& code) {
    // Library i18n catalog (protocol-aware: DRSE/DRSD read "Straight Line Mode
    // …" under 2026). Unknown codes fall back to the raw code.
    const QString key   = "event." + QString::fromStdString(code);
    const QString label = tnr::L(key);
    return label == key ? QString::fromStdString(code) : label;
}

QColor eventCodeColor(const std::string& code) {
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
const char* penaltyTypeLabel(int pt) {
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

// F1 infringement_type → human-readable reason (ported from the Electron App.tsx table).
QString infringementLabel(int id) {
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

} // namespace

// ── Color helpers ─────────────────────────────────────────────────────────

// Track/air temp colours come from the shared library spec (session.trackTemp /
// session.airTemp) via tnr::cardColor — see updateSession.

// ── Session page builder ──────────────────────────────────────────────────

SessionPage::SessionPage(QWidget* parent)
    : QWidget(parent)
{
    compact_ = settings_.value("ui/compactMode", false).toBool();

    QVBoxLayout* root = new QVBoxLayout(this);
    // No left/top padding here so the full-width separator lines reach the left edge
    // and the header's vertical separators reach the toolbar above; the left inset
    // is re-added to the inner text rows below instead.
    root->setContentsMargins(0, 0, 0, 0);
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

    // Separator: Heading | Marshal Strip
    hh->addWidget(tnrui::vline());

    QWidget* zoneWrap = new QWidget;
    QVBoxLayout* zv = new QVBoxLayout(zoneWrap);
    zv->setContentsMargins(16, 6, 16, 6);
    zv->setSpacing(8);
    // Keep the ZONES label + strip a tight, vertically-centred group (like the GP
    // block). Without this the default-policy headerWrap expands to fill the 58px
    // header and pushes the 4px strip far below the label, leaving a big gap.
    zv->setAlignment(Qt::AlignVCenter);

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

    // Separator: Marshal Strip | Timer
    hh->addWidget(tnrui::vline());

    QWidget* tmBlock = new QWidget;
    QVBoxLayout* tmv = new QVBoxLayout(tmBlock);
    tmv->setContentsMargins(0, 0, 16, 0);   // right padding so the timer isn't flush to the edge
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
    mapFsHide_.push_back(hdr);

    { QWidget* hl = tnrui::hline(); root->addWidget(hl); mapFsHide_.push_back(hl); }

    // ── Stat cards ───────────────────────────────────────────────
    spStatsRow_ = new QWidget;
    QHBoxLayout* sh = new QHBoxLayout(spStatsRow_);
    sh->setContentsMargins(10, 0, 0, 0);   // left inset for the first stat card
    sh->setSpacing(0);
    buildSessionCards();   // populates spStatsRow_'s layout (rebuilt on compact toggle)

    root->addWidget(spStatsRow_);
    mapFsHide_.push_back(spStatsRow_);

    { QWidget* hl = tnrui::hline(); root->addWidget(hl); mapFsHide_.push_back(hl); }

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
    connect(trackMap_, &TrackMapWidget::fullscreenToggled, this,
            [this]{ setMapFullscreen(!mapFullscreen_); });
    // Enlarge/Restore Map button label follows the global toolbar-labels setting.
    trackMap_->setShowLabels(settings_.value("ui/toolbarShowLabels", false).toBool());

    // Weather strip pinned to bottom
    { QWidget* hl = tnrui::hline(); lv->addWidget(hl); mapFsHide_.push_back(hl); }

    QWidget* weatherStrip = new QWidget;
    weatherStrip->setFixedHeight(92);
    mapFsHide_.push_back(weatherStrip);
    QHBoxLayout* wh = new QHBoxLayout(weatherStrip);
    // No outer padding: the inter-card vlines are children of this layout, so any
    // top/bottom margin here would inset them and leave a gap to the strip's
    // horizontal border. The padding lives on each card instead (below).
    wh->setContentsMargins(0, 0, 0, 0);
    wh->setSpacing(0);

    // NOW card — same horizontal layout + width as the forecast cards, minus the
    // rain-% row (a current reading has no forecast percentage).
    QWidget* nowCard = new QWidget;
    QHBoxLayout* nh = new QHBoxLayout(nowCard);
    nh->setContentsMargins(12, 12, 12, 12);
    nh->setSpacing(10);

    sp_weatherNowIcon = new QLabel;
    sp_weatherNowIcon->setFixedSize(44, 44);
    sp_weatherNowIcon->setAlignment(Qt::AlignCenter);

    QWidget* nowInfo = new QWidget;
    QVBoxLayout* niv = new QVBoxLayout(nowInfo);
    niv->setContentsMargins(0, 0, 0, 0);
    niv->setSpacing(1);

    QLabel* nowCap = new QLabel("NOW");
    QFont nowCapF; nowCapF.setPointSize(7); nowCapF.setBold(true);
    nowCap->setFont(nowCapF);
    nowCap->setForegroundRole(QPalette::PlaceholderText);

    sp_weatherNow = new QLabel("—");
    QFont wnf; wnf.setPointSize(9); wnf.setBold(true);
    sp_weatherNow->setFont(wnf);

    niv->addStretch();
    niv->addWidget(nowCap);
    niv->addWidget(sp_weatherNow);
    niv->addStretch();

    nh->addStretch();
    nh->addWidget(sp_weatherNowIcon);
    nh->addWidget(nowInfo);
    nh->addStretch();
    wh->addWidget(nowCard, 1);

    wh->addWidget(tnrui::vline());

    for (int i = 0; i < 5; ++i) {
        if (i > 0) wh->addWidget(tnrui::vline());   // separator between forecast cards

        QWidget* fcCard = new QWidget;
        QHBoxLayout* fh = new QHBoxLayout(fcCard);
        fh->setContentsMargins(12, 12, 12, 12);
        fh->setSpacing(10);

        // Left: large weather icon.
        sp_fcIcon[i] = new QLabel;
        sp_fcIcon[i]->setFixedSize(44, 44);
        sp_fcIcon[i]->setAlignment(Qt::AlignCenter);

        // Right: three stacked rows — time offset, weather name, rain %.
        QWidget* info = new QWidget;
        QVBoxLayout* iv = new QVBoxLayout(info);
        iv->setContentsMargins(0, 0, 0, 0);
        iv->setSpacing(1);

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

        iv->addStretch();
        iv->addWidget(sp_fcTime[i]);
        iv->addWidget(sp_fcWeather[i]);
        iv->addWidget(sp_fcRain[i]);
        iv->addStretch();

        fh->addStretch();
        fh->addWidget(sp_fcIcon[i]);
        fh->addWidget(info);
        fh->addStretch();
        wh->addWidget(fcCard, 1);
    }

    lv->addWidget(weatherStrip);
    ch->addWidget(leftArea, 1);

    // VLine between map and right panel
    { QWidget* vl = tnrui::vline(); ch->addWidget(vl); mapFsHide_.push_back(vl); }

    // ── Right panel: Proximity + Events ──────────────────────────
    QWidget* rightPanel = new QWidget;
    rightPanel->setFixedWidth(240);
    mapFsHide_.push_back(rightPanel);
    QVBoxLayout* rv = new QVBoxLayout(rightPanel);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(6);

    auto makeSection = [&](const QString& title, int topPad = 0) {
        rv->addWidget(tnrui::makeSectionLabel(title, 14, topPad));
    };

    auto rpDivider = [&]() {
        rv->addWidget(tnrui::hline());
    };

    // PROXIMITY
    makeSection("PROXIMITY", 10);
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
}

// ── Event log maintenance ─────────────────────────────────────────────────

void SessionPage::addEvent(const nlohmann::json& eventRow) {
    eventLog_.push_back(eventRow);
}

void SessionPage::clearEvents() {
    eventLog_.clear();
}

void SessionPage::setRenderingActive(bool on) {
    if (trackMap_) trackMap_->setRenderingActive(on);
}

// Build (or rebuild in place) the key-driven stat cards into spStatsRow_. Called
// from the ctor and again on a compact-mode toggle: it clears the row and the
// value map first so the new cards fully replace the old ones. Compact collapses
// each card to one line — label left, value (with its embedded units) centred.
void SessionPage::buildSessionCards() {
    QHBoxLayout* sh = qobject_cast<QHBoxLayout*>(spStatsRow_->layout());
    clearLayout(sh);
    spCardValue_.clear();
    spStatsRow_->setFixedHeight(compact_ ? 34 : 58);

    const bool compact = compact_;
    // Key-driven cards: registered into spCardValue_ by key. Unconditional colours
    // come from the shared library spec at build; conditional ones (temps) are
    // applied per-update in updateSession. `out` is kept as a convenience alias
    // for this page's bespoke per-card value formatting.
    auto makeStatCard = [&](const QString& key, const QString& cap, QLabel*& out,
                            const QString& colorSpec = "") -> QWidget* {
        QWidget* card = new QWidget;
        QLabel* capLbl = new QLabel(cap);
        QFont capf; capf.setPointSize(compact ? 8 : 7); capLbl->setFont(capf);
        capLbl->setForegroundRole(QPalette::PlaceholderText);
        out = new QLabel("—");
        QFont valf; valf.setPointSize(compact ? 13 : 16); valf.setBold(true); out->setFont(valf);
        if (!colorSpec.isEmpty()) {
            const QColor c = tnr::cardColor(colorSpec.toStdString());
            if (c.isValid()) out->setStyleSheet("color:" + c.name() + ";");
        }
        spCardValue_[key] = out;
        if (compact) {
            QHBoxLayout* cl = new QHBoxLayout(card);
            cl->setContentsMargins(12, 3, 12, 3);
            cl->setSpacing(4);
            cl->addWidget(capLbl);
            cl->addStretch();
            cl->addWidget(out);
            cl->addStretch();
        } else {
            QVBoxLayout* v = new QVBoxLayout(card);
            v->setContentsMargins(12, 6, 12, 6);
            v->setSpacing(2);
            v->addWidget(capLbl); v->addWidget(out);
        }
        return card;
    };

    auto addVSep = [&]() {
        sh->addWidget(tnrui::vline());
    };

    sh->addWidget(makeStatCard("totalLaps", "TOTAL LAPS", sp_statTotalLaps),                    1);
    addVSep();
    sh->addWidget(makeStatCard("remaining", "REMAINING",  sp_statRemain),                       1);
    addVSep();
    sh->addWidget(makeStatCard("pitSpeed",  "PIT SPEED",  sp_statPitSpeed, "session.pitSpeed"), 1);
    addVSep();
    sh->addWidget(makeStatCard("pitWindow", "PIT WINDOW", sp_statPitWin,   "session.pitWindow"),1);
    addVSep();
    sh->addWidget(makeStatCard("rejoin",    "REJOIN",     sp_statRejoin,   "session.rejoin"),   1);
    addVSep();
    sh->addWidget(makeStatCard("trackTemp", "TRACK TEMP", sp_trackTemp),                        1);
    addVSep();
    sh->addWidget(makeStatCard("airTemp",   "AIR TEMP",   sp_airTemp),                          1);
    addVSep();
    sh->addWidget(makeStatCard("trackLen",  "TRACK LENGTH", sp_trackLen),                       1);
    addVSep();
    sh->addWidget(makeStatCard("timeOfDay", "TIME OF DAY",  sp_timeOfDay),                      1);
}

// Live compact-mode toggle. Rebuilds the cards at the new density; MainWindow
// re-feeds the latest session row so the fresh labels repaint (see
// MainWindow::setCompactMode).
void SessionPage::setCompactMode(bool on) {
    if (compact_ == on) return;
    compact_ = on;
    buildSessionCards();
}

// ── Session page updater ──────────────────────────────────────────────────

void SessionPage::updateSession(const nlohmann::json& session, const nlohmann::json& timing) {
    if (!sp_gpName || session.empty()) return;

    int trackId   = session.value("track_id",        -1);
    int sessType  = session.value("session_type",     0);
    int timeLeft  = session.value("session_time_left", 0);
    int totalLaps = session.value("total_laps",       0);
    int pitSpeed  = session.value("pit_speed_limit",  0);
    int idealLap  = session.value("pit_stop_window_ideal_lap",  0);
    int latestLap = session.value("pit_stop_window_latest_lap", 0);
    int rejoin    = session.value("pit_stop_rejoin_position",   0);
    int weather   = session.value("weather",           0);
    int trackTemp = session.value("track_temp",        0);
    int airTemp   = session.value("air_temp",          0);
    int trackLenM = session.value("track_length_m",    0);
    uint32_t todS = session.value("time_of_day",       0u);

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
    sp_trackTemp->setStyleSheet("color:" + tnr::cardColor("session.trackTemp", trackTemp).name() + ";");

    sp_airTemp->setText(QString::number(airTemp) + "°C");
    sp_airTemp->setStyleSheet("color:" + tnr::cardColor("session.airTemp", airTemp).name() + ";");

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
    applyWeatherIcon(sp_weatherNowIcon, weather, 40);

    if (session.contains("weather_forecast_samples")) {
        const auto& fc = session["weather_forecast_samples"];
        int count = std::min((int)fc.size(), 5);
        for (int i = 0; i < 5; ++i) {
            if (i < count) {
                const int fw = fc[i].value("weather", 0);
                sp_fcTime[i]->setText(QString("+%1m").arg(fc[i].value("time_offset", 0)));
                sp_fcWeather[i]->setText(weatherLabel(fw));
                applyWeatherIcon(sp_fcIcon[i], fw, 40);
                int rain = fc[i].value("rain_percentage", 0);
                sp_fcRain[i]->setText(rain > 0 ? QString("%1%").arg(rain) : "");
            } else {
                sp_fcTime[i]->setText("");
                sp_fcWeather[i]->setText("");
                if (sp_fcIcon[i]) sp_fcIcon[i]->clear();
                sp_fcRain[i]->setText("");
            }
        }
    }

    if (session.contains("marshal_zones")) {
        auto* ms = static_cast<MarshalStripWidget*>(sp_marshalStrip);
        ms->zones.clear();
        for (const auto& z : session["marshal_zones"]) {
            int flag = z.value("flag", -1);
            if (flag != -1) {
                ms->zones.push_back({z.value("zone_start", 0.0f), flag});
            }
        }
        ms->update();
    }

    if (!timing.empty() && timing.contains("cars")) {
        int playerIdx = timing.value("player_idx", -1);
        for (const auto& car : timing["cars"]) {
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

void SessionPage::updateEvents(const nlohmann::json& participants) {
    if (!sp_eventsList || eventLog_.empty()) return;

    sp_eventsList->clear();

    auto get3LetterCode = [&](int carIdx) -> QString {
        if (carIdx < 0 || !participants.contains("drivers")) return "—";
        for (const auto& d : participants["drivers"]) {
            if (d.value("idx", -1) == carIdx) {
                QString qn = QString::fromStdString(d.value("name", ""));
                QStringList parts = qn.split(' ');
                QString last = parts.size() > 1 ? parts.last() : qn;
                return last.left(3).toUpper();
            }
        }
        return "—";
    };

    for (int i = (int)eventLog_.size() - 1; i >= 0; --i) {
        const auto& ev = eventLog_[i];
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

void SessionPage::updateProximity(const nlohmann::json& timing, const nlohmann::json& participants) {
    if (!sp_proxPos[0] || timing.empty()) return;

    int playerIdx = timing.value("player_idx", -1);
    if (playerIdx < 0 || !timing.contains("cars")) {
        for (int i = 0; i < 3; ++i) sp_proxRow[i]->setVisible(false);
        return;
    }

    struct CarEntry { int idx; int pos; int gapMs; };
    std::vector<CarEntry> cars;
    for (const auto& car : timing["cars"]) {
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
        if (carIdx < 0 || !participants.contains("drivers")) return "—";
        for (const auto& d : participants["drivers"]) {
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

// ── Track map updater ─────────────────────────────────────────────────────
// The map widget lives in the central area of this page; this pushes live
// data into it and re-reads the map display settings.

void SessionPage::updateTrackMap(const nlohmann::json& session,
                                 const nlohmann::json& participants,
                                 const nlohmann::json& positions) {
    if (!trackMap_) return;

    // Load the circuit geometry when the track changes.
    if (!session.empty()) {
        int tid = session.value("track_id", -1);
        if (tid >= 0 && tid != mapTrackId_) {
            trackMap_->setTrack(tid);
            mapTrackId_ = tid;
        }
        // 2026 SLM track status (0 = Full, 1 = Partial, -1 = n/a) selects which
        // SLM zone set the overlay draws. Absent on F1 24/25 sessions.
        trackMap_->setSlmTrackStatus(session.value("active_aero_track_status", -1));
    }

    // Theme: derive light/dark from the active palette.
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    trackMap_->setDark(dark);
    trackMap_->setLabelMode(static_cast<TrackMapWidget::LabelMode>(
        settings_.value("ui/trackMapLabelMode", 0).toInt()));
    trackMap_->setSectorColors(settings_.value("ui/trackMapSectorColors", true).toBool());
    trackMap_->setMapOpacity(settings_.value("ui/trackMapOpacity", 100).toInt() / 100.0);
    trackMap_->setIdleTimeout(settings_.value("ui/trackMapIdleTimeout", 0).toInt());

    if (!participants.empty())
        trackMap_->setParticipants(participants);
    if (!positions.empty())
        trackMap_->setPositions(positions);
}

void SessionPage::setMapFullscreen(bool on) {
    if (mapFullscreen_ == on) return;
    mapFullscreen_ = on;
    // Hide every sibling so the map's left area (and the map within it) expands to
    // fill the whole session view; restore them on exit.
    for (QWidget* w : mapFsHide_)
        if (w) w->setVisible(!on);
    if (trackMap_) trackMap_->setFullscreenState(on);
}
