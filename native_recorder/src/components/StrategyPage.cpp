#include "StrategyPage.h"
#include "TyreHelpers.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QProgressBar>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QBrush>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <vector>

using nlohmann::json;

// ─── File-local data + helpers ─────────────────────────────────────────────

namespace {

constexpr double CONS_PIT_COST_MS = 30000.0;
constexpr double CALL_PIT_COST_MS = 22000.0;
constexpr double TARGET_PIT_COST_MS = 20000.0;
constexpr double PIT_OUTLAP_COST_MS = 25000.0;  // boxing time added to the out-lap; promote to a setting later

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
    return QString("%1:%2").arg(mins).arg(secs, 4, 'f', 1, QChar('0'));
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

    auto makeColumn = [&](QVBoxLayout*& outLayout) {
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* content = new QWidget;
        outLayout = new QVBoxLayout(content);
        outLayout->setContentsMargins(0, 0, 0, 0);
        outLayout->setSpacing(0);
        outLayout->addStretch();
        scroll->setWidget(content);
        return scroll;
    };

    bl->addWidget(makeColumn(consLayout_), 1);
    bl->addWidget(vline());
    bl->addWidget(makeColumn(aggLayout_), 1);
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
    sidebar->setFixedWidth(230);
    {
        auto* content = new QWidget;
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
    pitCounts_.clear();
    prevDriverStatus_.clear();
    rivalAheadIdx_ = rivalBehindIdx_ = -1;
    rivalsLatched_ = false;
    prevLapNum_ = -1;
    consFrozenReqMs_.clear();
    aggFrozenReqMs_.clear();
    consPast_.clear();
    aggPast_.clear();
    consDisplay_.clear();
    aggDisplay_.clear();
    tableLapComputed_ = -1;
    stratColumnsBuilt_ = false;
    lastStratValid_ = false;
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

// Signed delta text (+slower / −faster), matching the stint-target delta styling.
static QString fmtDelta(double ms) {
    return QString("%1%2s").arg(ms > 0 ? "+" : "−").arg(std::abs(ms / 1000.0), 0, 'f', 1);
}

// Build one strategy column (header + stint rows). Renders completed, current and
// future stints alike — completed ones are kept on screen, not dropped after a pit.
static QWidget* buildStintTimeline(int stops, bool isMonaco, const QString& label,
                                   const QColor& accent, const QColor& sec,
                                   const std::vector<DisplayStint>& stints) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);

    // Headline row
    auto* hdr = new QWidget; auto* hl = new QHBoxLayout(hdr);
    hl->setContentsMargins(12, 5, 12, 5); hl->setSpacing(8);
    hl->addWidget(secLabel(label.toUpper(), sec, 7));
    if (isMonaco) hl->addWidget(makeChip("Monaco 2-stop", kAmber));
    hl->addStretch();
    auto* stopsLbl = new QLabel(QString::number(stops));
    { QFont f = stopsLbl->font(); f.setPointSize(14); f.setBold(true); stopsLbl->setFont(f);
      QPalette p = stopsLbl->palette(); p.setColor(QPalette::WindowText, accent); stopsLbl->setPalette(p); }
    hl->addWidget(stopsLbl, 0, Qt::AlignBottom);
    hl->addWidget(secLabel(stops == 1 ? "stop" : "stops", sec), 0, Qt::AlignBottom);
    v->addWidget(hdr);
    v->addWidget(makeHLine());

    for (size_t i = 0; i < stints.size(); ++i) {
        const DisplayStint& st = stints[i];
        // Row 1: compound + laps
        auto* r1 = new QWidget; auto* r1l = new QHBoxLayout(r1);
        r1l->setContentsMargins(12, 6, 12, 6); r1l->setSpacing(8);
        r1l->addWidget(makeChip(st.compoundName, st.color, 10));
        auto* laps = new QLabel(QString("~%1L").arg(st.lapCount));
        { QFont f = laps->font(); f.setBold(true); laps->setFont(f); }
        r1l->addWidget(laps);
        r1l->addWidget(secLabel(QString("%1–%2").arg(st.startLap).arg(st.endLap), sec, 7), 1);
        if (!st.wornText.isEmpty()) r1l->addWidget(secLabel(st.wornText, sec, 7));
        v->addWidget(r1);

        // Row 2: connector + target
        auto* r2 = new QWidget; auto* r2l = new QHBoxLayout(r2);
        r2l->setContentsMargins(12, 3, 12, 3); r2l->setSpacing(10);
        auto* conn = new QLabel(st.isLast ? QString("Finish · Lap %1").arg(st.endLap)
                                          : QString("Pit · Lap %1").arg(st.endLap));
        { QFont f = conn->font(); f.setPointSize(7); f.setBold(true); conn->setFont(f);
          QPalette p = conn->palette(); p.setColor(QPalette::WindowText, st.isLast ? kGreen : kAmber);
          conn->setPalette(p); }
        r2l->addWidget(conn);
        r2l->addStretch();
        if (st.targetPresent) {
            r2l->addWidget(secLabel(st.isEstimate ? "EST. PACE" : "TARGET", sec, 7));
            auto* tm = new QLabel(fmtLapTime(st.targetMs));
            { QFont f = tm->font(); f.setBold(true); tm->setFont(f);
              if (st.isEstimate) { QPalette p = tm->palette(); p.setColor(QPalette::WindowText, sec); tm->setPalette(p); } }
            r2l->addWidget(tm);
            if (st.hasDelta) {
                if (std::abs(st.deltaMs) > 50) {
                    auto* d = new QLabel(fmtDelta(st.deltaMs));
                    QFont f = d->font(); f.setBold(true); d->setFont(f);
                    QPalette p = d->palette(); p.setColor(QPalette::WindowText, st.deltaMs > 0 ? kRed : kGreen);
                    d->setPalette(p);
                    r2l->addWidget(d);
                } else {
                    r2l->addWidget(secLabel("on target", sec, 7));
                }
            }
        }
        v->addWidget(r2);

        // Per-lap target table for this stint. Native OS styling — only the delta
        // cells get a colour (red slower / green faster).
        if (!st.rows.empty()) {
            const std::vector<LapRow>& rows = st.rows;
            const QStringList heads{"LAP", "REQ", "ADJ REQ", "ACTUAL", "Δ LAP", "Δ STINT"};

            auto* tbl = new QTableWidget((int)rows.size(), (int)heads.size());
            tbl->setHorizontalHeaderLabels(heads);
            tbl->verticalHeader()->setVisible(false);
            tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);

            for (int r = 0; r < (int)rows.size(); ++r) {
                const LapRow& lr = rows[r];
                tbl->setItem(r, 0, new QTableWidgetItem(QString::number(lr.lapNum)));
                tbl->setItem(r, 1, new QTableWidgetItem(fmtLapTime(lr.requiredMs)));
                tbl->setItem(r, 2, new QTableWidgetItem(fmtLapTime(lr.actualRequiredMs)));
                tbl->setItem(r, 3, new QTableWidgetItem(lr.hasActual ? fmtLapTime(lr.actualMs) : "—"));
                auto* dl = new QTableWidgetItem(lr.hasActual ? fmtDelta(lr.deltaLapMs)   : "—");
                auto* ds = new QTableWidgetItem(lr.hasActual ? fmtDelta(lr.deltaStintMs) : "—");
                if (lr.hasActual) {
                    dl->setForeground(QBrush(lr.deltaLapMs   > 0 ? kRed : kGreen));
                    ds->setForeground(QBrush(lr.deltaStintMs > 0 ? kRed : kGreen));
                }
                tbl->setItem(r, 4, dl);
                tbl->setItem(r, 5, ds);
            }

            // Size to content so every lap shows inline (no inner scrollbar). Use
            // style metrics (valid before the widget is shown) rather than laid-out
            // geometry, which is provisional pre-show and caused height flicker.
            tbl->ensurePolished();
            const int rowH = tbl->verticalHeader()->defaultSectionSize();
            const int hdrH = tbl->horizontalHeader()->sizeHint().height();
            tbl->setFixedHeight(hdrH + (int)rows.size() * rowH + 2 * tbl->frameWidth());

            v->addWidget(tbl);
        }

        if (i + 1 < stints.size()) v->addWidget(makeHLine());
    }

    v->addStretch();
    return w;
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
    const bool hasTyreData = status.is_object() && status.value("tyre_age_laps", 0) >= 1;
    const int  trackId = session.is_object() ? session.value("track_id", -1) : -1;
    const int  weather = session.is_object() ? session.value("weather", 0) : 0;

    const auto sets   = parseSets(tyreSets);
    const auto cars   = parseTiming(timing);
    const int  playerIdx = timing.is_object() ? timing.value("player_idx", -1) : -1;

    // ── Strategy calculation ────────────────────────────────────────────
    StrategyData sd;
    if (hasLap && session.is_object() && status.is_object() && damage.is_object() &&
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

        const int consLapsLeft = std::max(0, (int)std::floor((cliffPct - avgWear) / wearPerLap));
        const int consCliffLap = lapNum + consLapsLeft;

        const Set* fittedSet = nullptr;
        for (const auto& s : sets) if (s.fitted) { fittedSet = &s; break; }
        const double fittedDelta = fittedSet ? fittedSet->lap_delta_ms : 0.0;
        const double freshGainMs = !consPool.empty()
            ? std::max(0.0, fittedDelta - consPool.front().lap_delta_ms) : 0.0;
        const int consFirstPit = (freshGainMs > 500 && freshGainMs * consLapsLeft > CONS_PIT_COST_MS)
            ? std::max(lapNum + 1, lapNum + (int)std::ceil(CONS_PIT_COST_MS / freshGainMs))
            : consCliffLap;

        StrategyResult conservative = buildStints(lapNum, totalLaps, consFirstPit, cur, avgWear, consPool, priCol);
        if (isMonaco)
            conservative = applyMonacoRule(conservative, consPool, usedCompoundsOf(conservative), priCol);

        const double aggWearRate = wearPerLap * 1.2;
        const int aggLapsLeft = std::max(0, (int)std::floor((cliffPct - avgWear) / aggWearRate));
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

    // ── Pit-count tracking ──────────────────────────────────────────────
    if (!cars.empty() && hasLap) {
        for (const auto& c : cars) {
            const int prev = prevDriverStatus_.count(c.idx) ? prevDriverStatus_[c.idx] : -1;
            if (prev == 2 && c.driver_status == 3 && lapNum > 1)
                pitCounts_[c.idx] = (pitCounts_.count(c.idx) ? pitCounts_[c.idx] : 0) + 1;
            prevDriverStatus_[c.idx] = c.driver_status;
        }
    }

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
                const double perLapOffset = aggressive ? TARGET_PIT_COST_MS / totalRemaining : 0.0;
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
        if (!past.empty() && openingStart <= past.back().startLap) return false;   // same stint
        if (!frozenMap.count(openingStart)) frozenMap[openingStart] = targets[0].targetMs;
        PastStint ps;
        ps.startLap     = openingStart;
        ps.reqBaseMs    = frozenMap[openingStart];
        ps.compoundName = res.stints[0].compoundName;
        ps.color        = res.stints[0].color;
        ps.postPit      = !past.empty();
        past.push_back(ps);
        return true;
    };
    bool stintsChanged = false;
    stintsChanged |= recordStint(sd.conservative, consFrozenReqMs_, consPast_, consTargets);
    stintsChanged |= recordStint(sd.aggressive,  aggFrozenReqMs_, aggPast_,  aggTargets);

    auto computeRows = [&](int start, int end, double base, bool postPit) {
        std::vector<LapRow> rows;
        double cum = 0.0;   // cumulative delta over stint, through the previous lap
        for (int n = start; n > 0 && n <= end; ++n) {
            LapRow lr;
            lr.lapNum = n;
            lr.requiredMs = base + ((postPit && n == start) ? PIT_OUTLAP_COST_MS : 0.0);
            lr.actualRequiredMs = lr.requiredMs + cum;
            auto it = lapTimesByNum.find(n);
            if (n < lapNum && it != lapTimesByNum.end() && it->second > 0) {
                lr.hasActual    = true;
                lr.actualMs     = it->second;
                lr.deltaLapMs   = lr.actualMs - lr.requiredMs;
                cum            += lr.deltaLapMs;
                lr.deltaStintMs = cum;
            }
            rows.push_back(lr);
        }
        return rows;
    };

    auto buildDisplay = [&](const StrategyResult& res, const std::vector<StintTarget>& targets,
                            std::map<int, double>& frozenMap, const std::vector<PastStint>& past) {
        std::vector<DisplayStint> out;
        if (!sd.valid) return out;
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
            d.startLap = start; d.endLap = end; d.lapCount = end - start + 1;
            d.targetPresent = true; d.targetMs = ps.reqBaseMs;
            d.rows = computeRows(start, end, ps.reqBaseMs, ps.postPit);
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
            d.startLap = start; d.endLap = end;
            d.lapCount = isOpening ? std::max(1, end - start + 1) : st.lapCount;
            d.isLast = st.isLast;
            d.wornText = isOpening ? QString("%1% worn").arg((int)std::lround(sd.avgWear)) : QString("Fresh");
            if (tg.present && tg.targetMs > 0) {
                const bool postPit = isOpening ? pittedBefore : true;
                const double base = frozenMap.count(start) ? frozenMap[start] : tg.targetMs;
                d.targetPresent = true; d.isEstimate = tg.isEstimate; d.hasDelta = tg.hasDelta;
                d.targetMs = tg.targetMs; d.deltaMs = tg.deltaMs;
                d.rows = computeRows(start, end, base, postPit);
            }
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
            const int crossover = freshGainMs > 0 ? (int)std::ceil(CALL_PIT_COST_MS / freshGainMs) : 999;
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

    const QColor wearC = wearColor(sd.avgWear);
    compoundChip_->setText(sd.valid ? sd.currentCompound.name : "—");
    {
        const QColor cc = sd.valid ? sd.currentCompound.color : sec;
        compoundChip_->setStyleSheet(QString(
            "QLabel{ color:%1; border:1px solid %1; border-radius:4px; padding:1px 6px;"
            " font-weight:bold; font-size:10px; background:%2; }")
            .arg(cc.name()).arg(rgba(cc, 0.1)));
    }
    wearPct_->setText(QString("%1%").arg((int)std::lround(sd.avgWear)));
    { QPalette p = wearPct_->palette(); p.setColor(QPalette::WindowText, wearC); wearPct_->setPalette(p); }
    wearAge_->setText(status.is_object()
        ? QString("%1L · %2%/L").arg(status.value("tyre_age_laps", 0))
            .arg(sd.valid ? QString::number(sd.wearPerLap, 'f', 1) : QString("—"))
        : QString("—"));
    wearBar_->setValue((int)std::min(100.0, sd.avgWear));
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

    // Rebuild the (heavy) stint columns only when their content actually changes —
    // not every tick — otherwise the QTableWidgets churn and the scrollbar flickers.
    if (!stratColumnsBuilt_ || displayChanged || sd.valid != lastStratValid_) {
        // Conservative column
        clearColumn(consLayout_);
        if (sd.valid) {
            consLayout_->addWidget(buildStintTimeline(sd.conservative.stops, sd.isMonaco,
                                                      "Conservative", blueAccent, sec, consDisplay_));
        } else {
            auto* wait = new QLabel("Waiting for tyre data…");
            wait->setAlignment(Qt::AlignCenter);
            QPalette p = wait->palette(); p.setColor(QPalette::WindowText, sec); wait->setPalette(p);
            consLayout_->addWidget(wait);
        }
        consLayout_->addStretch();

        // Aggressive column
        clearColumn(aggLayout_);
        if (sd.valid)
            aggLayout_->addWidget(buildStintTimeline(sd.aggressive.stops, sd.isMonaco,
                                                    "Aggressive", amberAccent, sec, aggDisplay_));
        aggLayout_->addStretch();

        stratColumnsBuilt_ = true;
        lastStratValid_    = sd.valid;
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
        auto* gap = new QLabel(isUnder ? QString("+%1s ahead").arg(callGap / 1000.0, 0, 'f', 1)
                                       : QString("−%1s behind").arg(callGap / 1000.0, 0, 'f', 1));
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
                if (rr.ahead) { const double g = player->gap_ms - car->gap_ms; if (g > 0) gapStr = QString("+%1s").arg(g / 1000.0, 0, 'f', 1); }
                else          { const double g = car->gap_ms - player->gap_ms; if (g > 0) gapStr = QString("−%1s").arg(g / 1000.0, 0, 'f', 1); }
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

                const int stops = pitCounts_.count(pr.car->idx) ? pitCounts_[pr.car->idx] : 0;
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
                    auto* g = new QLabel(pr.role == 0 ? QString("+%1s").arg(gapMs / 1000.0, 0, 'f', 1)
                                                      : QString("-%1s").arg(gapMs / 1000.0, 0, 'f', 1));
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
}
