#include "OverviewPage.h"
#include "../CompactSettings.h"
#include "../Labels.h"
#include "../TelemetryChart.h"
#include "../SessionModel.h"
#include "../ChartCoordinates.h"
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
#include <QShowEvent>

#include <algorithm>

#include <cmath>

// ── UI helpers ────────────────────────────────────────────────────────────

namespace {

// Fixed height for the tyre-cards widget at each density level (see
// TyreCardsWidget::Level): Full keeps the tall stacked cards; the compact levels
// shrink as rows are dropped (Ultra Compact 1/2 are a single value row).
int tyreCardsHeight(int level) {
    switch (level) {
        case TyreCardsWidget::Spacious:      return 210;
        case TyreCardsWidget::CompactColumn: return 130;
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
                     tnr::DensityMode density, QLabel** subOut = nullptr, QLabel** titleOut = nullptr,
                     QLabel** unitOut = nullptr) {
    const bool compact = density == tnr::DensityMode::Compact;
    const bool spacious = density == tnr::DensityMode::Spacious;
    QFrame* card = new QFrame;
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    QLabel* lbl = new QLabel(label.toUpper());
    QFont lf; lf.setPointSize(compact ? 8 : spacious ? 10 : 7);
    lbl->setFont(lf);
    lbl->setForegroundRole(QPalette::PlaceholderText);
    if (titleOut) *titleOut = lbl;   // expose the title so it can be re-labelled on format change

    valueOut = new QLabel("-");
    QFont vf; vf.setPointSize(compact ? 12 : spacious ? 24 : 15); vf.setBold(true);
    valueOut->setFont(vf);

    QLabel* ulbl = nullptr;
    if (!unit.isEmpty()) {
        ulbl = new QLabel(unit);
        QFont uf; uf.setPointSize(compact ? 8 : spacious ? 10 : 7);
        ulbl->setFont(uf);
        ulbl->setForegroundRole(QPalette::PlaceholderText);
        ulbl->hide();   // shown once the value is known
    }
    if (unitOut) *unitOut = ulbl;
    QLabel* sub = nullptr;
    if (subOut) {
        sub = new QLabel;
        QFont sf; sf.setPointSize(compact ? 8 : spacious ? 10 : 7); sf.setBold(true);
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
    cv->setContentsMargins(spacious ? 12 : 8, spacious ? 10 : 6,
                           spacious ? 12 : 8, spacious ? 10 : 6);
    cv->setSpacing(spacious ? 3 : 1);
    if (spacious) card->setMinimumHeight(96);

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

// Damage card: label, value + "%" unit and — in Spacious — Electron's
// Clean / Minor / Critical status line.
QFrame* makeDmgCard(const QString& label, QLabel*& valueOut, QLabel*& unitOut,
                    QLabel*& statusOut, tnr::DensityMode density) {
    const bool compact = density == tnr::DensityMode::Compact;
    const bool spacious = density == tnr::DensityMode::Spacious;
    QFrame* card = new QFrame;
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    QLabel* lbl = new QLabel(label.toUpper());
    QFont lf; lf.setPointSize(compact ? 7 : spacious ? 9 : 6);
    lbl->setFont(lf);
    lbl->setForegroundRole(QPalette::PlaceholderText);

    valueOut = new QLabel("—");
    QFont vf; vf.setPointSize(compact ? 11 : spacious ? 18 : 12); vf.setBold(true);
    valueOut->setFont(vf);

    unitOut = new QLabel("%");
    QFont uf; uf.setPointSize(compact ? 7 : spacious ? 9 : 7);
    unitOut->setFont(uf);
    unitOut->setForegroundRole(QPalette::PlaceholderText);
    unitOut->hide();

    statusOut = nullptr;

    if (compact) {
        // One line, two-value: label left, value pinned right.
        QHBoxLayout* cl = new QHBoxLayout(card);
        cl->setContentsMargins(6, 2, 6, 2);
        cl->setSpacing(2);
        cl->addWidget(lbl);
        cl->addStretch();
        cl->addWidget(valueOut);
        cl->addWidget(unitOut, 0, Qt::AlignBottom);
        return card;
    }

    QVBoxLayout* cv = new QVBoxLayout(card);
    cv->setContentsMargins(spacious ? 10 : 6, spacious ? 8 : 4,
                           spacious ? 10 : 6, spacious ? 8 : 4);
    cv->setSpacing(spacious ? 2 : 0);
    cv->addWidget(lbl);
    QHBoxLayout* valueRow = new QHBoxLayout;
    valueRow->setContentsMargins(0, 0, 0, 0);
    valueRow->setSpacing(2);
    valueRow->addWidget(valueOut);
    valueRow->addWidget(unitOut, 0, Qt::AlignBottom);
    valueRow->addStretch();
    cv->addLayout(valueRow);
    if (spacious) {
        statusOut = new QLabel("—");
        QFont sf; sf.setPointSize(8);
        statusOut->setFont(sf);
        statusOut->setForegroundRole(QPalette::PlaceholderText);
        cv->addWidget(statusOut);
    }
    return card;
}

// Electron's damage-card display order: each corner's tyre then brake, then
// the bodywork (note Diffuser before Sidepod).
constexpr int kDamageOrder[OverviewLayout::DmgCardCount] = {
    OverviewLayout::TyreFl, OverviewLayout::BrakeFl, OverviewLayout::TyreFr, OverviewLayout::BrakeFr,
    OverviewLayout::TyreRl, OverviewLayout::BrakeRl, OverviewLayout::TyreRr, OverviewLayout::BrakeRr,
    OverviewLayout::WingFl, OverviewLayout::WingFr, OverviewLayout::WingRear, OverviewLayout::Floor,
    OverviewLayout::Diffuser, OverviewLayout::Sidepod, OverviewLayout::Gearbox, OverviewLayout::Engine,
};

} // namespace

// ── Overview page ─────────────────────────────────────────────────────────

OverviewPage::OverviewPage(SessionModel* model, QWidget* parent)
    : QWidget(parent)
{
    statsDensity_ = tnr::densityFromValue(settings_.value(
        tnr::compactKey(tnr::CompactSection::OverviewStats), "normal"));
    damageDensity_ = tnr::densityFromValue(settings_.value(
        tnr::compactKey(tnr::CompactSection::OverviewDamage), "normal"));
    tyresLevel_ = qBound(0, settings_.value(
        tnr::compactKey(tnr::CompactSection::OverviewTyres), 0).toInt(), 6);

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
    // Chart-window selection and the clickable legend now live in the shared
    // global/per-chart controls. Retain the legacy widgets only as inert members
    // for compatibility with the existing playback plumbing.
    modeBar_->setMaximumHeight(0);
    modeBar_->hide();

    connect(lapCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (chart_ && idx >= 0)
            chart_->setCompareLap(lapCombo_->itemData(idx).toInt());
    });

    // ── Chart ────────────────────────────────────────────────────
    // Wrapped so the chart and its raw-values table can share one grid cell (toggled
    // by setTelemetryTable); the wrapper carries no inset so both fill edge to edge.
    model_ = model;

    QWidget* chartWrap = new QWidget;
    QGridLayout* chartWrapLay = new QGridLayout(chartWrap);
    // No inset: the Speed/RPM/ERS chart (and its table) fill the width edge to edge,
    // flush against the separators above/below — no gap on any side.
    chartWrapLay->setContentsMargins(0, 0, 0, 0);
    chartWrapLay->setSpacing(0);

    chart_ = new TelemetryChart(chartWrap);
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
                                       { "ERS (%)",     GraphTable::Fixed1 } }, chartWrap);
    // GraphTable is borderless/edge-to-edge (see its constructor), so this table needs
    // no frame. But it overlays the chart in the same cell, so it must stay opaque:
    // fill the base background here. (An opaque background is doubly required because
    // once a QTableView carries a stylesheet its scrollbar groove renders transparent,
    // which would otherwise let the chart show through that strip.)
    telemetryTable_->setStyleSheet(QStringLiteral(
        "QTableView, QTableView:hover { border: none; background: palette(base); }"));

    chartWrapLay->addWidget(chart_,          0, 0);
    chartWrapLay->addWidget(telemetryTable_, 0, 0);
    telemetryTable_->hide();   // chart shown by default; setTelemetryTable swaps them
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
    tyreCards_->setModel(model);   // feeds the per-corner Table view from the session buffer
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
    cardUnit_.clear();
    for (int i = 0; i < OverviewLayout::StatCardCount; ++i) {
        statCardFrame_[i] = nullptr;
        statCardSep_[i]   = nullptr;
    }
    // Compact cards carry their own left margin, so the row's L/R inset would
    // over-indent the first card ("SPEED") relative to the rest — drop it in
    // compact mode; the full two-line layout keeps its original inset.
    const bool compact = statsDensity_ == tnr::DensityMode::Compact;
    const bool spacious = statsDensity_ == tnr::DensityMode::Spacious;
    sh->setContentsMargins(compact ? 0 : spacious ? 12 : 8, 0,
                           compact ? 0 : spacious ? 12 : 8, 0);

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
        QLabel* val = nullptr; QLabel* sub = nullptr; QLabel* title = nullptr; QLabel* unit = nullptr;
        QFrame* frame = makeStatCard(tnr::L("ui.overview." + key), d.unit,
                                     val, statsDensity_, d.sub ? &sub : nullptr, &title, &unit);
        statCardFrame_[d.idx] = frame;
        cardValue_[key] = val;
        cardTitle_[key] = title;
        if (unit) cardUnit_[key] = unit;
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
    // Detach the row items without deleting their widgets, then delete the
    // separators and every card (hidden cards are not in a row layout).
    for (QLayout* row : { dmgRowA_->layout(), dmgRowB_->layout() })
        while (QLayoutItem* item = row->takeAt(0)) delete item;
    qDeleteAll(dmgSeps_);
    dmgSeps_.clear();
    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i) {
        delete dmgCardFrame_[i];
        dmgCardFrame_[i] = nullptr;
        dmgValue_[i] = dmgUnit_[i] = dmgStatus_[i] = nullptr;
    }

    const bool compact = damageDensity_ == tnr::DensityMode::Compact;
    const bool spacious = damageDensity_ == tnr::DensityMode::Spacious;
    const int rowH = compact ? 26 : spacious ? 96 : 60;
    dmgRowA_->setFixedHeight(rowH);
    dmgRowB_->setFixedHeight(rowH);

    // Drop the rows' L/R inset in compact mode so the first card lines up with
    // the rest; the full two-line layout keeps its original inset.
    const int dmgSide = compact ? 0 : spacious ? 12 : 8;
    dmgRowA_->layout()->setContentsMargins(dmgSide, 0, dmgSide, 0);
    dmgRowB_->layout()->setContentsMargins(dmgSide, 0, dmgSide, 0);

    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i) {
        QFrame* card = makeDmgCard(OverviewLayout::dmgCardLabel(i), dmgValue_[i], dmgUnit_[i],
                                   dmgStatus_[i], damageDensity_);
        card->setParent(dmgFrame_);   // parked until layoutDamageCards places it
        card->hide();
        dmgCardFrame_[i] = card;
    }
}

// Electron flows the visible damage cards into one row, splitting them into
// two equal-ish rows only when more than eight are shown.
void OverviewPage::layoutDamageCards(const OverviewLayout& L) {
    auto* rowA = qobject_cast<QHBoxLayout*>(dmgRowA_->layout());
    auto* rowB = qobject_cast<QHBoxLayout*>(dmgRowB_->layout());
    for (QHBoxLayout* row : { rowA, rowB })
        while (QLayoutItem* item = row->takeAt(0)) delete item;   // cards stay alive
    qDeleteAll(dmgSeps_);
    dmgSeps_.clear();

    QVector<int> visible;
    for (int idx : kDamageOrder) {
        if (dmgCardFrame_[idx]) dmgCardFrame_[idx]->setVisible(false);
        if (L.dmgCards[idx]) visible.push_back(idx);
    }
    const int n = visible.size();
    const bool twoRow = n > 8;
    const int split = twoRow ? (n + 1) / 2 : n;
    for (int i = 0; i < n; ++i) {
        QHBoxLayout* row = i < split ? rowA : rowB;
        if (i != 0 && i != split) {
            QFrame* sep = tnrui::vline();
            dmgSeps_.push_back(sep);
            row->addWidget(sep);
        }
        QFrame* card = dmgCardFrame_[visible[i]];
        row->addWidget(card, 1);
        card->setVisible(true);
    }
    dmgRowA_->setVisible(n > 0);
    dmgRowB_->setVisible(twoRow);
    if (dmgHdiv_)  dmgHdiv_->setVisible(twoRow);
    if (dmgFrame_) dmgFrame_->setVisible(n > 0);
    if (sep2_)     sep2_->setVisible(n > 0);
}

// Live per-section compact toggles. Each rebuilds only its own row/cards, re-applies
// the layout visibility (the rebuild recreated the frames applyLayout hides), then
// repopulates from the cache so nothing shows a stale "—" while paused.
void OverviewPage::setStatsCompact(bool on) {
    setStatsDensity(on ? tnr::DensityMode::Compact : tnr::DensityMode::Normal);
}

void OverviewPage::setStatsDensity(tnr::DensityMode mode) {
    if (statsDensity_ == mode) return;
    statsDensity_ = mode;
    buildStatCards();
    applyLayout(loadLayout());
    refreshCards();
}

void OverviewPage::setDamageCompact(bool on) {
    setDamageDensity(on ? tnr::DensityMode::Compact : tnr::DensityMode::Normal);
}

void OverviewPage::setDamageDensity(tnr::DensityMode mode) {
    if (damageDensity_ == mode) return;
    damageDensity_ = mode;
    buildDamageCards();
    applyLayout(loadLayout());
    damageDirty_ = true;
    flushPending();
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

void OverviewPage::onTelemetry(const TelemetryRow& row) {
    cache_.speed      = (float)row.speed_kph;
    cache_.rpm        = row.rpm;
    cache_.gear       = row.gear;
    cache_.throttle   = row.throttle;
    cache_.brake      = row.brake;
    cache_.drs        = row.drs;
    cache_.slm        = row.slm;
    cache_.engineTemp = row.engine_temp;
    if (!haveTelemetry_) { haveTelemetry_ = true; damageDirty_ = true; }
    cardsDirty_ = true;
}

void OverviewPage::onStatus(const StatusRow& row) {
    cache_.ersPct         = (float)row.ers_pct;
    cache_.ersMode        = row.ers_mode;
    cache_.fuelKg         = (float)row.fuel_kg;
    cache_.fuelLaps       = (float)row.fuel_laps;
    cache_.tyreCompound   = row.tyre_compound;
    cache_.tyreAgeLaps    = row.tyre_age_laps;
    cache_.fuelMix        = row.fuel_mix;
    cache_.visualCompound = row.visual_compound;
    cardsDirty_ = true;
}

void OverviewPage::onDamage(const DamageRow& row) {
    lastDamage_ = row;   // cached so a compact-mode rebuild can repaint while paused
    cache_.drsFault = row.drs_fault;
    cache_.ersFault = row.ers_fault;
    cardsDirty_ = true;
    damageDirty_ = true;
}

void OverviewPage::refreshDamage() {
    // Electron reads a missing damage value as 0 once connected, and shows "—"
    // (no unit) until the first telemetry row.
    const bool connected = haveTelemetry_;
    int values[OverviewLayout::DmgCardCount] = {};
    if (lastDamage_) {
        const DamageRow& row = *lastDamage_;
        const int raw[OverviewLayout::DmgCardCount] = {
            row.tyre_dmg_fl, row.tyre_dmg_fr, row.tyre_dmg_rl, row.tyre_dmg_rr,
            row.brake_dmg_fl, row.brake_dmg_fr, row.brake_dmg_rl, row.brake_dmg_rr,
            row.wing_fl, row.wing_fr, row.wing_rear, row.floor_damage,
            row.sidepod_damage, row.diffuser_damage, row.gearbox_damage, row.engine_damage,
        };
        for (int i = 0; i < OverviewLayout::DmgCardCount; ++i) values[i] = qMax(0, raw[i]);
    }
    const QString green = tnr::themed("#37872D", "#137333").name();
    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i) {
        const int v = values[i];
        const QString color = v > 0 ? QStringLiteral("#C4162A") : green;
        const QString text = connected ? QString::number(v) : QStringLiteral("—");
        const QString style = connected ? QString("color: %1;").arg(color) : QString();
        if (QLabel* l = dmgValue_[i]) {
            if (l->text() != text) l->setText(text);
            if (l->styleSheet() != style) l->setStyleSheet(style);
        }
        if (QLabel* u = dmgUnit_[i]) u->setVisible(connected);
        if (QLabel* st = dmgStatus_[i]) {
            const QString status = !connected ? QStringLiteral("—")
                : v == 0 ? QStringLiteral("Clean") : v > 20 ? QStringLiteral("Critical")
                : QStringLiteral("Minor");
            const QString statusStyle = connected && v > 0 ? QString("color: %1;").arg(color) : QString();
            if (st->text() != status) st->setText(status);
            if (st->styleSheet() != statusStyle) st->setStyleSheet(statusStyle);
        }
    }
}

void OverviewPage::resetLiveData() {
    cache_ = OvCache{};
    lastDamage_.reset();
    haveTelemetry_ = false;
    cardsDirty_ = damageDirty_ = true;
    flushPending();
}

void OverviewPage::onLap(const LapRow& row) {
    cache_.pos    = row.position;
    cache_.lapNum = row.lap_num;
    cardsDirty_ = true;
}

void OverviewPage::flushPending() {
    if (!isVisible()) return;
    if (damageDirty_) {
        damageDirty_ = false;
        refreshDamage();
    }
    if (cardsDirty_) {
        cardsDirty_ = false;
        refreshCards();
    }
}

void OverviewPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    flushPending();
}

void OverviewPage::updateTyreCards(const TelemetryRow* telemetry, const DamageRow* damage) {
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
    static const QString kMissing = QStringLiteral("-");   // Electron's MISSING placeholder
    const auto haveInt = [](int value) { return value != OvCache::missingInt; };
    const auto haveFloat = [](float value) { return std::isfinite(value); };
    const bool compact = statsDensity_ == tnr::DensityMode::Compact;

    // A missing value reads "-" with no unit and the default colour.
    auto setCard = [this](const QString& key, const QString& value, const QColor& color) {
        const bool have = value != kMissing;
        if (QLabel* l = cardValue_.value(key)) {
            const QString style = tnr::cardColorStyle(have ? color : QColor());
            if (l->text() != value) l->setText(value);
            if (l->styleSheet() != style) l->setStyleSheet(style);
        }
        if (QLabel* u = cardUnit_.value(key)) u->setVisible(have);
    };
    auto setSub = [this](const QString& key, const QString& sub, const QColor& subColor = QColor()) {
        if (QLabel* l = cardSub_.value(key)) {
            const QString style = subColor.isValid()
                ? QString("color: %1;").arg(subColor.name()) : QString();
            if (l->text() != sub) l->setText(sub);
            if (l->styleSheet() != style) l->setStyleSheet(style);
        }
    };

    // Until the first telemetry row every card is blank, as Electron.
    if (!haveTelemetry_) {
        for (auto it = cardValue_.cbegin(); it != cardValue_.cend(); ++it) {
            setCard(it.key(), kMissing, QColor());
            setSub(it.key(), QString());
        }
        return;
    }

    setCard("speed", haveFloat(c.speed) ? QString::number(qRound(c.speed)) : kMissing,
            tnr::cardColor("speed"));
    setCard("rpm", haveInt(c.rpm) ? QLocale().toString(c.rpm) : kMissing, tnr::cardColor("rpm"));
    const QString gear = !haveInt(c.gear) ? kMissing
        : c.gear <= 0 ? (c.gear < 0 ? QStringLiteral("R") : QStringLiteral("N"))
                      : QString::number(c.gear);
    setCard("gear", gear, haveInt(c.gear) ? tnr::cardColor("gear", c.gear) : QColor());
    setCard("throttle", haveFloat(c.throttle) ? QString::number(qRound(c.throttle * 100.0f)) : kMissing,
            tnr::cardColor("throttle"));
    setCard("brake", haveFloat(c.brake) ? QString::number(qRound(c.brake * 100.0f)) : kMissing,
            tnr::cardColor("brake", NAN, { {"brake", c.brake} }));

    // Wing card: data field is format-aware (drs in 2025, slm in 2026). Only
    // the DRS variant carries the FAULT sub-line.
    const bool slm = tnr::Labels::instance().t("card.wing.key") == "slm";
    const int wingValue = slm ? c.slm : c.drs;
    const bool haveWing = haveInt(wingValue);
    const bool wingOpen = haveWing && wingValue != 0;
    setCard("drs", haveWing ? (wingOpen ? QStringLiteral("ON") : QStringLiteral("OFF")) : kMissing,
            haveWing ? tnr::cardColor("wing", wingOpen ? 1 : 0) : QColor());
    const bool drsFault = !slm && haveInt(c.drsFault) && c.drsFault == 1;
    setSub("drs", drsFault ? QStringLiteral("FAULT") : QString(),
           drsFault ? QColor("#C4162A") : QColor());

    setCard("engine", haveInt(c.engineTemp) ? QString::number(c.engineTemp) : kMissing,
            haveInt(c.engineTemp) ? tnr::cardColor("engine", c.engineTemp) : QColor());

    setCard("ers", haveFloat(c.ersPct) ? QString::number(qRound(c.ersPct)) : kMissing,
            tnr::cardColor("ers", c.ersPct, { {"ers_mode", haveInt(c.ersMode) ? double(c.ersMode) : NAN},
                                              {"ers_pct", c.ersPct} }));
    if (haveInt(c.ersFault) && c.ersFault == 1) {
        setSub("ers", QStringLiteral("FAULT"), QColor("#C4162A"));
    } else {
        QString mode = haveInt(c.ersMode) && c.ersMode >= 0 && c.ersMode < 4
            ? tnr::Ln("ers.mode", c.ersMode) : QString();
        if (compact) mode.replace(QStringLiteral("Overtake"), QStringLiteral("OT"));
        setSub("ers", mode);
    }

    setCard("fuel", haveFloat(c.fuelKg) ? QString::number(c.fuelKg, 'f', 1) : kMissing,
            tnr::cardColor("fuel", NAN, { {"fuel_laps", c.fuelLaps} }));
    setSub("fuel", haveFloat(c.fuelLaps)
        ? QString("%1%2%3").arg(c.fuelLaps >= 0 ? "+" : "").arg(c.fuelLaps, 0, 'f', 1)
              .arg(compact ? QString() : QStringLiteral(" vs fin"))
        : QString());

    setCard("pos", haveInt(c.pos) && c.pos > 0 ? "P" + QString::number(c.pos) : kMissing, QColor());
    setSub("pos", haveInt(c.lapNum) && c.lapNum > 0 ? "Lap " + QString::number(c.lapNum) : QString());

    QColor tyreColor = haveInt(c.tyreCompound)
        ? tyreTextColor(c.tyreCompound, haveInt(c.visualCompound) ? c.visualCompound : 0)
        : QColor();
    if (!tyreColor.isValid() && haveInt(c.visualCompound))
        tyreColor = tnr::cardColor("tyre", NAN, { {"visual_compound", (double)c.visualCompound} });
    const QString tyre = haveInt(c.tyreCompound) ? tyreLabel(c.tyreCompound) : kMissing;
    setCard("tyre", tyre == QStringLiteral("—") ? kMissing : tyre, tyreColor);
    setSub("tyre", haveInt(c.tyreAgeLaps)
        ? QString("%1L%2").arg(c.tyreAgeLaps)
              .arg(c.fuelMix >= 0 && c.fuelMix < 4 ? QString(" · %1").arg(FUEL_MIX[c.fuelMix]) : QString())
        : QString());
}

// ── Playback plumbing ─────────────────────────────────────────────────────

void OverviewPage::setPlaybackMode(bool on, float currentTime) {
    playback_ = on;
    if (on) {
        currentTime_ = currentTime;
        if (chart_) { chart_->setPlaybackMode(true); chart_->setCurrentTime(currentTime); }
        if (tyreCharts_) { tyreCharts_->setPlaybackMode(true); tyreCharts_->setCurrentTime(currentTime); }
        if (tyreCards_) { tyreCards_->setPlaybackMode(true); tyreCards_->setCurrentTime(currentTime); }
        if (compareBtn_) compareBtn_->setEnabled(true);
    } else {
        if (chart_) { chart_->setPlaybackMode(false); chart_->setMode(ChartMode::Default); }
        if (tyreCharts_) tyreCharts_->setPlaybackMode(false);
        if (tyreCards_) tyreCards_->setPlaybackMode(false);
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
    if (tyreCards_) tyreCards_->setCurrentTime(t);
    if (telemetryTableMode_) refreshTelemetryTable();
}

void OverviewPage::setWindowSeconds(float secs) {
    windowS_ = secs;
    if (chart_) chart_->setWindowSeconds(secs);
    if (tyreCharts_) tyreCharts_->setWindowSeconds(secs);
    if (tyreCards_) tyreCards_->setWindowSeconds(secs);
    if (telemetryTableMode_) refreshTelemetryTable();
}

void OverviewPage::setTelemetryTable(bool table) {
    telemetryTableMode_ = table;
    // Hide the chart in table mode so it isn't replotting unseen behind the opaque
    // table. The table draws its own rounded frame (see its stylesheet) and fills an
    // opaque background, so it needs nothing painted behind it.
    if (chart_)          chart_->setVisible(!table);
    if (telemetryTable_) { telemetryTable_->setVisible(table); if (table) telemetryTable_->raise(); }
    // The mode bar (Default/Current Lap/… chart-mode buttons, lap-compare combo and
    // the Speed/RPM/ERS legend) only applies to the chart — hide it in table mode.
    if (modeBar_) modeBar_->hide();
    if (table) refreshTelemetryTable();
}

void OverviewPage::setTyreGraphTable(int section, bool table) {
    if (tyreCharts_) tyreCharts_->setSectionViewMode(section, table);
}

void OverviewPage::setCardTable(int corner, bool table) {
    if (tyreCards_) tyreCards_->setCornerTable(corner, table);
}

void OverviewPage::refreshTelemetryTable() {
    if (!telemetryTable_ || !model_) return;
    const SessionData& d = model_->data();
    const float endTime = playback_ ? currentTime_ : d.latestTime;
    const auto section = tnr::GraphSection::OverviewTelemetry;
    const ChartWindow window = model_->effectiveChartWindow(section);
    const int selectedLap = model_->referenceLap(section);
    const ChartDomain domain = resolveChartDomain(
        d, window, selectedLap, endTime, model_->sectorBoundaries(),
        model_->chartPrimaryLap(endTime),
        model_->chartReferenceLap(window, selectedLap, endTime));
    telemetryTable_->setDistanceMode(domain.distance);
    const SampleRange<TelSample> telemetry = domain.distance && domain.primary
        ? SampleRange<TelSample>(domain.primary->tel) : d.tel();
    const SampleRange<StsSample> status = domain.distance && domain.primary
        ? SampleRange<StsSample>(domain.primary->sts) : d.sts();

    // ERS lives in the status history, sampled independently of telemetry — match each telemetry
    // sample to the most recent status sample at or before it.
    auto ersAt = [&](float t) -> float {
        if (status.isEmpty()) return std::numeric_limits<float>::quiet_NaN();
        auto it = std::upper_bound(status.begin(), status.end(), t,
            [](float key, const StsSample& x) { return key < x.t; });
        if (it == status.begin()) return it->ers;
        return (it - 1)->ers;
    };

    telemetryTable_->beginRebuild(domain.lower, domain.upper,
                                  chartWindowAccumulatesLaps(domain.window));
    for (int i = telemetry.size() - 1; i >= 0 && !telemetryTable_->full(); --i) {
        const TelSample& sample = telemetry[i];
        if (sample.t > domain.currentTime) continue;
        const double coordinate = domain.distance
            ? d.distanceAtTime(domain.primary, sample.t) : sample.t;
        if (!qIsFinite(coordinate) || coordinate < domain.lower || coordinate > domain.upper) continue;
        telemetryTable_->addRow(coordinate, sample.speed, sample.rpm, ersAt(sample.t));
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
        // Electron defaults: tyre/brake damage and Sidepod hidden.
        const bool defaultHidden = i < OverviewLayout::WingFl || i == OverviewLayout::Sidepod;
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

    layoutDamageCards(L);

    // Toggle the chart's wrapper (its parent) so hiding it collapses the row
    // instead of leaving the inset wrapper as an empty stretched gap.
    if (chart_ && chart_->parentWidget()) chart_->parentWidget()->setVisible(L.showChart);
    if (modeBar_) modeBar_->hide();

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
