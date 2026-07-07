#include "OverviewPage.h"
#include "../CompactSettings.h"
#include "../Labels.h"
#include "../TelemetryChart.h"
#include "../SessionModel.h"
#include "CardColors.h"
#include "PageUiHelpers.h"
#include "TyreCardsWidget.h"
#include "TyreChartsWidget.h"
#include "TyreHelpers.h"
#include "GraphTable.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>
#include <QSizePolicy>
#include <QPushButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QSignalBlocker>
#include <QGridLayout>
#include <QApplication>
#include <QLocale>

#include <algorithm>

#include <cmath>

// ── UI helpers ────────────────────────────────────────────────────────────

namespace {

// Fixed height for the tyre-cards widget at each density level (see
// TyreCardsWidget::Level): Full keeps the tall stacked cards; the compact levels
// shrink as rows are dropped (Ultra Compact 1/2 are a single value row).
int tyreCardsHeight(int level) {
    switch (level) {
        case TyreCardsWidget::Compact:       return 44;
        case TyreCardsWidget::UltraCompact1: return 30;
        case TyreCardsWidget::UltraCompact2: return 24;
        case TyreCardsWidget::UltraCompact3: return 24;
        default:                             return 160;   // Full
    }
}

// Remove and delete every item in a layout so a card row can be rebuilt in place
// (used when compact mode toggles at runtime). The child widgets are deleted, not
// just detached, so the stale card frames don't linger under the new ones.
void clearLayout(QLayout* lay) {
    if (!lay) return;
    while (QLayoutItem* item = lay->takeAt(0)) {
        if (QWidget* w = item->widget()) delete w;
        delete item;
    }
}

// subOut, when non-null, receives a small bold label carrying the per-card extra
// info the Electron app shows as a sub-row under the value (ERS mode, fuel
// "vs fin", lap, tyre age). In the full (two-line) layout it's pinned to the
// top-right of the heading row; in compact mode it sits in the card's right zone.
//
// Compact collapses the card to one line — [label] · value+unit (middle) · [sub]
// (right) — trading vertical space for a shorter row.
QFrame* makeStatCard(const QString& label, const QString& unit, QLabel*& valueOut,
                     bool compact, QLabel** subOut = nullptr, QLabel** titleOut = nullptr) {
    QFrame* card = new QFrame;
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    QLabel* lbl = new QLabel(label.toUpper());
    QFont lf; lf.setPointSize(compact ? 8 : 7);
    lbl->setFont(lf);
    lbl->setForegroundRole(QPalette::PlaceholderText);
    if (titleOut) *titleOut = lbl;   // expose the title so it can be re-labelled on format change

    valueOut = new QLabel("—");
    QFont vf; vf.setPointSize(compact ? 12 : 15); vf.setBold(true);
    valueOut->setFont(vf);

    QLabel* ulbl = nullptr;
    if (!unit.isEmpty()) {
        ulbl = new QLabel(unit);
        QFont uf; uf.setPointSize(compact ? 8 : 7);
        ulbl->setFont(uf);
        ulbl->setForegroundRole(QPalette::PlaceholderText);
    }
    QLabel* sub = nullptr;
    if (subOut) {
        sub = new QLabel;
        QFont sf; sf.setPointSize(compact ? 8 : 7); sf.setBold(true);
        sub->setFont(sf);
        sub->setForegroundRole(QPalette::PlaceholderText);
        *subOut = sub;
    }

    if (compact) {
        // One line. Three-value cards (those carrying extra info) read
        // label · value+unit (centred) · info (right); two-value cards drop the
        // trailing stretch so the value pins to the right edge.
        QHBoxLayout* cl = new QHBoxLayout(card);
        cl->setContentsMargins(8, 3, 8, 3);
        cl->setSpacing(4);
        cl->addWidget(lbl);
        cl->addStretch();
        cl->addWidget(valueOut);
        if (ulbl) cl->addWidget(ulbl);
        if (sub) {
            cl->addStretch();
            cl->addWidget(sub);
        }
        return card;
    }

    QVBoxLayout* cv = new QVBoxLayout(card);
    cv->setContentsMargins(8, 6, 8, 6);
    cv->setSpacing(1);

    // Heading row: title on the left, optional sub-info pinned to the right.
    QWidget* hdrRow = new QWidget;
    QHBoxLayout* hh = new QHBoxLayout(hdrRow);
    hh->setContentsMargins(0, 0, 0, 0);
    hh->setSpacing(4);
    hh->addWidget(lbl);
    hh->addStretch();
    if (sub) hh->addWidget(sub);

    QWidget* valRow = new QWidget;
    QHBoxLayout* hl = new QHBoxLayout(valRow);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(4);
    hl->addWidget(valueOut);
    if (ulbl) hl->addWidget(ulbl);
    hl->addStretch();

    cv->addWidget(hdrRow);
    cv->addWidget(valRow);
    return card;
}

QFrame* makeDmgCard(const QString& label, QLabel*& valueOut, bool compact) {
    QFrame* card = new QFrame;
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    QLabel* lbl = new QLabel(label.toUpper());
    QFont lf; lf.setPointSize(compact ? 7 : 6);
    lbl->setFont(lf);
    lbl->setForegroundRole(QPalette::PlaceholderText);

    valueOut = new QLabel("—");
    QFont vf; vf.setPointSize(compact ? 11 : 12); vf.setBold(true);
    valueOut->setFont(vf);

    if (compact) {
        // One line, two-value: label left, value pinned right.
        QHBoxLayout* cl = new QHBoxLayout(card);
        cl->setContentsMargins(6, 2, 6, 2);
        cl->setSpacing(4);
        cl->addWidget(lbl);
        cl->addStretch();
        cl->addWidget(valueOut);
        return card;
    }

    QVBoxLayout* cv = new QVBoxLayout(card);
    cv->setContentsMargins(6, 4, 6, 4);
    cv->setSpacing(0);
    cv->addWidget(lbl);
    cv->addWidget(valueOut);
    return card;
}

void setDmgValue(QLabel* lbl, int val) {
    if (val < 0) { lbl->setText("—"); lbl->setStyleSheet(""); return; }
    lbl->setText(QString::number(val));
    lbl->setStyleSheet(val == 0 ? "color: #37872D;" : "color: #C4162A;");
}

} // namespace

// ── Overview page ─────────────────────────────────────────────────────────

OverviewPage::OverviewPage(SessionModel* model, QWidget* parent)
    : QWidget(parent)
{
    statsCompact_  = settings_.value(tnr::compactKey(tnr::CompactSection::OverviewStats),  false).toBool();
    damageCompact_ = settings_.value(tnr::compactKey(tnr::CompactSection::OverviewDamage), false).toBool();
    tyresLevel_    = settings_.value(tnr::compactKey(tnr::CompactSection::OverviewTyres),  0).toInt();

    QVBoxLayout* vbox = new QVBoxLayout(this);
    // No outer padding so the separator lines reach every edge; the inset is
    // re-added to the inner rows below (the chart keeps its own 8px L/R inset).
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // ── Stats row ────────────────────────────────────────────────
    statsFrame_ = new QFrame;
    QHBoxLayout* sh = new QHBoxLayout(statsFrame_);
    sh->setContentsMargins(8, 0, 8, 0);   // L/R inset; lines above/below reach edges
    sh->setSpacing(0);
    buildStatCards();   // populates statsFrame_'s layout (rebuilt on compact toggle)

    vbox->addWidget(statsFrame_);

    sep1_ = tnrui::hline();
    vbox->addWidget(sep1_);

    // ── Chart mode controls ──────────────────────────────────────
    modeBar_ = new QWidget;
    QHBoxLayout* mb = new QHBoxLayout(modeBar_);
    mb->setContentsMargins(8, 2, 8, 2);
    mb->setSpacing(4);

    auto* modeGroup = new QButtonGroup(modeBar_);
    modeGroup->setExclusive(true);
    struct { const char* label; ChartMode mode; } modes[] = {
        { "Default",      ChartMode::Default     },
        { "Current Lap",  ChartMode::CurrentLap  },
        { "Previous Lap", ChartMode::PreviousLap },
        { "Fastest Lap",  ChartMode::FastestLap  },
        { "Compare",      ChartMode::Compare     },
    };
    // Same flat, underline-on-active style as the page switcher in the toolbar,
    // instead of these being raised/boxed buttons. Every button — checked or
    // not — reserves the same border-bottom width (transparent unless checked),
    // so switching the active one only changes its color, not the row's height.
    const QString accent = QApplication::palette().color(QPalette::Highlight).name();
    const QString modeBtnStyle = QString(
        "QPushButton { padding: 6px 12px; border: none; background: transparent;"
        " border-bottom: 2px solid transparent; }"
        "QPushButton:checked { border-bottom: 2px solid %1; }"
    ).arg(accent);
    for (const auto& m : modes) {
        QPushButton* b = new QPushButton(m.label);
        b->setCheckable(true);
        b->setFlat(true);
        b->setChecked(m.mode == ChartMode::Default);
        b->setStyleSheet(modeBtnStyle);
        modeGroup->addButton(b);
        mb->addWidget(b);
        // Compare needs a loaded recording's laps; disabled until one is opened.
        if (m.mode == ChartMode::Compare) { compareBtn_ = b; b->setEnabled(false); }
        if (m.mode == ChartMode::Default) defaultBtn_ = b;
        ChartMode mode = m.mode;
        connect(b, &QPushButton::clicked, this, [this, mode] {
            if (chart_) chart_->setMode(mode);
            if (lapCombo_) lapCombo_->setVisible(mode == ChartMode::Compare);
        });
    }

    lapCombo_ = new QComboBox;
    lapCombo_->setVisible(false);
    lapCombo_->setMinimumWidth(70);
    mb->addWidget(lapCombo_);
    mb->addStretch(1);

    // Chart legend, pinned to the right of this row (the chart's own overlay
    // legend is hidden) so it never covers the traces. A short coloured line marker
    // (matching the line-style tyre-chart key) + name, sourced from the chart so
    // colours stay in sync. The larger em dash renders as a bold line sample.
    for (const auto& e : TelemetryChart::legendEntries()) {
        QLabel* item = new QLabel(
            QString("<span style='color:%1; font-size:16px'>—</span> %2").arg(e.color.name(), e.name));
        item->setTextFormat(Qt::RichText);
        mb->addWidget(item);
    }

    vbox->addWidget(modeBar_);

    connect(lapCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (chart_ && idx >= 0)
            chart_->setCompareLap(lapCombo_->itemData(idx).toInt());
    });

    // ── Chart ────────────────────────────────────────────────────
    // Wrapped so it keeps the same 8px L/R inset as the rows above/below now that
    // the outer layout has no margins.
    model_ = model;
    chart_ = new TelemetryChart;
    chart_->setModel(model);
    // The Settings "Graphs" tab can swap the Speed/RPM/ERS chart for its underlying
    // samples. Set up exactly like the other pages' graph tables: a plain GraphTable
    // in a QGridLayout on a plain QWidget, with the chart and table sharing the one
    // cell and toggled by setTelemetryTable. (A QStackedWidget was tried here but it
    // fills its own background, which squared off the table's rounded frame corners —
    // the grid host is transparent, so the table renders identically to the others.)
    telemetryTable_ = new GraphTable({ { "Time",        GraphTable::Time },
                                       { "Speed (kph)", GraphTable::Fixed0 },
                                       { "RPM",         GraphTable::Fixed0 },
                                       { "ERS (%)",     GraphTable::Fixed1 } });
    QWidget* chartHost = new QWidget;
    QGridLayout* chartHostLay = new QGridLayout(chartHost);
    chartHostLay->setContentsMargins(0, 8, 0, 8);   // top/bottom breathing room
    chartHostLay->setSpacing(0);
    chartHostLay->addWidget(chart_,          0, 0);
    chartHostLay->addWidget(telemetryTable_, 0, 0);
    telemetryTable_->hide();   // chart shown by default; setTelemetryTable swaps them
    QWidget* chartWrap = new QWidget;
    QVBoxLayout* chartWrapLay = new QVBoxLayout(chartWrap);
    chartWrapLay->setContentsMargins(8, 0, 8, 0);
    chartWrapLay->setSpacing(0);
    chartWrapLay->addWidget(chartHost);
    vbox->addWidget(chartWrap, 1);

    // Keep the telemetry table live while it's the visible page.
    connect(model, &SessionModel::telemetryAppended, this, [this] {
        if (telemetryTableMode_) refreshTelemetryTable();
    });
    connect(model, &SessionModel::wasReset, this, [this] {
        if (telemetryTableMode_) refreshTelemetryTable();
    });

    // Repopulate the compare-lap selector whenever the set of laps changes.
    connect(model, &SessionModel::lapsChanged, this, [this, model] {
        if (!lapCombo_) return;
        const int prev = lapCombo_->count() > 0 && lapCombo_->currentIndex() >= 0
            ? lapCombo_->currentData().toInt() : -1;
        QSignalBlocker block(lapCombo_);
        lapCombo_->clear();
        for (const LapBlock& l : model->data().laps)
            lapCombo_->addItem(QString("Lap %1").arg(l.lapNum), l.lapNum);
        int sel = lapCombo_->findData(prev);
        if (sel < 0 && lapCombo_->count() > 0) sel = 0;   // no prior selection — default to the first lap
        if (sel >= 0) lapCombo_->setCurrentIndex(sel);
        // QSignalBlocker above suppresses currentIndexChanged, so the chart never
        // learns about this selection on its own — sync it explicitly.
        if (chart_ && sel >= 0) chart_->setCompareLap(lapCombo_->itemData(sel).toInt());
    });

    // ── Tyre section ─────────────────────────────────────────────
    // Wrapped in a 0-spacing container so the separators sit flush against the
    // charts. The page vbox's 6px spacing would otherwise leave a gap between the
    // charts and the separators, so the charts' vertical dividers (which run to the
    // widget edge) wouldn't meet the horizontal separators above/below.
    QWidget* tyreSection = new QWidget;
    QVBoxLayout* tyreLay = new QVBoxLayout(tyreSection);
    tyreLay->setContentsMargins(0, 0, 0, 0);
    tyreLay->setSpacing(0);

    tyreSep_ = tnrui::hline();
    tyreLay->addWidget(tyreSep_);

    tyreCards_ = new TyreCardsWidget(Qt::Horizontal);
    tyreCards_->setFixedHeight(tyreCardsHeight(tyresLevel_));
    if (tyresLevel_ != TyreCardsWidget::Full)
        tyreCards_->setLevel(static_cast<TyreCardsWidget::Level>(tyresLevel_));
    tyreLay->addWidget(tyreCards_);

    tyreCharts_ = new TyreChartsWidget;
    tyreCharts_->setFixedHeight(200);
    tyreCharts_->setModel(model);
    tyreCharts_->setTyreLifeMode(tyreGraphLifeMode());
    tyreCharts_->setVisible(false);
    tyreLay->addWidget(tyreCharts_);

    sep2_ = tnrui::hline();
    tyreLay->addWidget(sep2_);

    vbox->addWidget(tyreSection);

    // ── Damage rows ──────────────────────────────────────────────
    dmgFrame_ = new QFrame;
    QVBoxLayout* dv = new QVBoxLayout(dmgFrame_);
    dv->setContentsMargins(0, 0, 0, 0);
    dv->setSpacing(0);

    dmgRowA_ = new QFrame;
    QHBoxLayout* ah = new QHBoxLayout(dmgRowA_);
    ah->setContentsMargins(8, 0, 8, 0);   // L/R inset; the row's lines reach edges
    ah->setSpacing(0);

    dmgHdiv_ = tnrui::hline();

    dmgRowB_ = new QFrame;
    QHBoxLayout* bh = new QHBoxLayout(dmgRowB_);
    bh->setContentsMargins(8, 0, 8, 0);   // L/R inset; the row's lines reach edges
    bh->setSpacing(0);

    buildDamageCards();   // populates both rows (rebuilt on compact toggle)

    dv->addWidget(dmgRowA_);
    dv->addWidget(dmgHdiv_);
    dv->addWidget(dmgRowB_);

    vbox->addWidget(dmgFrame_);

    applyLayout(loadLayout());
}

// Build (or rebuild in place) the key-driven stat cards into statsFrame_'s row.
// Called from the ctor and again on a compact-mode toggle: it clears the row and
// the card/pointer maps first, so the new cards fully replace the old ones.
void OverviewPage::buildStatCards() {
    QHBoxLayout* sh = qobject_cast<QHBoxLayout*>(statsFrame_->layout());
    clearLayout(sh);
    cardValue_.clear();
    cardSub_.clear();
    cardTitle_.clear();
    for (int i = 0; i < OverviewLayout::StatCardCount; ++i) {
        statCardFrame_[i] = nullptr;
        statCardSep_[i]   = nullptr;
    }
    // Compact cards carry their own left margin, so the row's L/R inset would
    // over-indent the first card ("SPEED") relative to the rest — drop it in
    // compact mode; the full two-line layout keeps its original inset.
    sh->setContentsMargins(statsCompact_ ? 0 : 8, 0, statsCompact_ ? 0 : 8, 0);

    // Key-driven stat cards. Each card is { key, label }: title from the i18n
    // catalog (ui.overview.<key>), value/colour from a per-key resolver over the
    // data cache (see refreshCards). The wing card keeps the 'drs'
    // visibility key while its data field is format-aware (drs ↔ slm).
    struct CardDef { OverviewLayout::StatCard idx; const char* unit; bool sub; };
    static const CardDef defs[] = {
        { OverviewLayout::Speed,      "kph", false }, { OverviewLayout::Rpm,        "",    false },
        { OverviewLayout::Gear,       "",    false }, { OverviewLayout::Throttle,   "%",   false },
        { OverviewLayout::Brake,      "%",   false }, { OverviewLayout::Drs,        "",    true  },
        { OverviewLayout::EngineTemp, "°C",  false }, { OverviewLayout::Ers,        "%",   true  },
        { OverviewLayout::Fuel,       "kg",  true  }, { OverviewLayout::Pos,        "",    true  },
        { OverviewLayout::Tyre,       "",    true  },
    };
    bool first = true;
    for (const CardDef& d : defs) {
        const QString key = OverviewLayout::statCardKey(d.idx);
        QLabel* val = nullptr; QLabel* sub = nullptr; QLabel* title = nullptr;
        QFrame* frame = makeStatCard(tnr::L("ui.overview." + key), d.unit,
                                     val, statsCompact_, d.sub ? &sub : nullptr, &title);
        statCardFrame_[d.idx] = frame;
        cardValue_[key] = val;
        cardTitle_[key] = title;
        if (sub) cardSub_[key] = sub;
        if (!first) {
            QFrame* sep = tnrui::vline();
            statCardSep_[d.idx] = sep;   // tracked so applyLayout can hide it with its card
            sh->addWidget(sep);
        }
        first = false;
        sh->addWidget(frame);
    }
}

// Build (or rebuild in place) both damage rows. Compact mode collapses the cards
// to one line, so the rows also shrink from their two-line fixed height.
void OverviewPage::buildDamageCards() {
    clearLayout(dmgRowA_->layout());
    clearLayout(dmgRowB_->layout());
    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i) {
        dmgCardFrame_[i] = nullptr;
        dmgCardSep_[i]   = nullptr;
    }

    const int rowH = damageCompact_ ? 26 : 60;
    dmgRowA_->setFixedHeight(rowH);
    dmgRowB_->setFixedHeight(rowH);

    // Drop the rows' L/R inset in compact mode so the first card ("WING FL") lines
    // up with the rest; the full two-line layout keeps its original inset.
    const int dmgSide = damageCompact_ ? 0 : 8;

    struct DmgDef { int idx; const char* label; QLabel** val; };
    const DmgDef rowA[] = {
        { OverviewLayout::TyreFl,  "Tyre FL",  &dmgTyreFl },  { OverviewLayout::TyreFr,  "Tyre FR",  &dmgTyreFr },
        { OverviewLayout::TyreRl,  "Tyre RL",  &dmgTyreRl },  { OverviewLayout::TyreRr,  "Tyre RR",  &dmgTyreRr },
        { OverviewLayout::BrakeFl, "Brake FL", &dmgBrakeFl }, { OverviewLayout::BrakeFr, "Brake FR", &dmgBrakeFr },
        { OverviewLayout::BrakeRl, "Brake RL", &dmgBrakeRl }, { OverviewLayout::BrakeRr, "Brake RR", &dmgBrakeRr },
    };
    const DmgDef rowB[] = {
        { OverviewLayout::WingFl,   "Wing FL",   &dmgWingFl },   { OverviewLayout::WingFr,  "Wing FR",  &dmgWingFr },
        { OverviewLayout::WingRear, "Wing Rear", &dmgWingRear }, { OverviewLayout::Floor,   "Floor",    &dmgFloor },
        { OverviewLayout::Sidepod,  "Sidepod",   &dmgSidepod },  { OverviewLayout::Diffuser,"Diffuser", &dmgDiffuser },
        { OverviewLayout::Gearbox,  "Gearbox",   &dmgGearbox },  { OverviewLayout::Engine,  "Engine",   &dmgEngine },
    };

    // Cards get equal stretch (all eight the same width, so they line up with the
    // four equal tyre cards above). The separator preceding a card is tracked in
    // dmgCardSep_ so applyLayout can hide it together with a hidden card.
    auto buildRow = [&](QHBoxLayout* rl, const DmgDef* defs, int n) {
        rl->setContentsMargins(dmgSide, 0, dmgSide, 0);
        for (int j = 0; j < n; ++j) {
            if (j > 0) rl->addWidget(dmgCardSep_[defs[j].idx] = tnrui::vline());
            QFrame* card = makeDmgCard(defs[j].label, *defs[j].val, damageCompact_);
            dmgCardFrame_[defs[j].idx] = card;
            rl->addWidget(card, 1);
        }
    };
    buildRow(qobject_cast<QHBoxLayout*>(dmgRowA_->layout()), rowA, 8);
    buildRow(qobject_cast<QHBoxLayout*>(dmgRowB_->layout()), rowB, 8);
}

// Live per-section compact toggles. Each rebuilds only its own row/cards, re-applies
// the layout visibility (the rebuild recreated the frames applyLayout hides), then
// repopulates from the cache so nothing shows a stale "—" while paused.
void OverviewPage::setStatsCompact(bool on) {
    if (statsCompact_ == on) return;
    statsCompact_ = on;
    buildStatCards();
    applyLayout(loadLayout());
    refreshCards();
}

void OverviewPage::setDamageCompact(bool on) {
    if (damageCompact_ == on) return;
    damageCompact_ = on;
    buildDamageCards();
    applyLayout(loadLayout());
    if (!lastDamage_.is_null()) { const nlohmann::json d = lastDamage_; onDamage(d); }
}

void OverviewPage::setTyresLevel(int level) {
    if (tyresLevel_ == level) return;
    tyresLevel_ = level;
    if (tyreCards_) {
        tyreCards_->setFixedHeight(tyreCardsHeight(level));
        tyreCards_->setLevel(static_cast<TyreCardsWidget::Level>(level));   // rebuilds the corner cards; applyLayout re-applies visibility
    }
    applyLayout(loadLayout());
}

// ── Per-row updates (were the MainWindow live/playback signals) ───────────

void OverviewPage::onTelemetry(const nlohmann::json& row) {
    cache_.speed      = row["speed_kph"].get<float>();
    cache_.rpm        = row["rpm"].get<int>();
    cache_.gear       = row["gear"].get<int>();
    cache_.throttle   = row["throttle"].get<float>();
    cache_.brake      = row["brake"].get<float>();
    cache_.drs        = row.value("drs", 0) != 0;
    cache_.slm        = row.value("slm", 0) != 0;
    cache_.engineTemp = row.value("engine_temp", 0);
    refreshCards();
}

void OverviewPage::onStatus(const nlohmann::json& row) {
    cache_.ersPct         = row["ers_pct"].get<float>();
    cache_.ersMode        = row["ers_mode"].get<int>();
    cache_.fuelKg         = row["fuel_kg"].get<float>();
    cache_.fuelLaps       = row["fuel_laps"].get<float>();
    cache_.tyreCompound   = row["tyre_compound"].get<int>();
    cache_.tyreAgeLaps    = row["tyre_age_laps"].get<int>();
    cache_.fuelMix        = row.value("fuel_mix", 0);
    cache_.visualCompound = row.value("visual_compound", 0);
    refreshCards();
}

void OverviewPage::onDamage(const nlohmann::json& row) {
    lastDamage_ = row;   // cached so a compact-mode rebuild can repaint while paused
    setDmgValue(dmgTyreFl,   row.value("tyre_dmg_fl",   0));
    setDmgValue(dmgTyreFr,   row.value("tyre_dmg_fr",   0));
    setDmgValue(dmgTyreRl,   row.value("tyre_dmg_rl",   0));
    setDmgValue(dmgTyreRr,   row.value("tyre_dmg_rr",   0));
    setDmgValue(dmgBrakeFl,  row.value("brake_dmg_fl",  0));
    setDmgValue(dmgBrakeFr,  row.value("brake_dmg_fr",  0));
    setDmgValue(dmgBrakeRl,  row.value("brake_dmg_rl",  0));
    setDmgValue(dmgBrakeRr,  row.value("brake_dmg_rr",  0));
    setDmgValue(dmgWingFl,   row.value("wing_fl",         0));
    setDmgValue(dmgWingFr,   row.value("wing_fr",         0));
    setDmgValue(dmgWingRear, row.value("wing_rear",       0));
    setDmgValue(dmgFloor,    row.value("floor_damage",    0));
    setDmgValue(dmgSidepod,  row.value("sidepod_damage",  0));
    setDmgValue(dmgDiffuser, row.value("diffuser_damage", 0));
    setDmgValue(dmgGearbox,  row.value("gearbox_damage",  0));
    setDmgValue(dmgEngine,   row.value("engine_damage",   0));

    cache_.drsFault = row.value("drs_fault", 0) == 1;
    cache_.ersFault = row.value("ers_fault", 0) == 1;
    refreshCards();
}

void OverviewPage::onLap(const nlohmann::json& row) {
    cache_.pos    = row["position"].get<int>();
    cache_.lapNum = row["lap_num"].get<int>();
    refreshCards();
}

void OverviewPage::updateTyreCards(const nlohmann::json& telemetry, const nlohmann::json& damage) {
    if (tyreCards_) tyreCards_->update(telemetry, damage);
}

void OverviewPage::refreshTitles() {
    for (auto it = cardTitle_.cbegin(); it != cardTitle_.cend(); ++it) {
        if (it.value()) it.value()->setText(tnr::L("ui.overview." + it.key()).toUpper());
    }
}

// Recompute every built overview card's value + colour (+ optional sub) from the
// data cache via the per-key resolvers. Colours come from the shared library spec
// (tnr::cardColor), so thresholds match the Electron app exactly.
void OverviewPage::refreshCards() {
    const OvCache& c = cache_;
    static const char* FUEL_MIX[] = { "Lean", "Std", "Rich", "Max" };

    auto setCard = [this](const QString& key, const QString& value, const QColor& color) {
        if (QLabel* l = cardValue_.value(key)) {
            l->setText(value);
            l->setStyleSheet(tnr::cardColorStyle(color));
        }
    };
    auto setSub = [this](const QString& key, const QString& sub, const QColor& subColor = QColor()) {
        if (QLabel* l = cardSub_.value(key)) {
            l->setText(sub);
            l->setStyleSheet(subColor.isValid() ? QString("color: %1;").arg(subColor.name()) : QString());
        }
    };

    setCard("speed", QString::number((int)c.speed), tnr::cardColor("speed"));
    setCard("rpm",   QLocale().toString(c.rpm),     tnr::cardColor("rpm"));
    setCard("gear",  c.gear <= 0 ? (c.gear < 0 ? QStringLiteral("R") : QStringLiteral("N"))
                                 : QString::number(c.gear),
            tnr::cardColor("gear", c.gear));
    setCard("throttle", QString::number((int)(c.throttle * 100)), tnr::cardColor("throttle"));
    setCard("brake",    QString::number((int)(c.brake * 100)),
            tnr::cardColor("brake", NAN, { {"brake", c.brake} }));

    // Wing card: data field is format-aware (drs in 2025, slm in 2026).
    const bool wingOpen = (tnr::Labels::instance().t("card.wing.key") == "slm") ? c.slm : c.drs;
    setCard("drs", wingOpen ? QStringLiteral("ON") : QStringLiteral("OFF"),
            tnr::cardColor("wing", wingOpen ? 1 : 0));
    setSub("drs", c.drsFault ? QStringLiteral("FAULT") : QString(),
           c.drsFault ? QColor("#C4162A") : QColor());

    setCard("engine", QString::number(c.engineTemp), tnr::cardColor("engine", c.engineTemp));

    setCard("ers", QString::number((int)c.ersPct),
            tnr::cardColor("ers", c.ersPct, { {"ers_mode", (double)c.ersMode}, {"ers_pct", c.ersPct} }));
    if (c.ersFault)
        setSub("ers", QStringLiteral("FAULT"), QColor("#C4162A"));
    else
        setSub("ers", (c.ersMode >= 0 && c.ersMode < 4) ? tnr::Ln("ers.mode", c.ersMode) : QString());

    setCard("fuel", QString::number(c.fuelKg, 'f', 1),
            tnr::cardColor("fuel", NAN, { {"fuel_laps", c.fuelLaps} }));
    setSub("fuel", QString("%1%2 vs fin").arg(c.fuelLaps >= 0 ? "+" : "").arg(c.fuelLaps, 0, 'f', 1));

    setCard("pos", "P" + QString::number(c.pos), QColor());
    setSub("pos", "Lap " + QString::number(c.lapNum));

    setCard("tyre", tyreLabel(c.tyreCompound),
            tnr::cardColor("tyre", NAN, { {"visual_compound", (double)c.visualCompound} }));
    setSub("tyre", QString("%1L · %2")
        .arg(c.tyreAgeLaps).arg(c.fuelMix >= 0 && c.fuelMix < 4 ? FUEL_MIX[c.fuelMix] : ""));
}

// ── Playback plumbing ─────────────────────────────────────────────────────

void OverviewPage::setPlaybackMode(bool on, float currentTime) {
    playback_ = on;
    if (on) {
        currentTime_ = currentTime;
        if (chart_) { chart_->setPlaybackMode(true); chart_->setCurrentTime(currentTime); }
        if (tyreCharts_) { tyreCharts_->setPlaybackMode(true); tyreCharts_->setCurrentTime(currentTime); }
        if (compareBtn_) compareBtn_->setEnabled(true);
    } else {
        if (chart_) { chart_->setPlaybackMode(false); chart_->setMode(ChartMode::Default); }
        if (tyreCharts_) tyreCharts_->setPlaybackMode(false);
        if (compareBtn_) compareBtn_->setEnabled(false);
        if (defaultBtn_) defaultBtn_->setChecked(true);
        if (lapCombo_) lapCombo_->setVisible(false);
    }
    if (telemetryTableMode_) refreshTelemetryTable();
}

void OverviewPage::setCurrentTime(float t) {
    currentTime_ = t;
    if (chart_) chart_->setCurrentTime(t);
    if (tyreCharts_) tyreCharts_->setCurrentTime(t);
    if (telemetryTableMode_) refreshTelemetryTable();
}

void OverviewPage::setWindowSeconds(float secs) {
    windowS_ = secs;
    if (chart_) chart_->setWindowSeconds(secs);
    if (tyreCharts_) tyreCharts_->setWindowSeconds(secs);
    if (telemetryTableMode_) refreshTelemetryTable();
}

void OverviewPage::setTelemetryTable(bool table) {
    telemetryTableMode_ = table;
    if (chart_)          chart_->setVisible(!table);
    if (telemetryTable_) { telemetryTable_->setVisible(table); if (table) telemetryTable_->raise(); }
    // The mode bar (Default/Current Lap/… chart-mode buttons, lap-compare combo and
    // the Speed/RPM/ERS legend) only applies to the chart — hide it in table mode.
    if (modeBar_) modeBar_->setVisible(!table);
    if (table) refreshTelemetryTable();
}

void OverviewPage::setTyreGraphTable(int section, bool table) {
    if (tyreCharts_) tyreCharts_->setSectionViewMode(section, table);
}

void OverviewPage::refreshTelemetryTable() {
    if (!telemetryTable_ || !model_) return;
    const SessionData& d = model_->data();
    const float endTime = playback_ ? currentTime_ : d.latestTime;
    const float left    = endTime - windowS_;

    // ERS lives in stsBuf, sampled independently of telBuf — match each telemetry
    // sample to the most recent status sample at or before it.
    auto ersAt = [&](float t) -> float {
        const auto& s = d.stsBuf;
        if (s.isEmpty()) return 0.0f;
        auto it = std::upper_bound(s.begin(), s.end(), t,
            [](float key, const StsSample& x) { return key < x.t; });
        if (it == s.begin()) return it->ers;
        return (it - 1)->ers;
    };

    telemetryTable_->beginRebuild();
    for (int i = d.telBuf.size() - 1; i >= 0 && !telemetryTable_->full(); --i) {
        const auto& s = d.telBuf[i];
        if (s.t > endTime) continue;
        if (s.t < left)    break;
        telemetryTable_->addRow(s.t, s.speed, s.rpm, ersAt(s.t));
    }
    telemetryTable_->endRebuild();
}

// ── Layout persistence ────────────────────────────────────────────────────

OverviewLayout OverviewPage::loadLayout()
{
    OverviewLayout L;
    settings_.beginGroup("overviewLayout");
    L.showChart       = settings_.value("showChart",       true).toBool();
    L.tyreView = settings_.value("tyreView", 0).toInt() == 1
                   ? OverviewLayout::TyreCharts : OverviewLayout::TyreCards;
    settings_.beginGroup("tyreCards");
    for (int i = 0; i < OverviewLayout::TyreCornerCount; ++i)
        L.tyreCardVisible[i] = settings_.value(OverviewLayout::tyreCardKey(i), true).toBool();
    settings_.endGroup();
    settings_.beginGroup("tyreCharts");
    for (int i = 0; i < OverviewLayout::TyreChartCount; ++i)
        L.tyreChartVisible[i] = settings_.value(OverviewLayout::tyreChartKey(i), true).toBool();
    settings_.endGroup();
    settings_.beginGroup("statCards");
    for (int i = 0; i < OverviewLayout::StatCardCount; ++i)
        L.statCards[i] = settings_.value(OverviewLayout::statCardKey(i), true).toBool();
    settings_.endGroup();
    settings_.beginGroup("dmgCards");
    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i) {
        const bool defaultHidden = i < OverviewLayout::WingFl;   // tyre/brake row defaults hidden
        L.dmgCards[i] = settings_.value(OverviewLayout::dmgCardKey(i), !defaultHidden).toBool();
    }
    settings_.endGroup();
    settings_.endGroup();
    return L;
}

void OverviewPage::saveLayout(const OverviewLayout& L)
{
    settings_.beginGroup("overviewLayout");
    settings_.setValue("showChart", L.showChart);
    settings_.setValue("tyreView",  (int)L.tyreView);
    settings_.beginGroup("tyreCards");
    for (int i = 0; i < OverviewLayout::TyreCornerCount; ++i)
        settings_.setValue(OverviewLayout::tyreCardKey(i), L.tyreCardVisible[i]);
    settings_.endGroup();
    settings_.beginGroup("tyreCharts");
    for (int i = 0; i < OverviewLayout::TyreChartCount; ++i)
        settings_.setValue(OverviewLayout::tyreChartKey(i), L.tyreChartVisible[i]);
    settings_.endGroup();
    settings_.beginGroup("statCards");
    for (int i = 0; i < OverviewLayout::StatCardCount; ++i)
        settings_.setValue(OverviewLayout::statCardKey(i), L.statCards[i]);
    settings_.endGroup();
    settings_.beginGroup("dmgCards");
    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i)
        settings_.setValue(OverviewLayout::dmgCardKey(i), L.dmgCards[i]);
    settings_.endGroup();
    settings_.endGroup();
}

void OverviewPage::applyLayout(const OverviewLayout& L)
{
    bool anyStat = false;
    for (int i = 0; i < OverviewLayout::StatCardCount; ++i) {
        if (statCardFrame_[i]) statCardFrame_[i]->setVisible(L.statCards[i]);
        // Show the separator preceding this card only when the card is visible AND
        // an earlier card is too (anyStat still reflects cards before i here). That
        // yields exactly one separator between consecutive visible cards, no leading
        // separator, and no doubled line where a hidden card sat between two visible
        // ones. Index 0 has no separator (nullptr), so it's skipped.
        if (statCardSep_[i]) statCardSep_[i]->setVisible(L.statCards[i] && anyStat);
        anyStat = anyStat || L.statCards[i];
    }
    if (statsFrame_) statsFrame_->setVisible(anyStat);
    if (sep1_)       sep1_->setVisible(anyStat);

    bool rowAVisible = false, rowBVisible = false;
    bool anyEarlierA = false, anyEarlierB = false;
    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i) {
        const bool vis = L.dmgCards[i];
        if (dmgCardFrame_[i]) dmgCardFrame_[i]->setVisible(vis);
        // Show a card's preceding separator only when it AND an earlier card in the
        // same row are visible — one line between consecutive visible cards, none
        // stranded beside a hidden one (mirrors the stat-card separator logic).
        bool& anyEarlier = (i < OverviewLayout::WingFl) ? anyEarlierA : anyEarlierB;
        if (dmgCardSep_[i]) dmgCardSep_[i]->setVisible(vis && anyEarlier);
        anyEarlier = anyEarlier || vis;
        if (i < OverviewLayout::WingFl) rowAVisible = rowAVisible || vis;
        else                            rowBVisible = rowBVisible || vis;
    }
    // A row whose cards are all hidden collapses entirely (rather than leaving
    // an empty 60px bar) so the chart's stretch factor can expand into the space.
    if (dmgRowA_) dmgRowA_->setVisible(rowAVisible);
    if (dmgRowB_) dmgRowB_->setVisible(rowBVisible);
    if (dmgHdiv_) dmgHdiv_->setVisible(rowAVisible && rowBVisible);
    if (dmgFrame_) dmgFrame_->setVisible(rowAVisible || rowBVisible);
    if (sep2_)     sep2_->setVisible(rowAVisible || rowBVisible);

    // Toggle the chart's wrapper (its parent) so hiding it collapses the row
    // instead of leaving the inset wrapper as an empty stretched gap.
    if (chart_ && chart_->parentWidget()) chart_->parentWidget()->setVisible(L.showChart);
    if (modeBar_) modeBar_->setVisible(L.showChart);

    // Tyre section — visibility derived from whether any card/chart is enabled
    bool anyCard  = false, anyChart = false;
    for (int i = 0; i < OverviewLayout::TyreCornerCount; ++i) {
        if (tyreCards_)  tyreCards_->setCornerVisible(i, L.tyreCardVisible[i]);
        anyCard = anyCard || L.tyreCardVisible[i];
    }
    for (int i = 0; i < OverviewLayout::TyreChartCount; ++i) {
        if (tyreCharts_) tyreCharts_->setChartSectionVisible(i, L.tyreChartVisible[i]);
        anyChart = anyChart || L.tyreChartVisible[i];
    }
    const bool showCards  = L.tyreView == OverviewLayout::TyreCards;
    const bool showCharts = L.tyreView == OverviewLayout::TyreCharts;
    const bool showTyre   = showCards ? anyCard : anyChart;
    if (tyreSep_)    tyreSep_->setVisible(showTyre);
    if (tyreCards_)  tyreCards_->setVisible(showTyre && showCards);
    if (tyreCharts_) tyreCharts_->setVisible(showTyre && showCharts);
}

void OverviewPage::applyAndSaveLayout(const OverviewLayout& L)
{
    applyLayout(L);
    saveLayout(L);
}

OverviewLayout::TyreView OverviewPage::currentTyreView() {
    return loadLayout().tyreView;
}

void OverviewPage::setTyreView(OverviewLayout::TyreView v) {
    OverviewLayout L = loadLayout();
    L.tyreView = v;
    applyAndSaveLayout(L);
}

void OverviewPage::setTyreGraphLifeMode(bool life) {
    settings_.setValue("ui/tyreWearMode", life ? "life" : "wear");
    if (tyreCharts_) tyreCharts_->setTyreLifeMode(life);
}
