#include "SessionPage.h"
#include "../CompactSettings.h"
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
#include <QMetaType>
#include <QVariant>

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

// The theme-aware tint used for weather code `w` — the same colour applyWeatherIcon
// gives the icon, reused to colour the weather-name text in the compact strip.
QColor weatherColor(int w) {
    if (w < 0 || w > 5) w = 2;
    const WeatherVis& v = kWeatherVis[w];
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    return QColor(dark ? v.dark : v.light);
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
        {2,  "Reversing off start line"},   {3,  "Severe collision"},
        {4,  "Collision"},            {5,  "Collision — failed to hand back"},
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
    cardsCompact_   = settings_.value(tnr::compactKey(tnr::CompactSection::SessionCards),   false).toBool();
    const QVariant weatherDensity = settings_.value(
        tnr::compactKey(tnr::CompactSection::SessionWeather), 0);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const bool legacyWeatherBool = weatherDensity.metaType().id() == QMetaType::Bool;
#else
    const bool legacyWeatherBool = weatherDensity.type() == QVariant::Bool;
#endif
    // Preserve the former boolean Compact appearance as the new Compact 2.
    weatherCompactLevel_ = legacyWeatherBool
        ? (weatherDensity.toBool() ? 2 : 0)
        : qBound(0, weatherDensity.toInt(), 3);
    const QVariant headerDensity = settings_.value(tnr::compactKey(tnr::CompactSection::SessionHeader), 0);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const bool legacyHeaderBool = headerDensity.metaType().id() == QMetaType::Bool;
#else
    const bool legacyHeaderBool = headerDensity.type() == QVariant::Bool;
#endif
    headerCompactLevel_ = legacyHeaderBool
        ? (headerDensity.toBool() ? 1 : 0)
        : qBound(0, headerDensity.toInt(), 2);
    eventsCompact_  = settings_.value(tnr::compactKey(tnr::CompactSection::SessionEvents),  false).toBool();
    proximityCompact_ = settings_.value(tnr::compactKey(tnr::CompactSection::SessionProximity), false).toBool();

    QVBoxLayout* root = new QVBoxLayout(this);
    // No left/top padding here so the full-width separator lines reach the left edge
    // and the header's vertical separators reach the toolbar above; the left inset
    // is re-added to the inner text rows below instead.
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Header ───────────────────────────────────────────────────
    sp_header_ = new QWidget;
    QHBoxLayout* hh = new QHBoxLayout(sp_header_);
    hh->setContentsMargins(10, 0, 0, 0);   // left inset for the header text
    buildHeader();   // populates the header (rebuilt on compact toggle)

    root->addWidget(sp_header_);
    mapFsHide_.push_back(sp_header_);

    sp_headerSep_ = tnrui::hline();
    root->addWidget(sp_headerSep_);
    mapFsHide_.push_back(sp_headerSep_);

    // ── Stat cards ───────────────────────────────────────────────
    spStatsRow_ = new QWidget;
    QHBoxLayout* sh = new QHBoxLayout(spStatsRow_);
    sh->setContentsMargins(10, 0, 0, 0);   // left inset for the first stat card
    sh->setSpacing(0);
    buildSessionCards();   // populates spStatsRow_'s layout (rebuilt on compact toggle)

    root->addWidget(spStatsRow_);
    mapFsHide_.push_back(spStatsRow_);

    sp_statsSep_ = tnrui::hline();
    root->addWidget(sp_statsSep_);
    mapFsHide_.push_back(sp_statsSep_);

    // ── Content ──────────────────────────────────────────────────
    QWidget* content = new QWidget;
    QHBoxLayout* ch = new QHBoxLayout(content);
    ch->setContentsMargins(0, 0, 0, 0);
    ch->setSpacing(0);

    // ── Left area: map placeholder + weather strip at bottom ──────
    leftArea_ = new QWidget;
    QVBoxLayout* lv = new QVBoxLayout(leftArea_);
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
    sp_weatherSep_ = tnrui::hline();
    lv->addWidget(sp_weatherSep_);
    mapFsHide_.push_back(sp_weatherSep_);

    sp_weatherStrip_ = new QWidget;
    mapFsHide_.push_back(sp_weatherStrip_);
    QHBoxLayout* wh = new QHBoxLayout(sp_weatherStrip_);
    // No outer padding: the inter-card vlines are children of this layout, so any
    // top/bottom margin here would inset them and leave a gap to the strip's
    // horizontal border. The padding lives on each card instead (see buildWeatherStrip).
    wh->setContentsMargins(0, 0, 0, 0);
    wh->setSpacing(0);
    buildWeatherStrip();   // populates the strip (rebuilt on compact toggle)

    lv->addWidget(sp_weatherStrip_);
    ch->addWidget(leftArea_, 1);

    // VLine between map and right panel
    midVLine_ = tnrui::vline();
    ch->addWidget(midVLine_);
    mapFsHide_.push_back(midVLine_);

    // ── Right panel: Proximity + Events ──────────────────────────
    rightPanel_ = new QWidget;
    rightPanel_->setFixedWidth(240);
    mapFsHide_.push_back(rightPanel_);
    QVBoxLayout* rv = new QVBoxLayout(rightPanel_);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(6);

    // PROXIMITY
    if (proximityCompact_) {
        sp_proxHeader = new QLabel("PROXIMITY");
        QFont f; f.setPointSize(8); f.setBold(true); f.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
        sp_proxHeader->setFont(f);
        sp_proxHeader->setForegroundRole(QPalette::PlaceholderText);
        sp_proxHeader->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        sp_proxHeader->setFixedHeight(32);
        sp_proxHeader->setContentsMargins(14, 0, 14, 0);
    } else {
        sp_proxHeader = tnrui::makeSectionLabel("PROXIMITY", 14, 10);
    }
    rv->addWidget(sp_proxHeader);

    for (int i = 0; i < 3; ++i) {
        QWidget* row = new QWidget;
        sp_proxRow[i] = row;
        if (proximityCompact_) {
            row->setFixedHeight(32);
        }
        QHBoxLayout* ph = new QHBoxLayout(row);
        ph->setContentsMargins(14, proximityCompact_ ? 0 : 4, 14, proximityCompact_ ? 0 : 4);
        ph->setSpacing(6);

        sp_proxPos[i] = new QLabel("—");
        QFont pf; pf.setPointSize(8); pf.setBold(true); sp_proxPos[i]->setFont(pf);
        sp_proxPos[i]->setFixedWidth(28);
        sp_proxPos[i]->setForegroundRole(QPalette::PlaceholderText);

        sp_proxName[i] = new QLabel("—");
        QFont nmf; nmf.setPointSize(proximityCompact_ ? 9 : 10); nmf.setBold(true); sp_proxName[i]->setFont(nmf);

        sp_proxGap[i] = new QLabel("—");
        QFont gf; gf.setPointSize(8); sp_proxGap[i]->setFont(gf);
        sp_proxGap[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sp_proxGap[i]->setForegroundRole(QPalette::PlaceholderText);

        ph->addWidget(sp_proxPos[i]);
        ph->addWidget(sp_proxName[i], 1);
        ph->addWidget(sp_proxGap[i]);
        rv->addWidget(row);
    }

    sp_proxSep_ = tnrui::hline();
    rv->addWidget(sp_proxSep_);

    // EVENTS
    if (eventsCompact_) {
        sp_eventsHeader = new QLabel("EVENTS");
        QFont f; f.setPointSize(8); f.setBold(true); f.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
        sp_eventsHeader->setFont(f);
        sp_eventsHeader->setForegroundRole(QPalette::PlaceholderText);
        sp_eventsHeader->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        sp_eventsHeader->setFixedHeight(32);
        sp_eventsHeader->setContentsMargins(14, 0, 14, 0);
    } else {
        sp_eventsHeader = tnrui::makeSectionLabel("EVENTS", 14, 0);
    }
    rv->addWidget(sp_eventsHeader);
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
    ch->addWidget(rightPanel_);
    root->addWidget(content, 1);

    applyLayout(loadLayout());
}

// ── Event log maintenance ─────────────────────────────────────────────────

void SessionPage::addEvent(const tnrp::RaceEventRow& eventRow) {
    eventLog_.push_back(eventRow);
}

void SessionPage::clearEvents() {
    eventLog_.clear();
}

void SessionPage::setRenderingActive(bool on) {
    if (trackMap_) trackMap_->setRenderingActive(on);
}

// Build (or rebuild in place) the top header: GP block, ZONES + marshal strip +
// legend, and the session clock. Compact mode drops the circuit name, folds the
// zones label, strip and legend onto a single row, and shrinks the clock. The full
// layout stacks the label+legend over the strip and shows the circuit name.
void SessionPage::buildHeader() {
    QHBoxLayout* hh = qobject_cast<QHBoxLayout*>(sp_header_->layout());
    clearLayout(hh);
    sp_gpBlock_ = nullptr;
    sp_zoneBlock_ = nullptr;
    sp_tmBlock_ = nullptr;
    sp_headerDiv1_ = nullptr;
    sp_headerDiv2_ = nullptr;
    const bool compact = headerCompactLevel_ > 0;
    const bool compact2 = headerCompactLevel_ == 2;
    sp_header_->setFixedHeight(compact ? 32 : 58);
    hh->setSpacing(compact ? 12 : 16);

    // GP block — name always; the circuit name only in the full layout.
    QWidget* gpBlock = new QWidget;
    sp_gpBlock_ = gpBlock;
    QVBoxLayout* gpv = new QVBoxLayout(gpBlock);
    gpv->setContentsMargins(0, 0, 0, 0);
    gpv->setSpacing(3);
    gpv->setAlignment(Qt::AlignVCenter);
    sp_gpName = new QLabel("—");
    QFont gnf; gnf.setPointSize(compact ? 11 : 13); gnf.setBold(true);
    sp_gpName->setFont(gnf);
    gpv->addWidget(sp_gpName);
    if (compact) {
        sp_circuitName = nullptr;   // dropped in compact; updateSession guards on it
    } else {
        sp_circuitName = new QLabel("—");
        QFont cnf; cnf.setPointSize(9);
        sp_circuitName->setFont(cnf);
        sp_circuitName->setForegroundRole(QPalette::PlaceholderText);
        gpv->addWidget(sp_circuitName);
    }
    hh->addWidget(gpBlock);

    sp_headerDiv1_ = tnrui::vline();
    hh->addWidget(sp_headerDiv1_);   // Heading | Marshal Strip

    // Shared pieces: the ZONES label and the marshal strip.
    QLabel* zonesLbl = new QLabel("ZONES");
    QFont zlf; zlf.setPointSize(7); zlf.setBold(true);
    zonesLbl->setFont(zlf);
    zonesLbl->setForegroundRole(QPalette::PlaceholderText);

    auto* strip = new MarshalStripWidget;
    sp_marshalStrip = strip;

    if (compact) {
        // One line: ZONES · [marshal strip] (or just [marshal strip] stretching full width in Compact 2).
        QWidget* zoneRow = new QWidget;
        sp_zoneBlock_ = zoneRow;
        QHBoxLayout* zl = new QHBoxLayout(zoneRow);
        zl->setContentsMargins(0, 0, 0, 0);
        zl->setSpacing(12);
        zl->setAlignment(Qt::AlignVCenter);
        if (!compact2) {
            zl->addWidget(zonesLbl);
        }
        zl->addWidget(strip, 1);   // strip stretches to fill the rest of the row
        hh->addWidget(zoneRow, 1);
    } else {
        // ZONES on top row, the marshal strip below it.
        QWidget* zoneWrap = new QWidget;
        sp_zoneBlock_ = zoneWrap;
        QVBoxLayout* zv = new QVBoxLayout(zoneWrap);
        zv->setContentsMargins(16, 6, 16, 6);
        zv->setSpacing(8);
        zv->setAlignment(Qt::AlignVCenter);

        QWidget* headerWrap = new QWidget;
        QHBoxLayout* headerL = new QHBoxLayout(headerWrap);
        headerL->setContentsMargins(0, 0, 0, 0);
        headerL->addWidget(zonesLbl);

        zv->addWidget(headerWrap);
        zv->addWidget(strip);
        hh->addWidget(zoneWrap, 1);
    }

    sp_headerDiv2_ = tnrui::vline();
    hh->addWidget(sp_headerDiv2_);   // Marshal Strip | Timer

    // Clock — no session-type sub-line, so it centres on its own.
    QWidget* tmBlock = new QWidget;
    sp_tmBlock_ = tmBlock;
    QVBoxLayout* tmv = new QVBoxLayout(tmBlock);
    tmv->setContentsMargins(0, 0, 16, 0);   // right padding so the timer isn't flush to the edge
    tmv->setSpacing(0);
    tmv->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sp_timeLeft = new QLabel("—:——");
    QFont tlf; tlf.setPointSize(compact ? 15 : 22); tlf.setBold(true);
    sp_timeLeft->setFont(tlf);
    sp_timeLeft->setAlignment(Qt::AlignRight);
    tmv->addWidget(sp_timeLeft);
    hh->addWidget(tmBlock);

    applyLayout(layout_);
}

// Build (or rebuild in place) the key-driven stat cards into spStatsRow_. Called
// from the ctor and again on a compact-mode toggle: it clears the row and the
// value map first so the new cards fully replace the old ones. Compact collapses
// each card to one line — label left, value (with its embedded units) centred.
void SessionPage::buildSessionCards() {
    QHBoxLayout* sh = qobject_cast<QHBoxLayout*>(spStatsRow_->layout());
    clearLayout(sh);
    spCardValue_.clear();
    for (int i = 0; i < SessionLayout::StatCardCount; ++i) sp_statCardFrames_[i] = nullptr;
    for (int i = 0; i < SessionLayout::StatCardCount - 1; ++i) sp_statCardDivs_[i] = nullptr;
    spStatsRow_->setFixedHeight(cardsCompact_ ? 34 : 58);
    // Compact cards carry their own 12px left margin, so the row's extra left inset
    // would over-indent the first card ("TOTAL LAPS") relative to the rest — drop it
    // in compact mode; the full two-line layout keeps its original inset.
    sh->setContentsMargins(cardsCompact_ ? 0 : 10, 0, 0, 0);

    const bool compact = cardsCompact_;
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
            // Two-value card: label left, value pinned right.
            QHBoxLayout* cl = new QHBoxLayout(card);
            cl->setContentsMargins(8, 3, 8, 3);
            cl->setSpacing(4);
            cl->addWidget(capLbl);
            cl->addStretch();
            cl->addWidget(out);
        } else {
            QVBoxLayout* v = new QVBoxLayout(card);
            v->setContentsMargins(12, 6, 12, 6);
            v->setSpacing(2);
            v->addWidget(capLbl); v->addWidget(out);
        }
        return card;
    };

    auto addCard = [&](int idx, const QString& key, const QString& cap, QLabel*& out, const QString& colorSpec = "") {
        if (idx > 0) {
            sp_statCardDivs_[idx - 1] = tnrui::vline();
            sh->addWidget(sp_statCardDivs_[idx - 1]);
        }
        sp_statCardFrames_[idx] = makeStatCard(key, cap, out, colorSpec);
        sh->addWidget(sp_statCardFrames_[idx], 1);
    };

    addCard(0, "totalLaps", "TOTAL LAPS", sp_statTotalLaps);
    addCard(1, "remaining", "REMAINING",  sp_statRemain);
    addCard(2, "pitSpeed",  "PIT SPEED",  sp_statPitSpeed, "session.pitSpeed");
    addCard(3, "pitWindow", "PIT WINDOW", sp_statPitWin,   "session.pitWindow");
    addCard(4, "rejoin",    "REJOIN",     sp_statRejoin,   "session.rejoin");
    addCard(5, "trackTemp", "TRACK TEMP", sp_trackTemp);
    addCard(6, "airTemp",   "AIR TEMP",   sp_airTemp);
    addCard(7, "trackLen",  compact ? "LENGTH" : "TRACK LENGTH", sp_trackLen);
    addCard(8, "timeOfDay", compact ? "TIME" : "TIME OF DAY", sp_timeOfDay);

    applyLayout(layout_);
}

// Build (or rebuild in place) the bottom weather strip: a NOW card plus five
// forecast cards. Compact 1 uses a medium-height horizontal row with a smaller
// icon. Compact 2 is the original short icon-free row: time (left) · weather name
// in the icon's colour (centre) · rain % (right). Compact 3 is the single-line layout:
// icon + weather (left) · time + rain % (right).
// The leaf labels are recreated, so updateSession repaints them (and tints compact weather names).
void SessionPage::buildWeatherStrip() {
    QHBoxLayout* wh = qobject_cast<QHBoxLayout*>(sp_weatherStrip_->layout());
    clearLayout(wh);
    // Icons are dropped in Compact 2 — null the members so updateSession skips
    // them and they aren't left dangling after clearLayout freed the old labels.
    sp_weatherNowIcon = nullptr;
    for (int i = 0; i < 5; ++i) sp_fcIcon[i] = nullptr;

    const bool compact = weatherCompactLevel_ > 0;
    const bool compact2 = weatherCompactLevel_ == 2;
    const bool compact3 = weatherCompactLevel_ == 3;
    sp_weatherStrip_->setFixedHeight(compact2 || compact3 ? 32 : compact ? 58 : 92);
    const int padX   = compact ? 8 : 12;
    const int padY   = (compact2 || compact3) ? 3 : compact ? 6 : 12;
    const int gap    = compact ? 7 : 10;
    const int iconSz = compact3 ? 18 : compact ? 30 : 44;

    // NOW card — current reading, so no forecast rain %.
    QWidget* nowCard = new QWidget;
    QHBoxLayout* nh = new QHBoxLayout(nowCard);
    nh->setContentsMargins(padX, padY, padX, padY);
    nh->setSpacing(compact3 ? 6 : gap);

    QLabel* nowCap = new QLabel("NOW");
    QFont nowCapF; nowCapF.setPointSize(7); nowCapF.setBold(true);
    nowCap->setFont(nowCapF);
    nowCap->setForegroundRole(QPalette::PlaceholderText);

    sp_weatherNow = new QLabel("—");
    QFont wnf; wnf.setPointSize(compact && !compact2 ? 10 : 9); wnf.setBold(true);
    sp_weatherNow->setFont(wnf);

    if (compact2) {
        // Single row: caption (left) · status (centre). No icon, no percentage.
        nh->addWidget(nowCap);
        nh->addStretch();
        nh->addWidget(sp_weatherNow);
        nh->addStretch();
    } else if (compact3) {
        // Single row: icon + weather (left) · NOW (right).
        sp_weatherNowIcon = new QLabel;
        sp_weatherNowIcon->setFixedSize(iconSz, iconSz);
        sp_weatherNowIcon->setAlignment(Qt::AlignCenter);

        nh->addWidget(sp_weatherNowIcon);
        nh->addWidget(sp_weatherNow);
        nh->addStretch();
        nh->addWidget(nowCap);
    } else {
        sp_weatherNowIcon = new QLabel;
        sp_weatherNowIcon->setFixedSize(iconSz, iconSz);
        sp_weatherNowIcon->setAlignment(Qt::AlignCenter);

        if (compact) {
            nh->addWidget(nowCap);
            nh->addStretch();
            nh->addWidget(sp_weatherNowIcon);
            nh->addWidget(sp_weatherNow);
            nh->addStretch();
        } else {
            QWidget* nowInfo = new QWidget;
            QVBoxLayout* niv = new QVBoxLayout(nowInfo);
            niv->setContentsMargins(0, 0, 0, 0);
            niv->setSpacing(1);
            niv->addStretch();
            niv->addWidget(nowCap);
            niv->addWidget(sp_weatherNow);
            niv->addStretch();

            nh->addStretch();
            nh->addWidget(sp_weatherNowIcon);
            nh->addWidget(nowInfo);
            nh->addStretch();
        }
    }
    wh->addWidget(nowCard, 1);
    wh->addWidget(tnrui::vline());

    for (int i = 0; i < 5; ++i) {
        if (i > 0) wh->addWidget(tnrui::vline());   // separator between forecast cards

        QWidget* fcCard = new QWidget;
        QHBoxLayout* fh = new QHBoxLayout(fcCard);
        fh->setContentsMargins(padX, padY, padX, padY);
        fh->setSpacing(compact3 ? 6 : gap);

        sp_fcTime[i] = new QLabel("");
        QFont ftf; ftf.setPointSize(compact && !compact2 && !compact3 ? 9 : 7);
        sp_fcTime[i]->setFont(ftf);
        sp_fcTime[i]->setForegroundRole(QPalette::PlaceholderText);

        sp_fcWeather[i] = new QLabel("");
        QFont fwf; fwf.setPointSize(compact && !compact2 && !compact3 ? 10 : 9); fwf.setBold(true);
        sp_fcWeather[i]->setFont(fwf);

        sp_fcRain[i] = new QLabel("");
        QFont frf; frf.setPointSize(compact && !compact2 && !compact3 ? 9 : 8);
        if (compact3) frf.setBold(true);
        sp_fcRain[i]->setFont(frf);
        sp_fcRain[i]->setStyleSheet("color:#5794F2;");

        if (compact2) {
            // Single row: time (left) · weather in icon colour (centre) · rain % (right).
            fh->addWidget(sp_fcTime[i]);
            fh->addStretch();
            fh->addWidget(sp_fcWeather[i]);
            fh->addStretch();
            fh->addWidget(sp_fcRain[i]);
        } else if (compact3) {
            // Single row: icon + weather (left) · time + rain % (right).
            sp_fcIcon[i] = new QLabel;
            sp_fcIcon[i]->setFixedSize(iconSz, iconSz);
            sp_fcIcon[i]->setAlignment(Qt::AlignCenter);

            fh->addWidget(sp_fcIcon[i]);
            fh->addWidget(sp_fcWeather[i]);
            fh->addStretch();
            fh->addWidget(sp_fcTime[i]);
            fh->addWidget(sp_fcRain[i]);
        } else {
            sp_fcIcon[i] = new QLabel;
            sp_fcIcon[i]->setFixedSize(iconSz, iconSz);
            sp_fcIcon[i]->setAlignment(Qt::AlignCenter);

            if (compact) {
                fh->addWidget(sp_fcTime[i]);
                fh->addStretch();
                fh->addWidget(sp_fcIcon[i]);
                fh->addWidget(sp_fcWeather[i]);
                fh->addStretch();
                fh->addWidget(sp_fcRain[i]);
            } else {
                // Three stacked rows — time offset, weather name, rain %.
                QWidget* info = new QWidget;
                QVBoxLayout* iv = new QVBoxLayout(info);
                iv->setContentsMargins(0, 0, 0, 0);
                iv->setSpacing(1);
                iv->addStretch();
                iv->addWidget(sp_fcTime[i]);
                iv->addWidget(sp_fcWeather[i]);
                iv->addWidget(sp_fcRain[i]);
                iv->addStretch();

                fh->addStretch();
                fh->addWidget(sp_fcIcon[i]);
                fh->addWidget(info);
                fh->addStretch();
            }
        }
        wh->addWidget(fcCard, 1);
    }
}

// Live per-section compact toggles. Each rebuilds only its part; MainWindow re-feeds
// the latest session row so the fresh labels repaint (see MainWindow::setCompactSection).
void SessionPage::setCardsCompact(bool on) {
    if (cardsCompact_ == on) return;
    cardsCompact_ = on;
    buildSessionCards();
}

void SessionPage::setWeatherCompactLevel(int level) {
    level = qBound(0, level, 3);
    if (weatherCompactLevel_ == level) return;
    weatherCompactLevel_ = level;
    buildWeatherStrip();
}

void SessionPage::setHeaderCompactLevel(int level) {
    level = qBound(0, level, 2);
    if (headerCompactLevel_ == level) return;
    headerCompactLevel_ = level;
    buildHeader();
}

void SessionPage::setEventsCompact(bool on) {
    if (eventsCompact_ == on) return;
    eventsCompact_ = on;
    if (sp_eventsHeader) {
        if (on) {
            sp_eventsHeader->setFixedHeight(32);
            sp_eventsHeader->setContentsMargins(14, 0, 14, 0);
            sp_eventsHeader->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        } else {
            sp_eventsHeader->setMinimumHeight(0);
            sp_eventsHeader->setMaximumHeight(QWIDGETSIZE_MAX);
            sp_eventsHeader->setContentsMargins(14, 0, 14, 0);
            sp_eventsHeader->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        }
    }
}

void SessionPage::setProximityCompact(bool on) {
    if (proximityCompact_ == on) return;
    proximityCompact_ = on;
    if (sp_proxHeader) {
        if (on) {
            sp_proxHeader->setFixedHeight(32);
            sp_proxHeader->setContentsMargins(14, 0, 14, 0);
            sp_proxHeader->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        } else {
            sp_proxHeader->setMinimumHeight(0);
            sp_proxHeader->setMaximumHeight(QWIDGETSIZE_MAX);
            sp_proxHeader->setContentsMargins(14, 10, 14, 0);
            sp_proxHeader->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (sp_proxRow[i]) {
            if (on) {
                sp_proxRow[i]->setFixedHeight(32);
                if (auto* ph = sp_proxRow[i]->layout()) ph->setContentsMargins(14, 0, 14, 0);
            } else {
                sp_proxRow[i]->setMinimumHeight(0);
                sp_proxRow[i]->setMaximumHeight(QWIDGETSIZE_MAX);
                if (auto* ph = sp_proxRow[i]->layout()) ph->setContentsMargins(14, 4, 14, 4);
            }
        }
        if (sp_proxPos[i]) {
            QFont pf = sp_proxPos[i]->font(); pf.setPointSize(8); sp_proxPos[i]->setFont(pf);
        }
        if (sp_proxName[i]) {
            QFont nmf = sp_proxName[i]->font(); nmf.setPointSize(on ? 9 : 10); sp_proxName[i]->setFont(nmf);
        }
        if (sp_proxGap[i]) {
            QFont gf = sp_proxGap[i]->font(); gf.setPointSize(8); sp_proxGap[i]->setFont(gf);
        }
    }
}

// ── Session page updater ──────────────────────────────────────────────────

void SessionPage::updateSession(const tnrp::SessionRow* session, const TimingRow* timing) {
    if (!sp_gpName || !session) return;

    int trackId   = session->track_id;
    int timeLeft  = session->session_time_left;
    int totalLaps = session->total_laps;
    int pitSpeed  = session->pit_speed_limit;
    int idealLap  = session->pit_stop_window_ideal_lap;
    int latestLap = session->pit_stop_window_latest_lap;
    int rejoin    = session->pit_stop_rejoin_position;
    int weather   = session->weather;
    int trackTemp = session->track_temp;
    int airTemp   = session->air_temp;
    int trackLenM = session->track_length_m;
    uint32_t todS = (uint32_t)session->time_of_day;

    // The bundled map JSON is the default source for both display names. The
    // library catalog contains only per-format overrides for names that change
    // between game years (track.<id>.track_name / circuit_name).
    if (trackMap_ && trackId >= 0 && trackId != mapTrackId_) {
        trackMap_->setTrack(trackId);
        mapTrackId_ = trackId;
    }
    const auto resolvedMapName = [trackId](const char* field, const QString& fallback) {
        const QString key = QString("track.%1.%2").arg(trackId).arg(field);
        const QString overridden = tnr::L(key);
        return overridden == key ? fallback : overridden;
    };
    const QString mapTrackName = trackMap_ && trackMap_->hasTrack()
        ? trackMap_->trackName() : QStringLiteral("Grand Prix");
    const QString mapCircuitName = trackMap_ && trackMap_->hasTrack()
        ? trackMap_->circuitName() : QStringLiteral("—");
    sp_gpName->setText(resolvedMapName("track_name", mapTrackName));
    if (sp_circuitName)
        sp_circuitName->setText(resolvedMapName("circuit_name", mapCircuitName));

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
    // Compact 1 uses the standard card text colour beside its tinted icon.
    // Compact 2 has no icon, so the name carries the weather tint instead.
    if (weatherCompactLevel_ == 2) sp_weatherNow->setStyleSheet("color:" + weatherColor(weather).name() + ";");
    applyWeatherIcon(sp_weatherNowIcon, weather, weatherCompactLevel_ == 3 ? 18 : weatherCompactLevel_ == 1 ? 26 : 40);

    {
        const auto& fc = session->weather_forecast_samples;
        int count = std::min((int)fc.size(), 5);
        for (int i = 0; i < 5; ++i) {
            if (i < count) {
                const int fw = fc[i].weather;
                sp_fcTime[i]->setText(QString("+%1m").arg(fc[i].time_offset));
                sp_fcWeather[i]->setText(weatherLabel(fw));
                if (weatherCompactLevel_ == 2) sp_fcWeather[i]->setStyleSheet("color:" + weatherColor(fw).name() + ";");
                applyWeatherIcon(sp_fcIcon[i], fw, weatherCompactLevel_ == 3 ? 18 : weatherCompactLevel_ == 1 ? 26 : 40);
                int rain = fc[i].rain_percentage;
                sp_fcRain[i]->setText(rain > 0 ? QString("%1%").arg(rain) : "");
            } else {
                sp_fcTime[i]->setText("");
                sp_fcWeather[i]->setText("");
                if (sp_fcIcon[i]) sp_fcIcon[i]->clear();
                sp_fcRain[i]->setText("");
            }
        }
    }

    {
        auto* ms = static_cast<MarshalStripWidget*>(sp_marshalStrip);
        ms->zones.clear();
        for (const tnrp::MarshalZone& z : session->marshal_zones) {
            if (z.flag != -1) {
                ms->zones.push_back({(float)z.zone_start, z.flag});
            }
        }
        ms->update();
    }

    if (timing) {
        for (const TimingCar& car : timing->cars) {
            if (car.idx == timing->player_idx) {
                int lap = car.lap_num;
                sp_statRemain->setText((totalLaps > 0 && lap > 0)
                    ? QString::number(totalLaps - lap + 1) : "—");
                break;
            }
        }
    }
}

// ── Session events updater ────────────────────────────────────────────────

void SessionPage::updateEvents(const tnrp::ParticipantsRow* participants) {
    if (!sp_eventsList || eventLog_.empty()) return;

    sp_eventsList->clear();

    auto get3LetterCode = [&](int carIdx) -> QString {
        if (carIdx < 0 || !participants) return "—";
        for (const tnrp::Driver& d : participants->drivers) {
            if (d.idx == carIdx) {
                QString qn = QString::fromStdString(d.name);
                QStringList parts = qn.split(' ');
                QString last = parts.size() > 1 ? parts.last() : qn;
                return last.left(3).toUpper();
            }
        }
        return "—";
    };

    for (int i = (int)eventLog_.size() - 1; i >= 0; --i) {
        const tnrp::RaceEventRow& ev = eventLog_[i];
        const std::string& code = ev.code;

        int totalSecs = (int)ev.session_time;
        QString timeStr = QString("%1:%2")
            .arg(totalSecs / 60, 2, 10, QChar('0'))
            .arg(totalSecs % 60, 2, 10, QChar('0'));

        QString eventType;
        QString text;
        QColor colorOverride;

        if (code == "FTLP") {
            eventType = "Fastest Lap";
            float lapS = ev.lap_time_s.value_or(0.0f);
            int lapMs  = (int)(lapS * 1000.0f);
            QString lapTimeStr = QString("%1:%2.%3")
                .arg(lapMs / 60000)
                .arg((lapMs % 60000) / 1000, 2, 10, QChar('0'))
                .arg(lapMs % 1000, 3, 10, QChar('0'));
            QString nameCode = get3LetterCode(ev.car_idx.value_or(-1));
            text = nameCode + " - " + lapTimeStr;
            colorOverride = QColor("#BF5FFF"); // Purple
        } else if (code == "PENA") {
            int pt = ev.penalty_type.value_or(-1);
            const char* ptLabel = penaltyTypeLabel(pt);
            if (!ptLabel) continue;

            QString nameCode = get3LetterCode(ev.car_idx.value_or(-1));
            QString inf = infringementLabel(ev.infringement_type.value_or(-1));

            if (pt == 5) {
                eventType = "Warning";
                text = nameCode + " - " + (inf.isEmpty() ? "Warning" : inf);
                colorOverride = QColor("#ffd700"); // Yellow
            } else {
                eventType = "Penalty";
                QString penText = ptLabel;
                int timeS = ev.penalty_time_s.value_or(0);
                if ((pt == 1 || pt == 4) && timeS > 0) penText += QString(" %1s").arg(timeS);
                text = nameCode + " - " + penText;
                if (!inf.isEmpty()) text += " (" + inf + ")";
                colorOverride = QColor("#e10600"); // Red
            }
        } else if (code == "SCAR") {
            int t = ev.safety_car_type.value_or(0);
            eventType = (t == 1) ? "Safety Car" : (t == 2) ? "Virtual SC"
                         : (t == 3) ? "Formation Lap" : "SC";
            int a = ev.event_type.value_or(0);
            const char* action = (a == 0) ? "Deployed" : (a == 1) ? "Returning"
                               : (a == 2) ? "Returned" : (a == 3) ? "Resume Race" : "";
            text = action;
        } else if (code == "RTMT" || code == "RCWN" || code == "DTSV" || code == "SGSV") {
            eventType = eventCodeLabel(code);
            QString nameCode = get3LetterCode(ev.car_idx.value_or(-1));
            text = nameCode;
        } else {
            eventType = eventCodeLabel(code);
            text = "";
        }

        QColor c = colorOverride.isValid() ? colorOverride : eventCodeColor(code);
        if (!c.isValid()) c = QColor("#c8ccd4");

        if (eventsCompact_) {
            const int hPad = 8;
            QWidget* rowW = new QWidget;
            rowW->setObjectName("eventRow");
            rowW->setFixedHeight(32);
            rowW->setStyleSheet(QString(
                "#eventRow {"
                "  border-left: 3px solid %1;"
                "}"
            ).arg(c.name()));

            QHBoxLayout* hl = new QHBoxLayout(rowW);
            hl->setContentsMargins(hPad, 0, hPad, 0);
            hl->setSpacing(6);

            QString fullText = !text.isEmpty() ? (eventType + " – " + text) : eventType;
            QLabel* descLbl = new QLabel(fullText);
            QFont df; df.setPointSize(8); df.setBold(true);
            descLbl->setFont(df);
            descLbl->setStyleSheet("color: " + c.name() + "; background: transparent;");
            descLbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

            QLabel* timeLbl = new QLabel(timeStr);
            QFont tf; tf.setPointSize(7); tf.setBold(true);
            tf.setStyleHint(QFont::Monospace); tf.setFamily("monospace");
            timeLbl->setFont(tf);
            timeLbl->setStyleSheet("color: #a0a8b8;");
            timeLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

            hl->addWidget(descLbl, 1);
            hl->addWidget(timeLbl);

            const int rowH = 32;

            auto* item = new QListWidgetItem;
            item->setSizeHint(QSize(avail, rowH));
            sp_eventsList->addItem(item);
            sp_eventsList->setItemWidget(item, rowW);
        } else {
            const int hPad = 8, vPad = 6, gap = 2;
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
}

// ── Proximity widget updater ──────────────────────────────────────────────

void SessionPage::updateProximity(const TimingRow* timing, const tnrp::ParticipantsRow* participants) {
    if (!sp_proxPos[0] || !timing) return;

    int playerIdx = timing->player_idx;
    if (playerIdx < 0) {
        for (int i = 0; i < 3; ++i) sp_proxRow[i]->setVisible(false);
        return;
    }

    struct CarEntry { int idx; int pos; int gapMs; };
    std::vector<CarEntry> cars;
    for (const TimingCar& car : timing->cars) {
        if (car.result_status == 0 || car.result_status == 3) continue;
        cars.push_back({car.idx, car.position, car.gap_ms});
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
        if (carIdx < 0 || !participants) return "—";
        for (const tnrp::Driver& d : participants->drivers) {
            if (d.idx == carIdx) {
                QString qn = QString::fromStdString(d.name);
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

void SessionPage::updateTrackMap(const tnrp::SessionRow* session,
                                 const tnrp::ParticipantsRow* participants,
                                 const PositionsRow* positions) {
    if (!trackMap_) return;

    // Load the circuit geometry when the track changes.
    if (session) {
        int tid = session->track_id;
        if (tid >= 0 && tid != mapTrackId_) {
            trackMap_->setTrack(tid);
            mapTrackId_ = tid;
        }
        // 2026 SLM track status (0 = Full, 1 = Partial, -1 = n/a) selects which
        // SLM zone set the overlay draws. Absent on F1 24/25 sessions.
        trackMap_->setSlmTrackStatus(session->active_aero_track_status);
    }

    // Theme: derive light/dark from the active palette.
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    trackMap_->setDark(dark);
    trackMap_->setLabelMode(static_cast<TrackMapWidget::LabelMode>(
        settings_.value("ui/trackMapLabelMode", 0).toInt()));
    trackMap_->setSectorColors(settings_.value("ui/trackMapSectorColors", true).toBool());
    trackMap_->setMapOpacity(settings_.value("ui/trackMapOpacity", 100).toInt() / 100.0);
    trackMap_->setIdleTimeout(settings_.value("ui/trackMapIdleTimeout", 0).toInt());

    if (participants)
        trackMap_->setParticipants(*participants);
    if (positions)
        trackMap_->setPositions(*positions);
}

void SessionPage::setMapFullscreen(bool on) {
    if (mapFullscreen_ == on) return;
    mapFullscreen_ = on;
    // Hide every sibling so the map's left area (and the map within it) expands to
    // fill the whole session view; restore them on exit according to layout_.
    if (on) {
        for (QWidget* w : mapFsHide_)
            if (w) w->setVisible(false);
    } else {
        applyLayout(layout_);
    }
    if (trackMap_) trackMap_->setFullscreenState(on);
}

SessionLayout SessionPage::loadLayout() {
    SessionLayout L;
    settings_.beginGroup("sessionLayout");
    L.showGpName       = settings_.value("showGpName", true).toBool();
    L.showMarshalZones = settings_.value("showMarshalZones", true).toBool();
    L.showTimeLeft     = settings_.value("showTimeLeft", true).toBool();
    L.showMap          = settings_.value("showMap", true).toBool();
    L.showProximity    = settings_.value("showProximity", true).toBool();
    L.showEvents       = settings_.value("showEvents", true).toBool();
    L.showWeather      = settings_.value("showWeather", true).toBool();
    L.sidebarPct       = qBound(15, settings_.value("sidebarPct", 28).toInt(), 60);
    for (int i = 0; i < SessionLayout::StatCardCount; ++i) {
        L.cards[i] = settings_.value(SessionLayout::cardKey(i), true).toBool();
    }
    settings_.endGroup();
    return L;
}

void SessionPage::saveLayout(const SessionLayout& L) {
    settings_.beginGroup("sessionLayout");
    settings_.setValue("showGpName", L.showGpName);
    settings_.setValue("showMarshalZones", L.showMarshalZones);
    settings_.setValue("showTimeLeft", L.showTimeLeft);
    settings_.setValue("showMap", L.showMap);
    settings_.setValue("showProximity", L.showProximity);
    settings_.setValue("showEvents", L.showEvents);
    settings_.setValue("showWeather", L.showWeather);
    settings_.setValue("sidebarPct", L.sidebarPct);
    for (int i = 0; i < SessionLayout::StatCardCount; ++i) {
        settings_.setValue(SessionLayout::cardKey(i), L.cards[i]);
    }
    settings_.endGroup();
}

void SessionPage::applyLayout(const SessionLayout& L) {
    layout_ = L;
    if (mapFullscreen_) return;

    bool anyHeader = L.showHeader();
    if (sp_header_) sp_header_->setVisible(anyHeader);
    if (sp_headerSep_) sp_headerSep_->setVisible(anyHeader);

    if (sp_gpBlock_) sp_gpBlock_->setVisible(L.showGpName);
    if (sp_zoneBlock_) sp_zoneBlock_->setVisible(L.showMarshalZones);
    if (sp_tmBlock_) sp_tmBlock_->setVisible(L.showTimeLeft);

    if (sp_headerDiv1_) sp_headerDiv1_->setVisible(L.showGpName && (L.showMarshalZones || L.showTimeLeft));
    if (sp_headerDiv2_) sp_headerDiv2_->setVisible(L.showMarshalZones && L.showTimeLeft);

    bool anyCard = false;
    for (int i = 0; i < SessionLayout::StatCardCount; ++i) {
        if (sp_statCardFrames_[i]) sp_statCardFrames_[i]->setVisible(L.cards[i]);
        anyCard = anyCard || L.cards[i];
    }
    int lastVisibleIdx = -1;
    for (int i = 0; i < SessionLayout::StatCardCount; ++i) {
        if (i > 0 && sp_statCardDivs_[i - 1]) {
            sp_statCardDivs_[i - 1]->setVisible(false);
        }
        if (L.cards[i]) {
            if (lastVisibleIdx != -1 && i > 0) {
                if (sp_statCardDivs_[i - 1]) sp_statCardDivs_[i - 1]->setVisible(true);
            }
            lastVisibleIdx = i;
        }
    }
    if (spStatsRow_) spStatsRow_->setVisible(anyCard);
    if (sp_statsSep_) sp_statsSep_->setVisible(anyCard);

    if (trackMap_) trackMap_->setVisible(L.showMap);
    if (sp_weatherStrip_) sp_weatherStrip_->setVisible(L.showWeather);
    if (sp_weatherSep_) sp_weatherSep_->setVisible(L.showWeather && L.showMap);
    if (leftArea_) leftArea_->setVisible(L.showMap || L.showWeather);

    if (sp_proxHeader) sp_proxHeader->setVisible(L.showProximity);
    for (int i = 0; i < 3; ++i) {
        if (sp_proxRow[i]) sp_proxRow[i]->setVisible(L.showProximity);
    }
    if (sp_proxSep_) sp_proxSep_->setVisible(L.showProximity && L.showEvents);

    if (sp_eventsHeader) sp_eventsHeader->setVisible(L.showEvents);
    if (sp_eventsList) sp_eventsList->setVisible(L.showEvents);

    if (rightPanel_) {
        rightPanel_->setVisible(L.showProximity || L.showEvents);
        if (leftArea_ && (L.showMap || L.showWeather) && (L.showProximity || L.showEvents)) {
            int totalW = width() > 0 ? width() : 1000;
            int pw = qBound(180, totalW * L.sidebarPct / 100, 600);
            rightPanel_->setFixedWidth(pw);
        } else {
            rightPanel_->setMinimumWidth(0);
            rightPanel_->setMaximumWidth(QWIDGETSIZE_MAX);
        }
    }
    if (midVLine_) midVLine_->setVisible((L.showMap || L.showWeather) && (L.showProximity || L.showEvents));
}

void SessionPage::applyAndSaveLayout(const SessionLayout& L) {
    applyLayout(L);
    saveLayout(L);
}

