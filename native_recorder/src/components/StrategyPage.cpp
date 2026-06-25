#include "StrategyPage.h"
#include "TyreHelpers.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QBrush>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <utility>
#include <vector>

using nlohmann::json;

// ─── File-local data + helpers ─────────────────────────────────────────────

namespace {

// Reads the circuit's pit-lane time loss from the track map (the same files used for
// map rendering). Returns the 11.25/13.75/25s fallback when no map exists.
PitLoss loadPitLoss(int trackId) {
    PitLoss pl;   // defaults already hold the fallback values
    QFile f(QString(":/maps/track_%1.json").arg(trackId));
    if (!f.open(QIODevice::ReadOnly)) return pl;
    const QByteArray bytes = f.readAll();
    f.close();
    try {
        const json j = json::parse(bytes.constData(), bytes.constData() + bytes.size());
        pl.inlapMs  = j.value("inlap_pit_time",  pl.inlapMs  / 1000.0) * 1000.0;
        pl.outlapMs = j.value("outlap_pit_time", pl.outlapMs / 1000.0) * 1000.0;
        pl.totalMs  = j.value("pit_time",        pl.totalMs  / 1000.0) * 1000.0;
    } catch (...) {}
    return pl;
}

// Domain colours (match the Electron panel + the rest of the native app).
const QColor kBlue ("#5794F2");
const QColor kAmber("#FADE2A");
const QColor kGreen("#73BF69");
const QColor kRed  ("#C4162A");
const QColor kOrange("#FF9830");

struct Set {
    int    idx = -1, actual = 0, visual = 0;
    double wear = 0.0, lap_delta_ms = 0.0;
    int    usable_life = 0;
    bool   available = false, fitted = false;
};

struct Compound { QString name; QColor color; int actual = 0; int visual = 0; };

struct Stint {
    QString compoundName;
    QColor  color;
    int     actual = 0, visual = 0;
    int     lapCount = 0, startLap = 0, pitLap = -1;   // pitLap = -1 when last stint
    bool    isLast = false;
    double  currentWear = -1.0;                         // >=0 only for the opening stint
};

struct StrategyResult { int stops = 0; std::vector<Stint> stints; };

struct StintTarget {
    bool   present = false;
    double targetMs = 0.0;
    bool   isEstimate = false;
    bool   hasDelta = false;
    double deltaMs = 0.0;
};

struct WearWarning { QString text, detail; QColor color; int priority = 3; };

// LapRow / PastStint / DisplayStint live in the header (cached as members).

struct TimingCar {
    int idx = -1, position = 0, driver_status = 0, result_status = 0, pit_status = 0;
    double gap_ms = 0.0;
    int last_lap_ms = 0;
    int num_pit_stops = 0;
};

// wearColor()/fmtLapTime()/driverName() ported from StrategyPanel.tsx
QColor wearColor(double pct) {
    if (pct < 20) return kGreen;
    if (pct < 40) return QColor("#A8D436");
    if (pct < 60) return kAmber;
    if (pct < 80) return kOrange;
    return kRed;
}

QString fmtLapTime(double ms) {
    if (ms <= 0 || ms > 600000) return "—";
    double totalSecs = ms / 1000.0;
    int mins = (int)std::floor(totalSecs / 60.0);
    double secs = totalSecs - mins * 60.0;
    return QString("%1:%2").arg(mins).arg(secs, 6, 'f', 3, QChar('0'));
}

QString driverName(const json& participants, int idx) {
    if (participants.is_object() && participants.contains("drivers")) {
        for (const auto& d : participants["drivers"]) {
            if (d.value("idx", -1) == idx) {
                QString name = QString::fromStdString(d.value("name", std::string()))
                                   .trimmed();
                if (name.isEmpty()) break;
                const QStringList parts = name.split(QRegularExpression("\\s+"),
                                                     Qt::SkipEmptyParts);
                return parts.isEmpty() ? name : parts.last().toUpper();
            }
        }
    }
    return QString("Car %1").arg(idx);
}

QColor liveryColor(const json& participants, int idx) {
    if (participants.is_object() && participants.contains("drivers")) {
        for (const auto& d : participants["drivers"]) {
            if (d.value("idx", -1) == idx) {
                const QString c = QString::fromStdString(d.value("livery_color", std::string()));
                if (!c.isEmpty()) return QColor(c);
            }
        }
    }
    return QColor("#8e8e8e");
}

QColor compoundColor(int visual, const QColor& fallback) {
    const QColor c = tyreTextColor(visual);   // invalid for hard
    return c.isValid() ? c : fallback;
}

std::vector<Set> parseSets(const json& tyreSets) {
    std::vector<Set> out;
    if (tyreSets.is_object() && tyreSets.contains("sets") && tyreSets["sets"].is_array()) {
        for (const auto& s : tyreSets["sets"]) {
            Set e;
            e.idx          = s.value("idx", -1);
            e.actual       = s.value("actual_compound", 0);
            e.visual       = s.value("visual_compound", 0);
            e.wear         = s.value("wear", 0.0);
            e.usable_life  = s.value("usable_life", 0);
            e.lap_delta_ms = s.value("lap_delta_ms", 0.0);
            e.available    = s.value("available", false);
            e.fitted       = s.value("fitted", false);
            out.push_back(e);
        }
    }
    return out;
}

std::vector<TimingCar> parseTiming(const json& timing) {
    std::vector<TimingCar> out;
    if (timing.is_object() && timing.contains("cars") && timing["cars"].is_array()) {
        for (const auto& c : timing["cars"]) {
            TimingCar t;
            t.idx           = c.value("idx", -1);
            t.position      = c.value("position", 0);
            t.driver_status = c.value("driver_status", 0);
            t.result_status = c.value("result_status", 0);
            t.pit_status    = c.value("pit_status", 0);
            t.gap_ms        = c.value("gap_ms", 0.0);
            t.last_lap_ms   = c.value("last_lap_ms", 0);
            t.num_pit_stops = c.value("num_pit_stops", 0);
            out.push_back(t);
        }
    }
    return out;
}

// ── Strategy calculation (ported verbatim from StrategyPanel.tsx) ──────────

StrategyResult buildStints(int lapNum, int totalLaps, int firstPitLap,
                           const Compound& cur, double avgWear,
                           const std::vector<Set>& availableSets,
                           const QColor& fallback) {
    StrategyResult r;
    const int clampedPit = std::min(firstPitLap, totalLaps);
    Stint s0;
    s0.compoundName = cur.name; s0.color = cur.color;
    s0.actual = cur.actual;     s0.visual = cur.visual;
    s0.lapCount = clampedPit - lapNum;
    s0.startLap = lapNum;
    s0.isLast   = clampedPit >= totalLaps;
    s0.pitLap   = s0.isLast ? -1 : clampedPit;
    s0.currentWear = avgWear;
    r.stints.push_back(s0);
    if (s0.isLast) { r.stops = 0; return r; }

    int pitLap = clampedPit;
    int remaining = totalLaps - pitLap;
    for (const auto& set : availableSets) {
        if (remaining <= 0) break;
        const int stintLen = std::min(set.usable_life, remaining);
        const int nextPit   = pitLap + stintLen;
        const bool isLast   = nextPit >= totalLaps;
        Stint s;
        s.compoundName = tyreLabel(set.actual);
        s.color  = compoundColor(set.visual, fallback);
        s.actual = set.actual; s.visual = set.visual;
        s.lapCount = stintLen; s.startLap = pitLap;
        s.isLast = isLast;     s.pitLap = isLast ? -1 : nextPit;
        r.stints.push_back(s);
        remaining -= stintLen;
        pitLap = nextPit;
        if (isLast) break;
    }
    if (remaining > 0 && !r.stints.empty()) {
        Stint& last = r.stints.back();
        last.lapCount += remaining;
        last.pitLap = -1;
        last.isLast = true;
    }
    r.stops = (int)r.stints.size() - 1;
    return r;
}

StrategyResult forceExtraStop(const StrategyResult& result,
                              const std::vector<Set>& availableSets,
                              const std::set<int>& usedActual,
                              const QColor& fallback) {
    if (result.stints.size() < 2) return result;

    int longestIdx = 1;
    for (size_t i = 2; i < result.stints.size(); ++i)
        if (result.stints[i].lapCount > result.stints[longestIdx].lapCount) longestIdx = (int)i;

    const Stint target = result.stints[longestIdx];
    if (target.lapCount < 4) return result;

    const int splitLap = target.startLap + target.lapCount / 2;

    const Set* nextSet = nullptr;
    for (const auto& s : availableSets) if (!usedActual.count(s.actual)) { nextSet = &s; break; }
    if (!nextSet)
        for (const auto& s : availableSets) if (s.actual != target.actual) { nextSet = &s; break; }
    if (!nextSet && !availableSets.empty()) nextSet = &availableSets.front();
    if (!nextSet) return result;

    Stint part1 = target;
    part1.lapCount = splitLap - target.startLap;
    part1.pitLap = splitLap;
    part1.isLast = false;

    const int targetEnd = target.isLast ? (target.startLap + target.lapCount)
                        : (target.pitLap >= 0 ? target.pitLap : target.startLap + target.lapCount);
    Stint part2;
    part2.compoundName = tyreLabel(nextSet->actual);
    part2.color  = compoundColor(nextSet->visual, fallback);
    part2.actual = nextSet->actual; part2.visual = nextSet->visual;
    part2.lapCount = targetEnd - splitLap;
    part2.startLap = splitLap;
    part2.pitLap   = target.pitLap;
    part2.isLast   = target.isLast;

    StrategyResult out;
    out.stints = result.stints;
    out.stints.erase(out.stints.begin() + longestIdx);
    out.stints.insert(out.stints.begin() + longestIdx, part2);
    out.stints.insert(out.stints.begin() + longestIdx, part1);
    out.stops = result.stops + 1;
    return out;
}

StrategyResult applyMonacoRule(const StrategyResult& result,
                               const std::vector<Set>& availableSets,
                               const std::set<int>& usedActual,
                               const QColor& fallback) {
    if (result.stops >= 2) return result;
    return forceExtraStop(result, availableSets, usedActual, fallback);
}

std::set<int> usedCompoundsOf(const StrategyResult& r) {
    std::set<int> s;
    for (const auto& st : r.stints) s.insert(st.actual);
    return s;
}

}  // namespace

// ─── Computed result bundle (lives in the .cpp; built fresh per update) ────

namespace {
struct StrategyData {
    bool   valid = false;
    double avgWear = 0.0, wearPerLap = 0.0;
    // The limiting tyre is the corner that reaches the cliff first; pit timing
    // follows it rather than the average (see update()).
    double  limitWear = 0.0, limitWearPerLap = 0.0;
    QString limitCorner;
    int    estimatedCliffLap = 0, lapsUntilCliff = 0;
    StrategyResult conservative, aggressive;
    Compound currentCompound;
    bool   isMonaco = false;
    std::vector<WearWarning> wearWarnings;
};
}  // namespace

// ─── StrategyPage ──────────────────────────────────────────────────────────

StrategyPage::StrategyPage(QWidget* parent) : QWidget(parent) {
    // Top level is horizontal: a left pane (header + strategy columns) beside a
    // full-height sidebar. The header therefore spans only the columns, and the
    // sidebar runs edge-to-edge from the very top.
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* leftPane = new QWidget;
    auto* lpv = new QVBoxLayout(leftPane);
    lpv->setContentsMargins(0, 0, 0, 0);
    lpv->setSpacing(0);

    auto hline = [](QWidget* p = nullptr) {
        auto* f = new QFrame(p);
        f->setFrameShape(QFrame::HLine);
        f->setFrameShadow(QFrame::Sunken);
        return f;
    };
    auto vline = [](QWidget* p = nullptr) {
        auto* f = new QFrame(p);
        f->setFrameShape(QFrame::VLine);
        f->setFrameShadow(QFrame::Sunken);
        return f;
    };

    // Secondary chrome text uses the palette role so it re-tints automatically on
    // theme change (these header/placeholder labels are built once, never rebuilt).
    auto capLabel = [&](const QString& t) {
        auto* l = new QLabel(t);
        QFont f = l->font(); f.setPointSize(7); f.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
        l->setFont(f);
        l->setForegroundRole(QPalette::PlaceholderText);
        return l;
    };

    // ── Header ────────────────────────────────────────────────────────────
    auto* header = new QWidget;
    header->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);

    // Lap cell
    {
        auto* cell = new QWidget; auto* cv = new QVBoxLayout(cell);
        cv->setContentsMargins(16, 6, 16, 6); cv->setSpacing(1);
        cv->addWidget(capLabel("LAP"));
        auto* row = new QHBoxLayout; row->setContentsMargins(0, 0, 0, 0); row->setSpacing(6);
        lapValue_ = new QLabel("—");
        QFont vf = lapValue_->font(); vf.setPointSize(19); vf.setBold(true); lapValue_->setFont(vf);
        lapTotal_ = new QLabel("/ —");
        lapTotal_->setForegroundRole(QPalette::PlaceholderText);
        row->addWidget(lapValue_); row->addWidget(lapTotal_, 0, Qt::AlignBottom); row->addStretch();
        cv->addLayout(row);
        hl->addWidget(cell);
    }
    hl->addWidget(vline());

    // Compound + wear cell
    {
        auto* cell = new QWidget; auto* cl = new QHBoxLayout(cell);
        cl->setContentsMargins(16, 6, 16, 6); cl->setSpacing(12);
        compoundChip_ = new QLabel("—");
        compoundChip_->setAlignment(Qt::AlignCenter);
        cl->addWidget(compoundChip_, 0, Qt::AlignVCenter);
        auto* col = new QVBoxLayout; col->setContentsMargins(0, 0, 0, 0); col->setSpacing(6);
        auto* top = new QHBoxLayout; top->setContentsMargins(0, 0, 0, 0); top->setSpacing(12);
        wearPct_ = new QLabel("0%");
        QFont wf = wearPct_->font(); wf.setPointSize(11); wf.setBold(true); wearPct_->setFont(wf);
        wearAge_ = new QLabel("—");
        wearAge_->setForegroundRole(QPalette::PlaceholderText);
        top->addWidget(wearPct_); top->addStretch(); top->addWidget(wearAge_);
        col->addLayout(top);
        wearBar_ = new QProgressBar; wearBar_->setRange(0, 100); wearBar_->setValue(0);
        wearBar_->setTextVisible(false); wearBar_->setFixedHeight(6);
        col->addWidget(wearBar_);
        cl->addLayout(col, 1);
        hl->addWidget(cell, 1);
    }
    hl->addWidget(vline());

    // Cliff cell
    {
        auto* cell = new QWidget; auto* cv = new QVBoxLayout(cell);
        cv->setContentsMargins(16, 6, 16, 6); cv->setSpacing(1);
        cv->addWidget(capLabel("TYRE CLIFF"));
        auto* row = new QHBoxLayout; row->setContentsMargins(0, 0, 0, 0); row->setSpacing(6);
        cliffValue_ = new QLabel("—");
        QFont cf = cliffValue_->font(); cf.setPointSize(11); cf.setBold(true); cliffValue_->setFont(cf);
        cliffPlus_ = new QLabel("");
        cliffPlus_->setForegroundRole(QPalette::PlaceholderText);
        row->addWidget(cliffValue_); row->addWidget(cliffPlus_, 0, Qt::AlignBottom); row->addStretch();
        cv->addLayout(row);
        hl->addWidget(cell);
    }

    lpv->addWidget(header);
    lpv->addWidget(hline());

    // ── Left pane body: strategy columns vs non-race placeholder ───────────
    bodyStack_ = new QStackedWidget;

    // Page 0: the two strategy columns
    auto* cols = new QWidget;
    auto* bl = new QHBoxLayout(cols);
    bl->setContentsMargins(0, 0, 0, 0); bl->setSpacing(0);

    auto makeColumn = [&](QVBoxLayout*& outHeaderLayout, QVBoxLayout*& outLayout,
                          QScrollArea*& outScroll) {
        auto* col = new QWidget;
        auto* cv = new QVBoxLayout(col);
        cv->setContentsMargins(0, 0, 0, 0); cv->setSpacing(0);

        // Fixed (non-scrolling) header bar — the strategy name + stop count stays put
        // while the stint tables scroll beneath it.
        auto* headerHolder = new QWidget;
        outHeaderLayout = new QVBoxLayout(headerHolder);
        outHeaderLayout->setContentsMargins(0, 0, 0, 0);
        outHeaderLayout->setSpacing(0);
        cv->addWidget(headerHolder);
        cv->addWidget(hline());

        auto* scroll = new QScrollArea;
        outScroll = scroll;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // Keep the vertical scrollbar always present: the column's height changes as
        // stints complete/laps advance, so an as-needed bar would toggle on/off (and
        // flicker badly while scrubbing). Always-on reserves the gutter and is stable.
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        auto* content = new QWidget;
        outLayout = new QVBoxLayout(content);
        outLayout->setContentsMargins(0, 0, 0, 0);
        outLayout->setSpacing(0);
        outLayout->addStretch();
        scroll->setWidget(content);
        cv->addWidget(scroll, 1);
        return col;
    };

    bl->addWidget(makeColumn(consHeaderLayout_, consLayout_, consScroll_), 1);
    bl->addWidget(vline());
    bl->addWidget(makeColumn(aggHeaderLayout_, aggLayout_, aggScroll_), 1);
    bodyStack_->addWidget(cols);   // index 0

    // Page 1: non-race placeholder
    auto* placeholder = new QWidget;
    auto* pl = new QVBoxLayout(placeholder);
    pl->setAlignment(Qt::AlignCenter); pl->setSpacing(8);
    auto* pCap = capLabel("STRATEGY"); pCap->setAlignment(Qt::AlignCenter);
    auto* pTitle = new QLabel("Race sessions only"); pTitle->setAlignment(Qt::AlignCenter);
    { QFont f = pTitle->font(); f.setPointSize(11); f.setBold(true); pTitle->setFont(f); }
    auto* pBody = new QLabel("Strategy suggestions are available during Race, Race 2, and Race 3 sessions.");
    pBody->setAlignment(Qt::AlignCenter); pBody->setWordWrap(true); pBody->setMaximumWidth(320);
    pBody->setForegroundRole(QPalette::PlaceholderText);
    pl->addWidget(pCap); pl->addWidget(pTitle); pl->addWidget(pBody);
    bodyStack_->addWidget(placeholder);   // index 1

    lpv->addWidget(bodyStack_, 1);

    // ── Full-height sidebar (right column) ─────────────────────────────────
    auto* sidebar = new QScrollArea;
    sidebar->setWidgetResizable(true);
    sidebar->setFrameShape(QFrame::NoFrame);
    sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebar->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // Reserve the vertical scrollbar's gutter once, up-front: the outer width
    // includes it but the content is capped to the base width, so the scrollbar
    // appears within the reserved space — no layout shift when it shows/hides.
    {
        const int sbw = sidebar->verticalScrollBar()->sizeHint().width();
        sidebar->setFixedWidth(230 + sbw);
        auto* content = new QWidget;
        content->setMaximumWidth(230);
        sidebarLayout_ = new QVBoxLayout(content);
        sidebarLayout_->setContentsMargins(0, 0, 0, 0);
        sidebarLayout_->setSpacing(0);
        sidebarLayout_->addStretch();
        sidebar->setWidget(content);
    }
    sidebarScroll_ = sidebar;
    sidebarSep_ = vline();

    root->addWidget(leftPane, 1);
    root->addWidget(sidebarSep_);
    root->addWidget(sidebarScroll_);
}

void StrategyPage::resetForNewSession() {
    rivalAheadIdx_ = rivalBehindIdx_ = -1;
    rivalsLatched_ = false;
    prevLapNum_ = -1;
    consFrozenReqMs_.clear();
    aggFrozenReqMs_.clear();
    consPast_.clear();
    aggPast_.clear();
    consDisplay_.clear();
    aggDisplay_.clear();
    consRendered_.clear();
    aggRendered_.clear();
    consStopsShown_ = aggStopsShown_ = -1;
    monacoShown_ = false;
    tableLapComputed_ = -1;
    stratBuilt_ = false;
    stratShowingTimeline_ = false;
    everStratValid_ = false;
    hasPrevAheadGap_ = hasPrevBehindGap_ = false;
    prevAheadGap_ = prevBehindGap_ = 0.0;
    playerGainingAhead_ = behindIsGaining_ = -1;
}

// ─── Small UI builders (members so they can read palette()) ────────────────

namespace {

QFrame* makeHLine(QWidget* parent = nullptr) {
    auto* f = new QFrame(parent);
    f->setFrameShape(QFrame::HLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

QString rgba(const QColor& c, double a) {
    return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(a, 0, 'f', 3);
}

// Compound/strategy chip: coloured text + border + faint fill.
QLabel* makeChip(const QString& text, const QColor& color, int fontPt = 9) {
    auto* l = new QLabel(text);
    l->setAlignment(Qt::AlignCenter);
    l->setStyleSheet(QString(
        "QLabel{ color:%1; border:1px solid %1; border-radius:4px;"
        " padding:1px 6px; font-weight:bold; font-size:%2px; background:%3; }")
        .arg(color.name()).arg(fontPt).arg(rgba(color, 0.1)));
    return l;
}

QLabel* secLabel(const QString& t, const QColor& sec, int pt = -1, bool bold = false) {
    auto* l = new QLabel(t);
    QFont f = l->font(); if (pt > 0) f.setPointSize(pt); f.setBold(bold); l->setFont(f);
    QPalette p = l->palette(); p.setColor(QPalette::WindowText, sec); l->setPalette(p);
    return l;
}

QLabel* sectionHeader(const QString& t, const QColor& sec) {
    auto* l = new QLabel(t.toUpper());
    QFont f = l->font(); f.setPointSize(7); f.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    l->setFont(f);
    QPalette p = l->palette(); p.setColor(QPalette::WindowText, sec); l->setPalette(p);
    l->setContentsMargins(12, 6, 12, 3);
    return l;
}

void clearColumn(QVBoxLayout* lay) {
    // Remove everything but keep one trailing stretch.
    while (lay->count() > 0) {
        QLayoutItem* it = lay->takeAt(0);
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }
}

}  // namespace

// Seconds with up to 3 decimals, trailing zeros trimmed, no unit (e.g. "0.3", "2.456").
static QString fmtSecs(double ms) {
    QString s = QString::number(std::abs(ms) / 1000.0, 'f', 3);
    if (s.contains('.')) { while (s.endsWith('0')) s.chop(1); if (s.endsWith('.')) s.chop(1); }
    return s;
}

// Signed delta text (+slower / −faster), matching the stint-target delta styling.
static QString fmtDelta(double ms) {
    return (ms > 0 ? QString("+") : QString("−")) + fmtSecs(ms);
}

// Row heights, measured once from representative components (so they honour the OS
// font/style — nothing hardcoded), then enforced on every row. With these known the
// whole column height is computed up-front, removing the dependence on the tables
// laying out (which was provisional pre-show and caused the intermittent collapse).
namespace {
struct ColMetrics { int heading = 0, stintHead = 0, tableRow = 0, tableHeader = 0, sep = 0; };

const ColMetrics& colMetrics() {
    static const ColMetrics m = [] {
        ColMetrics r;
        {   // table row + header, straight from a table configured like the live ones
            QTableWidget t(0, 7);
            t.verticalHeader()->setVisible(false);
            t.ensurePolished();
            r.tableRow    = t.verticalHeader()->defaultSectionSize();
            r.tableHeader = t.horizontalHeader()->sizeHint().height();
        }
        {   // strategy heading row — the 14pt bold number drives its height
            QWidget row; auto* l = new QHBoxLayout(&row);
            l->setContentsMargins(12, 5, 12, 5); l->setSpacing(8);
            auto* big = new QLabel("0"); QFont f = big->font(); f.setPointSize(14); f.setBold(true); big->setFont(f);
            l->addWidget(big);
            row.ensurePolished();
            r.heading = row.sizeHint().height();
        }
        {   // stint header row — the compound chip drives its height
            QWidget row; auto* l = new QHBoxLayout(&row);
            l->setContentsMargins(12, 6, 12, 6); l->setSpacing(8);
            l->addWidget(makeChip("C3", kRed, 10));
            row.ensurePolished();
            r.stintHead = row.sizeHint().height();
        }
        {   // separator line
            auto* ln = makeHLine();
            ln->ensurePolished();
            r.sep = std::max(1, ln->sizeHint().height());
            delete ln;
        }
        return r;
    }();
    return m;
}
}  // namespace

// Fixed (non-scrolling) header bar for one column: strategy name + Monaco chip +
// stop count. Built separately from the timeline so it stays put while the stint
// tables scroll beneath it.
static QWidget* buildStintHeaderBar(int stops, bool isMonaco, const QString& label,
                                    const QColor& accent, const QColor& sec) {
    const ColMetrics& M = colMetrics();
    auto* hdr = new QWidget; hdr->setFixedHeight(M.heading);
    auto* hl = new QHBoxLayout(hdr);
    hl->setContentsMargins(12, 5, 12, 5); hl->setSpacing(8);
    hl->addWidget(secLabel(label.toUpper(), sec, 7));
    if (isMonaco) hl->addWidget(makeChip("Monaco 2-stop", kAmber));
    hl->addStretch();
    auto* stopsLbl = new QLabel(QString::number(stops));
    { QFont f = stopsLbl->font(); f.setPointSize(14); f.setBold(true); stopsLbl->setFont(f);
      QPalette p = stopsLbl->palette(); p.setColor(QPalette::WindowText, accent); stopsLbl->setPalette(p); }
    hl->addWidget(stopsLbl, 0, Qt::AlignBottom);
    hl->addWidget(secLabel(stops == 1 ? "stop" : "stops", sec), 0, Qt::AlignBottom);
    return hdr;
}

// Exact pixel height of one stint group (optional top separator + header + table),
// computed purely from the measured ColMetrics — never from a widget's sizeHint.
// The column's content height is summed from this so the scroll range is a value
// we control, not something QTableWidget's (wrong) sizeHint can collapse.
static int stintGroupHeight(const DisplayStint& st, bool withTopSep) {
    const ColMetrics& M = colMetrics();
    int h = (withTopSep ? M.sep : 0) + M.stintHead;
    if (!st.rows.empty()) h += M.tableHeader + M.tableRow * (int)st.rows.size();
    return h;
}

// Build the widget for a single stint: an optional top separator, the one-line
// header (compound · laps · range · wear · target) and, when present, the per-lap
// target table. Self-contained and fixed-height so the column reconciler can drop
// it in or out without disturbing its neighbours — which is what keeps the
// scrollbar from churning when only one stint changes.
static QWidget* buildStintGroup(const QColor& sec, const DisplayStint& st, bool withTopSep) {
    const ColMetrics& M = colMetrics();
    auto* g = new QWidget;
    auto* v = new QVBoxLayout(g);
    v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);

    // Separator above every stint but the first (kept inside the group so it is
    // added/removed together with the stint it precedes).
    if (withTopSep) {
        auto* ln = makeHLine(); ln->setFixedHeight(M.sep);
        v->addWidget(ln);
    }

    // Single header row. Left: the tyre choice (compound chip) + stint number.
    // Right: planned vs actually-run lap counts for this set.
    auto* r1 = new QWidget; r1->setFixedHeight(M.stintHead);
    auto* r1l = new QHBoxLayout(r1);
    r1l->setContentsMargins(12, 6, 12, 6); r1l->setSpacing(8);
    const auto boldNum = [](int n) {
        auto* l = new QLabel(QString::number(n));
        QFont f = l->font(); f.setBold(true); l->setFont(f);
        return l;
    };
    r1l->addWidget(makeChip(st.compoundName, st.color, 10));
    r1l->addWidget(boldNum(st.stintNumber));
    r1l->addStretch();
    // Middle: the lap range this set ran (start–end).
    r1l->addWidget(secLabel(QString("%1–%2").arg(st.startLap).arg(st.endLap), sec, 7));
    r1l->addStretch();
    r1l->addWidget(secLabel("Expected Laps:", sec, 7));
    r1l->addWidget(boldNum(st.lapCount));
    r1l->addWidget(secLabel("Actual Laps:", sec, 7));
    r1l->addWidget(boldNum(st.actualLaps));
    v->addWidget(r1);

    // Per-lap target table for this stint. Native OS styling — only the delta
    // cells get a colour (red slower / green faster).
    if (!st.rows.empty()) {
        const std::vector<LapRow>& rows = st.rows;
        const QStringList heads{"LAP", "REQ", "ADJ REQ", "ACTUAL", "Δ LAP", "Δ STINT", "Δ TOTAL"};

        auto* tbl = new QTableWidget((int)rows.size(), (int)heads.size());
        tbl->setHorizontalHeaderLabels(heads);
        tbl->verticalHeader()->setVisible(false);
        tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tbl->setFrameShape(QFrame::NoFrame);
        tbl->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // Alternating row tint + no grid, matching the Tyres/Standings tables.
        tbl->setShowGrid(false);
        tbl->setAlternatingRowColors(true);
        tbl->setSelectionMode(QAbstractItemView::NoSelection);
        tbl->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);  // fill available width
        tbl->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
        // Enforce the measured row + header heights so the table's height equals
        // the value we sum below — no querying this table's laid-out geometry.
        tbl->verticalHeader()->setDefaultSectionSize(M.tableRow);
        tbl->horizontalHeader()->setFixedHeight(M.tableHeader);

        // The lap to pit on (the stint's in-lap) is tinted green across the row,
        // mirroring the Standings fastest-lap highlight (~15% translucent fill).
        const QColor pitTint(kGreen.red(), kGreen.green(), kGreen.blue(), 38);
        for (int r = 0; r < (int)rows.size(); ++r) {
            const LapRow& lr = rows[r];
            const bool isPitRow = !st.isLast && lr.lapNum == st.endLap;
            auto cell = [&](const QString& txt) {
                auto* it = new QTableWidgetItem(txt);
                it->setTextAlignment(Qt::AlignCenter);
                if (isPitRow) it->setBackground(pitTint);
                return it;
            };
            tbl->setItem(r, 0, cell(QString::number(lr.lapNum)));
            tbl->setItem(r, 1, cell(fmtLapTime(lr.requiredMs)));
            tbl->setItem(r, 2, cell(fmtLapTime(lr.actualRequiredMs)));
            tbl->setItem(r, 3, cell(lr.hasActual ? fmtLapTime(lr.actualMs) : "—"));
            auto* dl = cell(lr.hasActual ? fmtDelta(lr.deltaLapMs)   : "—");
            auto* ds = cell(lr.hasActual ? fmtDelta(lr.deltaStintMs) : "—");
            auto* dt = cell(lr.hasActual ? fmtDelta(lr.deltaTotalMs) : "—");
            if (lr.hasActual) {
                dl->setForeground(QBrush(lr.deltaLapMs   > 0 ? kRed : kGreen));
                ds->setForeground(QBrush(lr.deltaStintMs > 0 ? kRed : kGreen));
                dt->setForeground(QBrush(lr.deltaTotalMs > 0 ? kRed : kGreen));
            }
            tbl->setItem(r, 4, dl);
            tbl->setItem(r, 5, ds);
            tbl->setItem(r, 6, dt);
        }

        // table header + one row per lap (from the measured metrics).
        const int tableH = M.tableHeader + M.tableRow * (int)rows.size();
        tbl->setFixedHeight(tableH);
        v->addWidget(tbl);
    }

    g->setFixedHeight(stintGroupHeight(st, withTopSep));   // exact, sizeHint-free
    return g;
}

// Equality over the rendered fields of a stint (and its rows). Drives the
// reconciler: a stint whose display is unchanged keeps its existing widget rather
// than being torn down and rebuilt. Values are recomputed deterministically from
// the same integer-ms inputs, so identical content compares equal exactly.
static bool sameRow(const LapRow& a, const LapRow& b) {
    return a.lapNum == b.lapNum && a.requiredMs == b.requiredMs &&
           a.actualRequiredMs == b.actualRequiredMs && a.actualMs == b.actualMs &&
           a.deltaLapMs == b.deltaLapMs && a.deltaStintMs == b.deltaStintMs &&
           a.deltaTotalMs == b.deltaTotalMs && a.hasActual == b.hasActual;
}
static bool sameStint(const DisplayStint& a, const DisplayStint& b) {
    if (a.compoundName != b.compoundName || a.color != b.color ||
        a.stintNumber != b.stintNumber || a.actualLaps != b.actualLaps ||
        a.startLap != b.startLap || a.endLap != b.endLap || a.lapCount != b.lapCount ||
        a.isLast != b.isLast || a.wornText != b.wornText ||
        a.targetPresent != b.targetPresent || a.isEstimate != b.isEstimate ||
        a.hasDelta != b.hasDelta || a.targetMs != b.targetMs || a.deltaMs != b.deltaMs ||
        a.rows.size() != b.rows.size())
        return false;
    for (size_t i = 0; i < a.rows.size(); ++i)
        if (!sameRow(a.rows[i], b.rows[i])) return false;
    return true;
}

// Reconcile one column's stint widgets against the freshly computed stints. The
// layout holds one group widget per stint followed by a trailing stretch; only
// stints that actually changed are replaced, and only the tail grows/shrinks when
// the stint count changes — so untouched stints (and the scroll position) hold,
// and the scrollbar stops churning on every lap. Returns true if any widget was
// added, removed or replaced.
static bool reconcileColumn(QVBoxLayout* lay, const QColor& sec,
                            std::vector<DisplayStint>& rendered,
                            const std::vector<DisplayStint>& next) {
    bool changed = false;
    const int n = (int)next.size();
    const int m = (int)rendered.size();
    auto dropAt = [&](int i) {
        QLayoutItem* it = lay->takeAt(i);
        if (QWidget* w = it->widget()) {
            // Taken out of the layout, the widget keeps its old geometry until the
            // queued delete runs — hide it so it can't paint over its replacement.
            w->hide();
            w->deleteLater();
        }
        delete it;
    };
    // Replace stints that changed, in place (neighbours untouched).
    for (int i = 0; i < std::min(n, m); ++i) {
        if (sameStint(rendered[i], next[i])) continue;
        dropAt(i);
        lay->insertWidget(i, buildStintGroup(sec, next[i], i > 0));
        changed = true;
    }
    // Append new stints just before the trailing stretch.
    for (int i = m; i < n; ++i) {
        lay->insertWidget(lay->count() - 1, buildStintGroup(sec, next[i], i > 0));
        changed = true;
    }
    // Remove surplus stints from the tail (high index first keeps indices valid).
    for (int i = m - 1; i >= n; --i) { dropAt(i); changed = true; }
    rendered = next;
    return changed;
}

// ─── update() ───────────────────────────────────────────────────────────────

void StrategyPage::update(const json& lap, const json& session, const json& status,
                          const json& damage, const json& timing, const json& participants,
                          const json& tyreSets, const json& allStatus,
                          const std::map<int, int>& lapTimesByNum) {
    const QColor sec    = palette().color(QPalette::PlaceholderText);
    const QColor priCol = palette().color(QPalette::WindowText);

    // Session-change detection → drop stale tracking.
    if (session.is_object()) {
        std::string key = std::to_string(session.value("track_id", -1)) + "/" +
                          std::to_string(session.value("session_type", -1));
        if (key != lastSessionKey_) { resetForNewSession(); lastSessionKey_ = key; }
    }

    const bool hasLap   = lap.is_object() && lap.contains("lap_num");
    const int  lapNum   = hasLap ? lap.value("lap_num", 0) : 0;
    const int  totalLaps = session.is_object() ? session.value("total_laps", 0) : 0;
    const int  sessionType = session.is_object() ? session.value("session_type", -1) : -1;
    const bool isRace = (sessionType == 15 || sessionType == 16 || sessionType == 17);
    // A fitted tyre keeps the strategy valid regardless of age, so a fresh tyre on a
    // pit out-lap (age 0) doesn't blank the panel back to "Waiting for tyre data".
    const bool hasTyreData = status.is_object() && status.value("tyre_compound", 0) > 0;
    const int  trackId = session.is_object() ? session.value("track_id", -1) : -1;
    const int  weather = session.is_object() ? session.value("weather", 0) : 0;

    // Pit-loss is circuit-specific (from the track map); reload only when it changes.
    if (trackId != pitLossTrackId_) { pitLoss_ = loadPitLoss(trackId); pitLossTrackId_ = trackId; }

    const auto sets   = parseSets(tyreSets);
    const auto cars   = parseTiming(timing);
    const int  playerIdx = timing.is_object() ? timing.value("player_idx", -1) : -1;

    // ── Strategy calculation ────────────────────────────────────────────
    // Hold off until lap 2 is complete (i.e. once lap_num ≥ 3, so last_lap_ms is
    // lap 2's time). Lap 1 is a standing start — its pace is useless and would
    // otherwise be frozen as the stint's Required baseline; lap 2 is the first
    // representative lap, so the strategy engages off that.
    StrategyData sd;
    if (hasLap && lapNum >= 3 && session.is_object() && status.is_object() && damage.is_object() &&
        isRace && totalLaps > 0 && hasTyreData) {
        sd.valid = true;

        const double flW = damage.value("tyre_wear_fl", 0.0);
        const double frW = damage.value("tyre_wear_fr", 0.0);
        const double rlW = damage.value("tyre_wear_rl", 0.0);
        const double rrW = damage.value("tyre_wear_rr", 0.0);
        const int    tyreAge = status.value("tyre_age_laps", 0);
        const int    tyreCompound  = status.value("tyre_compound", 0);
        const int    visualCompound = status.value("visual_compound", 0);

        const double avgWear = (flW + frW + rlW + rrW) / 4.0;
        const double wearPerLap = tyreAge > 0 ? avgWear / tyreAge : 2.0;
        const double cliffPct = (visualCompound == 16 || visualCompound == 17 || visualCompound == 18) ? 80.0 : 70.0;
        const bool   isMonaco = trackId == 5;

        // ── Limiting tyre ───────────────────────────────────────────────────
        // Pit timing must follow the corner that reaches the performance cliff
        // first, not the four-corner average. At tracks like Barcelona the
        // average wear can read healthy while one corner (classically the
        // front-left) is already structurally dead — pitting off the average
        // there boxes far too late. Per corner we project laps-to-cliff from its
        // own wear and wear rate, and the soonest one governs the strategy.
        struct CornerWear { const char* name; double wear; };
        const CornerWear cornersW[4] = { {"FL", flW}, {"FR", frW}, {"RL", rlW}, {"RR", rrW} };
        // Per-corner wear rate (linear from new), with the same nominal fallback
        // the average uses before an age baseline exists.
        const auto cornerRate = [&](double w){ return tyreAge > 0 ? w / tyreAge : 2.0; };
        const auto lapsToCliff = [&](double w){ return (cliffPct - w) / std::max(0.01, cornerRate(w)); };
        int    limitIdx = 0;
        double limitLapsLeft = lapsToCliff(cornersW[0].wear);
        for (int i = 1; i < 4; ++i) {
            const double left = lapsToCliff(cornersW[i].wear);
            if (left < limitLapsLeft) { limitLapsLeft = left; limitIdx = i; }
        }
        const double  limitWear       = cornersW[limitIdx].wear;
        const double  limitWearPerLap = std::max(0.01, cornerRate(limitWear));
        const QString limitCorner     = cornersW[limitIdx].name;

        const bool isWet = weather >= 3;
        const auto isWetCompound = [](int c){ return c == 7 || c == 8; };

        std::vector<Set> basePool;
        for (const auto& s : sets) {
            if (s.available && !s.fitted && s.usable_life > 0 &&
                (isWet ? isWetCompound(s.actual) : !isWetCompound(s.actual)))
                basePool.push_back(s);
        }
        std::vector<Set> consPool = basePool, aggPool = basePool;
        std::sort(consPool.begin(), consPool.end(),
                  [](const Set& a, const Set& b){ return a.usable_life > b.usable_life; });
        std::sort(aggPool.begin(), aggPool.end(),
                  [](const Set& a, const Set& b){ return a.lap_delta_ms < b.lap_delta_ms; });

        Compound cur;
        cur.name = tyreLabel(tyreCompound);
        cur.color = compoundColor(visualCompound, priCol);
        cur.actual = tyreCompound; cur.visual = visualCompound;

        const int consLapsLeft = std::max(0, (int)std::floor((cliffPct - limitWear) / limitWearPerLap));
        const int consCliffLap = lapNum + consLapsLeft;

        const Set* fittedSet = nullptr;
        for (const auto& s : sets) if (s.fitted) { fittedSet = &s; break; }
        const double fittedDelta = fittedSet ? fittedSet->lap_delta_ms : 0.0;
        const double freshGainMs = !consPool.empty()
            ? std::max(0.0, fittedDelta - consPool.front().lap_delta_ms) : 0.0;
        const int consFirstPit = (freshGainMs > 500 && freshGainMs * consLapsLeft > pitLoss_.totalMs)
            ? std::max(lapNum + 1, lapNum + (int)std::ceil(pitLoss_.totalMs / freshGainMs))
            : consCliffLap;

        StrategyResult conservative = buildStints(lapNum, totalLaps, consFirstPit, cur, avgWear, consPool, priCol);
        if (isMonaco)
            conservative = applyMonacoRule(conservative, consPool, usedCompoundsOf(conservative), priCol);

        const double aggWearRate = limitWearPerLap * 1.2;
        const int aggLapsLeft = std::max(0, (int)std::floor((cliffPct - limitWear) / aggWearRate));
        const int aggCliffLap = lapNum + aggLapsLeft;
        const int economicOptLap = !aggPool.empty()
            ? std::max(lapNum + 1, totalLaps - aggPool.front().usable_life) : aggCliffLap;
        const int aggPitLap = std::min(aggCliffLap, economicOptLap);
        StrategyResult aggressive = buildStints(lapNum, totalLaps, aggPitLap, cur, avgWear, aggPool, priCol);
        if (aggressive.stops <= conservative.stops)
            aggressive = forceExtraStop(aggressive, aggPool, usedCompoundsOf(aggressive), priCol);
        if (isMonaco)
            aggressive = applyMonacoRule(aggressive, aggPool, usedCompoundsOf(aggressive), priCol);

        // Wear warnings (priority-ranked; keep at most 2)
        const double leftWear  = (flW + rlW) / 2.0;
        const double rightWear = (frW + rrW) / 2.0;
        const double frontWear = (flW + frW) / 2.0;
        const double rearWear  = (rlW + rrW) / 2.0;
        const double avg = avgWear;
        const double maxWear = std::max({flW, frW, rlW, rrW});
        const double sideImb = rightWear - leftWear;
        const double axleImb = frontWear - rearWear;
        const double diagImb = (flW + rrW) / 2.0 - (frW + rlW) / 2.0;
        const double rearImb = rlW - rrW;

        std::vector<WearWarning> cand;
        if (maxWear > avg + 22) {
            QString corner = maxWear == flW ? "FL" : maxWear == frW ? "FR" : maxWear == rlW ? "RL" : "RR";
            QString detail = corner == "FL"
                ? "Outside front taking 70–80% of axle load in right-handers — structural fatigue imminent; prioritise your box lap"
                : corner == "FR"
                ? "Outside front taking 70–80% of axle load in left-handers — structural fatigue imminent; prioritise your box lap"
                : corner == "RL"
                ? "Rear-left near structural limit — wheelspin overheating on exit; close differential or ease early throttle"
                : "Rear-right near structural limit — peak lateral and drive load; smooth corner entry and mid-corner throttle";
            cand.push_back({ QString("%1 at structural risk").arg(corner), detail, kRed, 0 });
        }
        {
            std::vector<double> s4 = { flW, frW, rlW, rrW };
            std::sort(s4.begin(), s4.end(), std::greater<double>());
            if (maxWear - s4[1] > 10) {
                if (maxWear == flW) cand.push_back({ "Front-left is the limit tyre",
                    "Sustained right-handers push 70–80% of axle load onto the outside front — classic Silverstone/Lusail/Suzuka pattern; monitor cliff lap closely", kOrange, 1 });
                else if (maxWear == frW) cand.push_back({ "Front-right is the limit tyre",
                    "Sustained left-handers are loading the outside front corner — ease entry speed and let steering unwind earlier", kOrange, 1 });
                else if (maxWear == rlW) cand.push_back({ "Rear-left overheating under throttle",
                    "Inside rear spinning on corner exit — differential too open; ease the initial throttle application and smooth the trace", kOrange, 1 });
                else cand.push_back({ "Rear-right taking excess mid-corner load",
                    "Outside rear under simultaneous lateral and drive load — ease through fast right-handers; smooth the corner entry", kOrange, 1 });
            }
        }
        if (std::abs(diagImb) > 8) {
            cand.push_back(diagImb > 0
                ? WearWarning{ "Cross-diagonal wear — FL + RR",
                    "Car rotating too aggressively; oversteer tendency loading opposite corners — try stiffening rear ARB or easing initial throttle on exit", kOrange, 2 }
                : WearWarning{ "Cross-diagonal wear — FR + RL",
                    "Car pushing at apex; understeer tendency loading opposite corners — ease brake bias or add front wing to reduce entry push", kOrange, 2 });
        }
        if (rearWear > frontWear && std::abs(rearImb) > 8) {
            cand.push_back(rlW > rrW
                ? WearWarning{ "Rear-left overheating under traction",
                    "Inside rear losing grip first on exit — differential too open; reduce wheelspin by easing the initial throttle application", kOrange, 2 }
                : WearWarning{ "Rear-right absorbing peak lateral load",
                    "Outside rear carrying combined slip through right-handers — ease mid-corner throttle and smooth the steering transition", kOrange, 2 });
        }
        if (std::abs(axleImb) > 8) {
            cand.push_back(axleImb > 0
                ? WearWarning{ "Front axle under peak stress",
                    "Braking zones and lateral load wearing fronts faster than rears — shift brake bias rearward or trim front wing to reduce load", kOrange, 3 }
                : WearWarning{ "Rear tyres absorbing excess torque load",
                    "Combined slip from drive torque and lateral force — reduce wheelspin on corner exit; smooth the throttle trace", kOrange, 3 });
        }
        if (std::abs(sideImb) > 5) {
            cand.push_back(sideImb > 0
                ? WearWarning{ "Right tyres degrading faster",
                    "Left-hand corners loading the right-side contact patches harder — ease entry speed and let the car flow more smoothly", kOrange, 3 }
                : WearWarning{ "Left tyres degrading faster",
                    "Right-hand corners loading the left-side contact patches harder — ease entry speed and let the car flow more smoothly", kOrange, 3 });
        }
        std::stable_sort(cand.begin(), cand.end(),
                         [](const WearWarning& a, const WearWarning& b){ return a.priority < b.priority; });
        if (cand.size() > 2) cand.resize(2);
        for (auto& w : cand)
            w.color = w.priority <= 1 ? kRed : w.priority == 2 ? kOrange : kAmber;

        sd.avgWear = avgWear; sd.wearPerLap = wearPerLap;
        sd.limitWear = limitWear; sd.limitWearPerLap = limitWearPerLap; sd.limitCorner = limitCorner;
        sd.estimatedCliffLap = consCliffLap; sd.lapsUntilCliff = consLapsLeft;
        sd.conservative = conservative; sd.aggressive = aggressive;
        sd.currentCompound = cur; sd.isMonaco = isMonaco;
        sd.wearWarnings = cand;
    }

    // ── Position window + gap trend (needs timing) ──────────────────────
    const TimingCar* player = nullptr;
    std::vector<const TimingCar*> active;
    for (const auto& c : cars) if (c.result_status == 2 && c.position > 0) active.push_back(&c);
    for (const auto* c : active) if (c->idx == playerIdx) player = c;

    std::vector<const TimingCar*> aheadCars, behindCars;
    if (player) {
        int carsAhead = 0, carsBehind = 0;
        for (const auto* c : active) { if (c->position < player->position) ++carsAhead; else if (c->position > player->position) ++carsBehind; }
        const int baseAhead = std::min(3, carsAhead);
        const int baseBehind = std::min(3, carsBehind);
        const int aheadCount  = std::min(baseAhead  + std::max(0, 3 - baseBehind), carsAhead);
        const int behindCount = std::min(baseBehind + std::max(0, 3 - baseAhead),  carsBehind);
        for (int i = 0; i < aheadCount; ++i)
            for (const auto* c : active) if (c->position == player->position - (i + 1)) { aheadCars.push_back(c); break; }
        for (int i = 0; i < behindCount; ++i)
            for (const auto* c : active) if (c->position == player->position + (i + 1)) { behindCars.push_back(c); break; }

        const TimingCar* immAhead  = aheadCars.empty()  ? nullptr : aheadCars.front();
        const TimingCar* immBehind = behindCars.empty() ? nullptr : behindCars.front();
        if (!immBehind) { hasPrevBehindGap_ = false; behindIsGaining_ = -1; }
        else {
            const double g = immBehind->gap_ms - player->gap_ms;
            if (hasPrevBehindGap_) { const double d = g - prevBehindGap_; if (std::abs(d) > 30) behindIsGaining_ = d < 0 ? 1 : 0; }
            prevBehindGap_ = g; hasPrevBehindGap_ = true;
        }
        if (!immAhead) { hasPrevAheadGap_ = false; playerGainingAhead_ = -1; }
        else {
            const double g = player->gap_ms - immAhead->gap_ms;
            if (hasPrevAheadGap_) { const double d = g - prevAheadGap_; if (std::abs(d) > 30) playerGainingAhead_ = d < 0 ? 1 : 0; }
            prevAheadGap_ = g; hasPrevAheadGap_ = true;
        }
    } else {
        hasPrevAheadGap_ = hasPrevBehindGap_ = false;
        playerGainingAhead_ = behindIsGaining_ = -1;
    }

    // Pit counts come straight from the game's per-car num_pit_stops (replay-safe).

    // ── Rivals latch (lap 1 → 2) ────────────────────────────────────────
    if (hasLap && !cars.empty()) {
        if (prevLapNum_ == 1 && lapNum == 2 && !rivalsLatched_ && player) {
            for (const auto* c : active) if (c->position == player->position - 1) { rivalAheadIdx_ = c->idx; break; }
            for (const auto* c : active) if (c->position == player->position + 1) { rivalBehindIdx_ = c->idx; break; }
            rivalsLatched_ = true;
        }
        prevLapNum_ = lapNum;
    }

    // ── Pace / stint targets ────────────────────────────────────────────
    std::vector<StintTarget> consTargets, aggTargets;
    if (sd.valid && player) {
        double playerLapMs = player->last_lap_ms;
        if (playerLapMs <= 0) playerLapMs = hasLap ? lap.value("last_lap_ms", 0) : 0;
        double behindLapMs = 0;
        for (const auto* c : active) if (c->position == player->position + 1) { behindLapMs = c->last_lap_ms; break; }

        if (playerLapMs > 0 && playerLapMs <= 600000) {
            const Set* fittedSet = nullptr;
            for (const auto& s : sets) if (s.fitted) { fittedSet = &s; break; }
            const double fittedDelta = fittedSet ? fittedSet->lap_delta_ms : 0.0;
            const int totalRemaining = std::max(1, totalLaps - lapNum);

            auto calc = [&](const StrategyResult& res, bool aggressive) {
                std::vector<StintTarget> out;
                const double perLapOffset = aggressive ? pitLoss_.totalMs / totalRemaining : 0.0;
                for (size_t i = 0; i < res.stints.size(); ++i) {
                    StintTarget t;
                    if (i == 0) {
                        if (behindLapMs > 0 && behindLapMs < 600000) {
                            t.present = true; t.isEstimate = false; t.hasDelta = true;
                            t.targetMs = behindLapMs - perLapOffset;
                            t.deltaMs = playerLapMs - t.targetMs;
                        } else {
                            t.present = true; t.isEstimate = false; t.hasDelta = true;
                            t.targetMs = playerLapMs; t.deltaMs = 0;
                        }
                    } else {
                        const Set* match = nullptr;
                        for (const auto& s : sets)
                            if (!s.fitted && s.available && s.actual == res.stints[i].actual) { match = &s; break; }
                        if (match) {
                            const double deltaVsCurrent = match->lap_delta_ms - fittedDelta;
                            t.present = true; t.isEstimate = true; t.hasDelta = false;
                            t.targetMs = std::max(60000.0, playerLapMs + deltaVsCurrent - perLapOffset);
                        }
                    }
                    out.push_back(t);
                }
                return out;
            };
            consTargets = calc(sd.conservative, false);
            aggTargets  = calc(sd.aggressive, true);
        }
    }

    // ── Stint display (completed + current + future) ────────────────────
    // The live plan only carries the current + future stints, so each stint is
    // recorded the moment it begins (compound + frozen Required, captured at its
    // start lap). Completed stints are then kept on screen instead of vanishing
    // after a pit. The opening stint is anchored to the tyre's fit lap (lapNum −
    // tyre age) so laps already driven stay in the table. Actuals are gated to laps
    // completed as of the playhead (n < lapNum), so this is identical live and in
    // playback. The display is rebuilt once per lap and cached.
    const int tyreAge = status.is_object() ? status.value("tyre_age_laps", 0) : 0;
    const int openingStart = std::max(1, lapNum - tyreAge);

    // Record the current opening stint into the per-variant history (idempotent —
    // its true start only jumps forward on a pit).
    auto recordStint = [&](const StrategyResult& res, std::map<int, double>& frozenMap,
                           std::vector<PastStint>& past, const std::vector<StintTarget>& targets) -> bool {
        if (!sd.valid || res.stints.empty() || targets.empty() || !targets[0].present) return false;
        bool added = false;
        if (past.empty() || openingStart > past.back().startLap) {   // a new stint begins
            if (!frozenMap.count(openingStart)) frozenMap[openingStart] = targets[0].targetMs;
            PastStint ps;
            ps.startLap     = openingStart;
            ps.reqBaseMs    = frozenMap[openingStart];
            ps.compoundName = res.stints[0].compoundName;
            ps.color        = res.stints[0].color;
            ps.postPit      = !past.empty();
            past.push_back(ps);
            added = true;
        }
        // Keep the current (still-open) stint's planned length live. Once the next
        // stint pushes in front of it, it stops updating and keeps this value — so a
        // stint boxed early still reports the laps it was planned for, not the fewer
        // laps it actually ran (which is "Actual Laps").
        const Stint& s0 = res.stints[0];
        const int s0end = s0.isLast ? totalLaps : s0.pitLap;
        past.back().expectedLaps = std::max(1, s0end - openingStart + 1);
        return added;
    };
    bool stintsChanged = false;
    stintsChanged |= recordStint(sd.conservative, consFrozenReqMs_, consPast_, consTargets);
    stintsChanged |= recordStint(sd.aggressive,  aggFrozenReqMs_, aggPast_,  aggTargets);

    auto computeRows = [&](int start, int end, double base, bool postPit, bool prePit,
                           double& raceCum) {
        std::vector<LapRow> rows;
        double cum = 0.0;   // cumulative delta over stint, through the previous lap
        for (int n = start; n > 0 && n <= end; ++n) {
            LapRow lr;
            lr.lapNum = n;
            // Pitting costs time on two laps: the out-lap (first lap of a post-pit
            // stint) and the in-lap (last lap of a stint that ends in a pit). Each
            // carries its circuit-specific penalty.
            lr.requiredMs = base
                + ((postPit && n == start) ? pitLoss_.outlapMs : 0.0)
                + ((prePit  && n == end)   ? pitLoss_.inlapMs  : 0.0);
            // Adjusted required = base minus the running buffer: time banked (negative
            // cumulative delta = faster) raises the time you may run; lost time lowers it.
            lr.actualRequiredMs = lr.requiredMs - cum;
            auto it = lapTimesByNum.find(n);
            if (n < lapNum && it != lapTimesByNum.end() && it->second > 0) {
                lr.hasActual    = true;
                lr.actualMs     = it->second;
                lr.deltaLapMs   = lr.actualMs - lr.requiredMs;
                cum            += lr.deltaLapMs;
                lr.deltaStintMs = cum;
                raceCum        += lr.deltaLapMs;   // running total across the whole race
                lr.deltaTotalMs = raceCum;
            }
            rows.push_back(lr);
        }
        return rows;
    };

    auto buildDisplay = [&](const StrategyResult& res, const std::vector<StintTarget>& targets,
                            std::map<int, double>& frozenMap, const std::vector<PastStint>& past) {
        std::vector<DisplayStint> out;
        if (!sd.valid) return out;
        // Δ TOTAL accumulates gain/loss across the whole race — threaded through every
        // stint's rows in lap order so it never resets at a pit (unlike Δ STINT).
        double raceCum = 0.0;
        // Stints already begun as of the playhead (keeps playback playhead-exact).
        std::vector<const PastStint*> done;
        for (const PastStint& ps : past) if (ps.startLap <= lapNum) done.push_back(&ps);

        // Completed stints — every recorded one but the current (last) opening stint.
        for (size_t k = 0; k + 1 < done.size(); ++k) {
            const PastStint& ps = *done[k];
            const int start = ps.startLap, end = done[k + 1]->startLap - 1;
            if (end < start) continue;
            DisplayStint d;
            d.compoundName = ps.compoundName; d.color = ps.color;
            d.stintNumber = (int)out.size() + 1;
            d.startLap = start; d.endLap = end;
            // Expected = the plan frozen at the box; Actual = laps it really ran.
            d.lapCount = ps.expectedLaps > 0 ? ps.expectedLaps : end - start + 1;
            d.actualLaps = end - start + 1;
            d.targetPresent = true; d.targetMs = ps.reqBaseMs;
            // A completed stint always ended in a pit (a later stint followed it).
            d.rows = computeRows(start, end, ps.reqBaseMs, ps.postPit, true, raceCum);
            out.push_back(d);
        }
        // Current + future stints from the live forward plan. The card always shows;
        // the target line and per-lap table appear once a target exists.
        const bool pittedBefore = done.size() > 1;
        for (size_t i = 0; i < res.stints.size(); ++i) {
            const Stint& st = res.stints[i];
            const StintTarget tg = (i < targets.size()) ? targets[i] : StintTarget{};
            const bool isOpening = (i == 0);
            const int  start = isOpening ? openingStart : st.startLap;
            const int  end   = st.isLast ? totalLaps : st.pitLap;
            DisplayStint d;
            d.compoundName = st.compoundName; d.color = st.color;
            d.stintNumber = (int)out.size() + 1;
            d.startLap = start; d.endLap = end;
            d.lapCount = isOpening ? std::max(1, end - start + 1) : st.lapCount;
            d.isLast = st.isLast;
            d.wornText = isOpening
                ? QString("%1 %2% worn").arg(sd.limitCorner).arg((int)std::lround(sd.limitWear))
                : QString("Fresh");
            if (tg.present && tg.targetMs > 0) {
                const bool postPit = isOpening ? pittedBefore : true;
                const double base = frozenMap.count(start) ? frozenMap[start] : tg.targetMs;
                d.targetPresent = true; d.isEstimate = tg.isEstimate; d.hasDelta = tg.hasDelta;
                d.targetMs = tg.targetMs; d.deltaMs = tg.deltaMs;
                d.rows = computeRows(start, end, base, postPit, !st.isLast, raceCum);
            }
            // Laps actually run so far on this set: completed (timed) rows. Future
            // stints have none yet → 0; the opening stint counts laps done to date.
            for (const LapRow& lr : d.rows) if (lr.hasActual) ++d.actualLaps;
            out.push_back(d);
        }
        return out;
    };

    bool displayChanged = false;
    if (sd.valid && (lapNum != tableLapComputed_ || stintsChanged)) {
        consDisplay_ = buildDisplay(sd.conservative, consTargets, consFrozenReqMs_, consPast_);
        aggDisplay_  = buildDisplay(sd.aggressive,  aggTargets,  aggFrozenReqMs_, aggPast_);
        tableLapComputed_ = lapNum;
        displayChanged = true;
    }

    // ── Undercut / overcut call ─────────────────────────────────────────
    enum CallType { None, Undercut, Overcut };
    CallType callType = None; int callTarget = -1; double callGap = 0; int callCross = 0;
    if (sd.valid && player) {
        const int playerTyreAge = status.value("tyre_age_laps", 0);
        double freshGainMs = 0;
        if (sd.conservative.stints.size() > 1) {
            const Set* fittedSet = nullptr;
            for (const auto& s : sets) if (s.fitted) { fittedSet = &s; break; }
            const double fittedDelta = fittedSet ? fittedSet->lap_delta_ms : 0.0;
            const bool isWet = weather >= 3;
            const auto isWetCompound = [](int c){ return c == 7 || c == 8; };
            std::vector<Set> pool;
            for (const auto& s : sets)
                if (s.available && !s.fitted && s.usable_life > 0 &&
                    (isWet ? isWetCompound(s.actual) : !isWetCompound(s.actual))) pool.push_back(s);
            std::sort(pool.begin(), pool.end(), [](const Set& a, const Set& b){ return a.lap_delta_ms < b.lap_delta_ms; });
            if (!pool.empty()) freshGainMs = std::max(0.0, fittedDelta - pool.front().lap_delta_ms);
        }

        auto rivalTyreAge = [&](int idx) -> int {
            if (allStatus.is_object() && allStatus.contains("cars"))
                for (const auto& c : allStatus["cars"]) if (c.value("idx", -1) == idx) return c.value("tyre_age_laps", 0);
            return 0;
        };

        const TimingCar* carAhead = nullptr;
        for (const auto* c : active) if (c->position == player->position - 1) { carAhead = c; break; }
        if (carAhead) {
            const double aheadGapMs = player->gap_ms - carAhead->gap_ms;
            const int crossover = freshGainMs > 0 ? (int)std::ceil(pitLoss_.totalMs / freshGainMs) : 999;
            if (aheadGapMs > 0 && aheadGapMs < 3000 && freshGainMs > 200 &&
                rivalTyreAge(carAhead->idx) >= playerTyreAge + 3 && crossover <= 10) {
                callType = Undercut; callTarget = carAhead->idx; callGap = aheadGapMs; callCross = crossover;
            }
        }
        if (callType == None) {
            const TimingCar* carBehind = nullptr;
            for (const auto* c : active) if (c->position == player->position + 1) { carBehind = c; break; }
            if (carBehind && sd.lapsUntilCliff >= 3) {
                const double behindGapMs = carBehind->gap_ms - player->gap_ms;
                const bool isOnInlap = carBehind->driver_status == 2;
                if (behindGapMs >= 0 && behindGapMs < 3000 &&
                    (isOnInlap || rivalTyreAge(carBehind->idx) >= playerTyreAge + 5)) {
                    callType = Overcut; callTarget = carBehind->idx; callGap = behindGapMs;
                }
            }
        }
    }

    // ── Render header ───────────────────────────────────────────────────
    lapValue_->setText(hasLap ? QString::number(lapNum) : "—");
    lapTotal_->setText(QString("/ %1").arg(totalLaps > 0 ? QString::number(totalLaps) : "—"));

    // Headline tracks the limiting tyre so the wear figure and the cliff lap
    // tell the same story (the per-corner Tyre Wear panel still shows all four).
    const QColor wearC = wearColor(sd.valid ? sd.limitWear : sd.avgWear);
    compoundChip_->setText(sd.valid ? sd.currentCompound.name : "—");
    {
        const QColor cc = sd.valid ? sd.currentCompound.color : sec;
        compoundChip_->setStyleSheet(QString(
            "QLabel{ color:%1; border:1px solid %1; border-radius:4px; padding:1px 6px;"
            " font-weight:bold; font-size:10px; background:%2; }")
            .arg(cc.name()).arg(rgba(cc, 0.1)));
    }
    const double headlineWear = sd.valid ? sd.limitWear : sd.avgWear;
    wearPct_->setText(QString("%1%").arg((int)std::lround(headlineWear)));
    { QPalette p = wearPct_->palette(); p.setColor(QPalette::WindowText, wearC); wearPct_->setPalette(p); }
    wearAge_->setText(status.is_object()
        ? (sd.valid
               ? QString("%1 · %2L · %3%/L").arg(sd.limitCorner).arg(status.value("tyre_age_laps", 0))
                     .arg(status.value("tyre_age_laps", 0) > 0
                              ? QString::number(sd.limitWearPerLap, 'f', 1) : QString("—"))
               : QString("%1L · —%/L").arg(status.value("tyre_age_laps", 0)))
        : QString("—"));
    wearBar_->setValue((int)std::min(100.0, headlineWear));
    wearBar_->setStyleSheet(QString(
        "QProgressBar{ background:%1; border:none; border-radius:3px; }"
        "QProgressBar::chunk{ background:%2; border-radius:3px; }")
        .arg(rgba(sec, 0.25)).arg(wearC.name()));

    if (sd.valid) {
        const QColor cliffC = sd.lapsUntilCliff <= 5 ? kRed : sd.lapsUntilCliff <= 10 ? kAmber : priCol;
        cliffValue_->setText(QString("Lap %1").arg(sd.estimatedCliffLap));
        QPalette p = cliffValue_->palette(); p.setColor(QPalette::WindowText, cliffC); cliffValue_->setPalette(p);
        cliffPlus_->setText(QString("+%1").arg(sd.lapsUntilCliff));
    } else {
        cliffValue_->setText("—");
        QPalette p = cliffValue_->palette(); p.setColor(QPalette::WindowText, sec); cliffValue_->setPalette(p);
        cliffPlus_->setText("");
    }

    // ── Render body ─────────────────────────────────────────────────────
    if (!isRace) {
        bodyStack_->setCurrentIndex(1);
        sidebarSep_->hide();
        sidebarScroll_->hide();
        return;
    }
    bodyStack_->setCurrentIndex(0);
    sidebarSep_->show();
    sidebarScroll_->show();

    const bool isDark = palette().color(QPalette::Window).lightness() < 128;
    const QColor blueAccent  = isDark ? kBlue  : QColor("#0B57D0");
    const QColor amberAccent = isDark ? kAmber : QColor("#B06000");

    if (sd.valid) everStratValid_ = true;

    // Once a valid strategy has shown, keep the timeline on screen through stint
    // changes (a fresh tyre no longer blanks it). The stint widgets are reconciled
    // in place — only the stints that actually changed are rebuilt — so unchanged
    // stints, and the scroll position, hold steady from lap to lap.
    if (everStratValid_) {
        if (displayChanged || !stratShowingTimeline_) {
            // Header bar (outside the scroll area) only depends on the stop count and
            // the Monaco flag — rebuild it solely when one of those changes.
            if (sd.conservative.stops != consStopsShown_ || sd.isMonaco != monacoShown_) {
                clearColumn(consHeaderLayout_);
                consHeaderLayout_->addWidget(buildStintHeaderBar(sd.conservative.stops, sd.isMonaco,
                                                                 "Conservative", blueAccent, sec));
                consStopsShown_ = sd.conservative.stops;
            }
            if (sd.aggressive.stops != aggStopsShown_ || sd.isMonaco != monacoShown_) {
                clearColumn(aggHeaderLayout_);
                aggHeaderLayout_->addWidget(buildStintHeaderBar(sd.aggressive.stops, sd.isMonaco,
                                                                "Aggressive", amberAccent, sec));
                aggStopsShown_ = sd.aggressive.stops;
            }
            monacoShown_ = sd.isMonaco;

            // First time in from the "Waiting…" placeholder: drop it and seed each
            // column with just the trailing stretch the reconciler inserts before.
            if (!stratShowingTimeline_) {
                clearColumn(consLayout_); consLayout_->addStretch(); consRendered_.clear();
                clearColumn(aggLayout_);  aggLayout_->addStretch();  aggRendered_.clear();
                stratShowingTimeline_ = true;
            }
            stratBuilt_ = true;

            // Where each column sat before reconciling — captured up-front so an
            // add/remove that shifts content above the viewport doesn't move the view.
            const int consPrevScroll = consScroll_->verticalScrollBar()->value();
            const int aggPrevScroll  = aggScroll_->verticalScrollBar()->value();

            // Reconcile both columns (always run both — they are independent).
            const bool consChanged = reconcileColumn(consLayout_, sec, consRendered_, consDisplay_);
            const bool aggChanged  = reconcileColumn(aggLayout_,  sec, aggRendered_,  aggDisplay_);
            const bool changed = consChanged || aggChanged;

            // Nothing to do when no widget moved — the scrollbar stays exactly as it
            // was, which is the whole point of reconciling rather than rebuilding.
            if (changed) {
                // Pin each column's content to its EXACT summed height. The scroll
                // range must not depend on the content's sizeHint: a stint's table is
                // a QTableWidget whose sizeHint ignores our setFixedHeight and reports
                // a tiny default, so widgetResizable would size the content to the
                // viewport height (range 0 → scrollbar hides) before the real layout
                // lands (→ scrollbar shows) — the flicker. setMinimumHeight stops
                // widgetResizable ever clamping below the true height; the resize
                // applies it synchronously so the range is right this frame, not next.
                auto columnHeight = [](const std::vector<DisplayStint>& v) {
                    int h = 0;
                    for (size_t i = 0; i < v.size(); ++i) h += stintGroupHeight(v[i], i > 0);
                    return h;
                };
                const std::pair<QScrollArea*, int> sized[2] = {
                    { consScroll_, columnHeight(consRendered_) },
                    { aggScroll_,  columnHeight(aggRendered_)  } };
                for (const auto& [sc, h] : sized) {
                    if (QWidget* w = sc->widget()) {
                        w->setMinimumHeight(h);
                        w->resize(sc->viewport()->width(), std::max(sc->viewport()->height(), h));
                    }
                }

                // Keep each column where it was, allowing only a small (≤100px) drift
                // as the timeline grows a row; a larger change would yank the view, so
                // reject it and stay put. Run once now and again queued, after the
                // pending layout pass recomputes the range, so the position sticks.
                auto restore = [this, consPrevScroll, aggPrevScroll] {
                    const std::pair<QScrollArea*, int> cols[2] = {
                        { consScroll_, consPrevScroll }, { aggScroll_, aggPrevScroll } };
                    for (const auto& [sc, prev] : cols) {
                        QScrollBar* bar = sc->verticalScrollBar();
                        if (qAbs(bar->value() - prev) > 100)
                            bar->setValue(qMin(prev, bar->maximum()));
                    }
                };
                restore();
                QMetaObject::invokeMethod(this, restore, Qt::QueuedConnection);
            }
        }
    } else if (!stratBuilt_) {
        // Only before the very first valid strategy.
        clearColumn(consLayout_);
        auto* wait = new QLabel("Waiting for tyre data…");
        wait->setAlignment(Qt::AlignCenter);
        QPalette p = wait->palette(); p.setColor(QPalette::WindowText, sec); wait->setPalette(p);
        consLayout_->addWidget(wait);
        consLayout_->addStretch();
        clearColumn(aggLayout_);
        aggLayout_->addStretch();
        stratBuilt_ = true;
    }

    // ── Sidebar ─────────────────────────────────────────────────────────
    clearColumn(sidebarLayout_);

    // Strategy call
    if (callType != None) {
        const bool isUnder = callType == Undercut;
        const QColor color = isUnder ? kAmber : kGreen;
        auto* sect = new QWidget; auto* sv = new QVBoxLayout(sect);
        sv->setContentsMargins(0, 0, 0, 0); sv->setSpacing(0);
        sv->addWidget(sectionHeader("Strategy Call", sec));
        auto* row = new QWidget; auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(12, 0, 12, 4); rl->setSpacing(10);
        rl->addWidget(makeChip(isUnder ? "UNDERCUT" : "OVERCUT", color));
        auto* nm = new QLabel(driverName(participants, callTarget));
        { QFont f = nm->font(); f.setBold(true); nm->setFont(f); }
        nm->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // shrink, don't force row wider than sidebar
        rl->addWidget(nm, 1);
        auto* gap = new QLabel(isUnder ? QString("+%1 ahead").arg(fmtSecs(callGap))
                                       : QString("−%1 behind").arg(fmtSecs(callGap)));
        { QFont f = gap->font(); f.setBold(true); gap->setFont(f);
          QPalette p = gap->palette(); p.setColor(QPalette::WindowText, color); gap->setPalette(p); }
        rl->addWidget(gap);
        sv->addWidget(row);
        auto* action = new QLabel(isUnder
            ? QString("Pit now — recover time in %1 lap%2").arg(callCross).arg(callCross != 1 ? "s" : "")
            : QString("Stay out — build gap while they stop"));
        action->setWordWrap(true); action->setContentsMargins(12, 0, 12, 6);
        { QPalette p = action->palette(); p.setColor(QPalette::WindowText, sec); action->setPalette(p); }
        sv->addWidget(action);
        sidebarLayout_->addWidget(sect);
        sidebarLayout_->addWidget(makeHLine());
    }

    // Rivals
    if (rivalsLatched_ && (rivalAheadIdx_ != -1 || rivalBehindIdx_ != -1) && !cars.empty() && player) {
        auto* sect = new QWidget; auto* sv = new QVBoxLayout(sect);
        sv->setContentsMargins(0, 0, 0, 0); sv->setSpacing(0);
        sv->addWidget(sectionHeader("Rivals", sec));
        struct RivalRow { int idx; bool ahead; };
        std::vector<RivalRow> rows;
        if (rivalAheadIdx_  != -1) rows.push_back({ rivalAheadIdx_, true });
        if (rivalBehindIdx_ != -1) rows.push_back({ rivalBehindIdx_, false });
        for (size_t r = 0; r < rows.size(); ++r) {
            const auto& rr = rows[r];
            const TimingCar* car = nullptr;
            for (const auto& c : cars) if (c.idx == rr.idx) { car = &c; break; }
            const bool retired = !car || car->result_status != 2;
            const QColor dirColor = rr.ahead ? kBlue : kAmber;
            auto* row = new QWidget; auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(12, 6, 12, 6); rl->setSpacing(8);
            auto* arrow = new QLabel(rr.ahead ? "▲" : "▼");
            { QFont f = arrow->font(); f.setBold(true); arrow->setFont(f);
              QPalette p = arrow->palette(); p.setColor(QPalette::WindowText, dirColor); arrow->setPalette(p); }
            rl->addWidget(arrow);
            if (car && !retired) {
                const QColor lc = liveryColor(participants, rr.idx);
                auto* pos = new QLabel(QString("P%1").arg(car->position));
                pos->setAlignment(Qt::AlignCenter);
                pos->setStyleSheet(QString("QLabel{ color:%1; background:%2; border-radius:3px;"
                    " padding:1px 4px; font-weight:bold; font-size:10px; }")
                    .arg(lc.name()).arg(rgba(lc, 0.16)));
                rl->addWidget(pos);
            } else {
                rl->addWidget(secLabel("—", sec));
            }
            auto* nm = new QLabel(driverName(participants, rr.idx));
            { QFont f = nm->font(); f.setBold(true); nm->setFont(f);
              QPalette p = nm->palette(); p.setColor(QPalette::WindowText, sec); nm->setPalette(p); }
            nm->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            rl->addWidget(nm, 1);
            if (retired) {
                rl->addWidget(secLabel("DNF", sec, -1, true));
            } else {
                QString gapStr;
                if (rr.ahead) { const double g = player->gap_ms - car->gap_ms; if (g > 0) gapStr = "+" + fmtSecs(g); }
                else          { const double g = car->gap_ms - player->gap_ms; if (g > 0) gapStr = "−" + fmtSecs(g); }
                if (!gapStr.isEmpty()) rl->addWidget(secLabel(gapStr, sec, -1, true));
            }
            sv->addWidget(row);
            if (r + 1 < rows.size()) sv->addWidget(makeHLine());
        }
        sidebarLayout_->addWidget(sect);
        sidebarLayout_->addWidget(makeHLine());
    }

    // Position
    if (!cars.empty()) {
        auto* sect = new QWidget; auto* sv = new QVBoxLayout(sect);
        sv->setContentsMargins(0, 0, 0, 0); sv->setSpacing(0);
        sv->addWidget(sectionHeader("Position", sec));
        if (player) {
            struct PosRow { const TimingCar* car; int role; bool immediate; };  // role: 0 ahead, 1 player, 2 behind
            std::vector<PosRow> rows;
            for (int i = (int)aheadCars.size() - 1; i >= 0; --i)
                rows.push_back({ aheadCars[i], 0, i == 0 });
            rows.push_back({ player, 1, false });
            for (size_t i = 0; i < behindCars.size(); ++i)
                rows.push_back({ behindCars[i], 2, i == 0 });

            for (size_t r = 0; r < rows.size(); ++r) {
                const auto& pr = rows[r];
                const bool isPlayer = pr.role == 1;
                const QColor lc = liveryColor(participants, pr.car->idx);
                auto* row = new QWidget; auto* rl = new QHBoxLayout(row);
                rl->setContentsMargins(12, 6, 12, 6); rl->setSpacing(8);
                auto* pos = new QLabel(QString("P%1").arg(pr.car->position));
                pos->setAlignment(Qt::AlignCenter);
                pos->setStyleSheet(QString("QLabel{ color:%1; background:%2; border-radius:3px;"
                    " padding:1px 4px; font-weight:bold; font-size:10px; }")
                    .arg(lc.name()).arg(rgba(lc, 0.16)));
                rl->addWidget(pos);
                auto* nm = new QLabel(driverName(participants, pr.car->idx));
                { QFont f = nm->font(); f.setBold(true); nm->setFont(f);
                  QPalette p = nm->palette(); p.setColor(QPalette::WindowText, isPlayer ? priCol : sec); nm->setPalette(p); }
                nm->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                rl->addWidget(nm, 1);

                const int stops = pr.car->num_pit_stops;   // game count — replay-safe
                const bool inPit = pr.car->pit_status != 0;
                if (inPit) rl->addWidget(makeChip("PIT", kAmber));
                else if (stops > 0) rl->addWidget(secLabel(QString("%1 pit%2").arg(stops).arg(stops != 1 ? "s" : ""), sec, 7));

                double gapMs = -1; QColor gapColor = sec;
                if (pr.role == 0) {
                    const double gap = player->gap_ms - pr.car->gap_ms;
                    if (gap > 0) { gapMs = gap;
                        if (gap > 10000) gapColor = kRed;
                        else if (pr.immediate) gapColor = playerGainingAhead_ == 1 ? kGreen : kAmber; }
                } else if (pr.role == 2) {
                    const double gap = pr.car->gap_ms - player->gap_ms;
                    if (gap > 0) { gapMs = gap;
                        if (gap < 1000) gapColor = kRed;
                        else if (pr.immediate) gapColor = behindIsGaining_ == 1 ? kAmber : kGreen; }
                }
                if (gapMs >= 0) {
                    auto* g = new QLabel(pr.role == 0 ? "+" + fmtSecs(gapMs)
                                                      : "-" + fmtSecs(gapMs));
                    QFont f = g->font(); f.setBold(true); g->setFont(f);
                    QPalette p = g->palette(); p.setColor(QPalette::WindowText, gapColor); g->setPalette(p);
                    rl->addWidget(g);
                }
                sv->addWidget(row);
                if (r + 1 < rows.size()) sv->addWidget(makeHLine());
            }
        } else {
            sv->addWidget(secLabel("No position data", sec, -1));
        }
        sidebarLayout_->addWidget(sect);
        sidebarLayout_->addWidget(makeHLine());
    }

    // Tyre wear
    if (damage.is_object()) {
        auto* sect = new QWidget; auto* sv = new QVBoxLayout(sect);
        sv->setContentsMargins(0, 0, 0, 0); sv->setSpacing(0);
        sv->addWidget(sectionHeader("Tyre Wear", sec));
        auto* grid = new QWidget; auto* gl = new QGridLayout(grid);
        gl->setContentsMargins(0, 0, 0, 0); gl->setSpacing(0);
        struct Corner { const char* label; double value; };
        const Corner corners[4] = {
            { "FL", damage.value("tyre_wear_fl", 0.0) }, { "FR", damage.value("tyre_wear_fr", 0.0) },
            { "RL", damage.value("tyre_wear_rl", 0.0) }, { "RR", damage.value("tyre_wear_rr", 0.0) },
        };
        for (int i = 0; i < 4; ++i) {
            const QColor c = wearColor(corners[i].value);
            auto* cell = new QWidget; auto* cv = new QVBoxLayout(cell);
            cv->setContentsMargins(12, 7, 12, 7); cv->setSpacing(5);
            auto* top = new QHBoxLayout; top->setContentsMargins(0, 0, 0, 0); top->setSpacing(6);
            top->addWidget(secLabel(corners[i].label, sec, 7));
            top->addStretch();
            auto* val = new QLabel(QString("%1%").arg((int)std::lround(corners[i].value)));
            { QFont f = val->font(); f.setBold(true); val->setFont(f);
              QPalette p = val->palette(); p.setColor(QPalette::WindowText, c); val->setPalette(p); }
            top->addWidget(val);
            cv->addLayout(top);
            auto* bar = new QProgressBar; bar->setRange(0, 100);
            bar->setValue((int)std::min(100.0, corners[i].value));
            bar->setTextVisible(false); bar->setFixedHeight(4);
            bar->setStyleSheet(QString(
                "QProgressBar{ background:%1; border:none; border-radius:2px; }"
                "QProgressBar::chunk{ background:%2; border-radius:2px; }")
                .arg(rgba(sec, 0.25)).arg(c.name()));
            cv->addWidget(bar);
            gl->addWidget(cell, i / 2, i % 2);
        }
        sv->addWidget(grid);

        for (const auto& w : sd.wearWarnings) {
            auto* warn = new QWidget; auto* wv = new QVBoxLayout(warn);
            wv->setContentsMargins(12, 6, 12, 6); wv->setSpacing(2);
            auto* t = new QLabel(QString("⚠ %1").arg(w.text));
            t->setWordWrap(true);
            { QFont f = t->font(); f.setBold(true); t->setFont(f);
              QPalette p = t->palette(); p.setColor(QPalette::WindowText, w.color); t->setPalette(p); }
            wv->addWidget(t);
            auto* d = new QLabel(w.detail); d->setWordWrap(true);
            { QPalette p = d->palette(); p.setColor(QPalette::WindowText, sec); d->setPalette(p); }
            wv->addWidget(d);
            sv->addWidget(makeHLine());
            sv->addWidget(warn);
        }
        sidebarLayout_->addWidget(sect);
    }

    sidebarLayout_->addStretch();
    // Sidebar width is fixed at construction with the scrollbar gutter reserved, so
    // the scrollbar shows/hides with no layout shift — nothing to do here per tick.
}
