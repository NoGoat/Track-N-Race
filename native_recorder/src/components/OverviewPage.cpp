#include "OverviewPage.h"
#include "../Labels.h"
#include "../TelemetryChart.h"
#include "../SessionModel.h"
#include "CardColors.h"
#include "PageUiHelpers.h"
#include "TyreCardsWidget.h"
#include "TyreChartsWidget.h"
#include "TyreHelpers.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>
#include <QSizePolicy>
#include <QPushButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QSignalBlocker>
#include <QApplication>
#include <QLocale>

#include <cmath>

// ── UI helpers ────────────────────────────────────────────────────────────

namespace {

// subOut, when non-null, receives a small bold label pinned to the top-right of
// the heading row — the native home for the per-card extra info the Electron app
// shows as a sub-row under the value (ERS mode, fuel "vs fin", lap, tyre age).
QFrame* makeStatCard(const QString& label, const QString& unit, QLabel*& valueOut,
                     QLabel** subOut = nullptr, QLabel** titleOut = nullptr) {
    QFrame* card = new QFrame;
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QVBoxLayout* cv = new QVBoxLayout(card);
    cv->setContentsMargins(8, 6, 8, 6);
    cv->setSpacing(1);

    QLabel* lbl = new QLabel(label.toUpper());
    QFont lf; lf.setPointSize(7);
    lbl->setFont(lf);
    lbl->setForegroundRole(QPalette::PlaceholderText);
    if (titleOut) *titleOut = lbl;   // expose the title so it can be re-labelled on format change

    // Heading row: title on the left, optional sub-info pinned to the right.
    QWidget* hdrRow = new QWidget;
    QHBoxLayout* hh = new QHBoxLayout(hdrRow);
    hh->setContentsMargins(0, 0, 0, 0);
    hh->setSpacing(4);
    hh->addWidget(lbl);
    hh->addStretch();
    if (subOut) {
        QLabel* sub = new QLabel;
        QFont sf; sf.setPointSize(7); sf.setBold(true);
        sub->setFont(sf);
        sub->setForegroundRole(QPalette::PlaceholderText);
        hh->addWidget(sub);
        *subOut = sub;
    }

    QWidget* valRow = new QWidget;
    QHBoxLayout* hl = new QHBoxLayout(valRow);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(4);

    valueOut = new QLabel("—");
    QFont vf; vf.setPointSize(15); vf.setBold(true);
    valueOut->setFont(vf);

    hl->addWidget(valueOut);
    if (!unit.isEmpty()) {
        QLabel* ulbl = new QLabel(unit);
        QFont uf; uf.setPointSize(7);
        ulbl->setFont(uf);
        ulbl->setForegroundRole(QPalette::PlaceholderText);
        hl->addWidget(ulbl);
    }
    hl->addStretch();

    cv->addWidget(hdrRow);
    cv->addWidget(valRow);
    return card;
}

QFrame* makeDmgCard(const QString& label, QLabel*& valueOut) {
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
                                     val, d.sub ? &sub : nullptr, &title);
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
    chart_ = new TelemetryChart;
    chart_->setModel(model);
    QWidget* chartWrap = new QWidget;
    QVBoxLayout* chartWrapLay = new QVBoxLayout(chartWrap);
    chartWrapLay->setContentsMargins(8, 0, 8, 0);
    chartWrapLay->setSpacing(0);
    chartWrapLay->addWidget(chart_);
    vbox->addWidget(chartWrap, 1);

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
    tyreCards_->setFixedHeight(160);
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
    dmgRowA_->setFixedHeight(60);
    QHBoxLayout* ah = new QHBoxLayout(dmgRowA_);
    ah->setContentsMargins(8, 0, 8, 0);   // L/R inset; the row's lines reach edges
    ah->setSpacing(0);
    ah->addWidget(dmgCardFrame_[OverviewLayout::TyreFl] =
        makeDmgCard("Tyre FL",  dmgTyreFl));   ah->addWidget(tnrui::vline());
    ah->addWidget(dmgCardFrame_[OverviewLayout::TyreFr] =
        makeDmgCard("Tyre FR",  dmgTyreFr));   ah->addWidget(tnrui::vline());
    ah->addWidget(dmgCardFrame_[OverviewLayout::TyreRl] =
        makeDmgCard("Tyre RL",  dmgTyreRl));   ah->addWidget(tnrui::vline());
    ah->addWidget(dmgCardFrame_[OverviewLayout::TyreRr] =
        makeDmgCard("Tyre RR",  dmgTyreRr));   ah->addWidget(tnrui::vline());
    ah->addWidget(dmgCardFrame_[OverviewLayout::BrakeFl] =
        makeDmgCard("Brake FL", dmgBrakeFl));  ah->addWidget(tnrui::vline());
    ah->addWidget(dmgCardFrame_[OverviewLayout::BrakeFr] =
        makeDmgCard("Brake FR", dmgBrakeFr));  ah->addWidget(tnrui::vline());
    ah->addWidget(dmgCardFrame_[OverviewLayout::BrakeRl] =
        makeDmgCard("Brake RL", dmgBrakeRl));  ah->addWidget(tnrui::vline());
    ah->addWidget(dmgCardFrame_[OverviewLayout::BrakeRr] =
        makeDmgCard("Brake RR", dmgBrakeRr));

    dmgHdiv_ = tnrui::hline();

    dmgRowB_ = new QFrame;
    dmgRowB_->setFixedHeight(60);
    QHBoxLayout* bh = new QHBoxLayout(dmgRowB_);
    bh->setContentsMargins(8, 0, 8, 0);   // L/R inset; the row's lines reach edges
    bh->setSpacing(0);
    bh->addWidget(dmgCardFrame_[OverviewLayout::WingFl] =
        makeDmgCard("Wing FL",   dmgWingFl));   bh->addWidget(tnrui::vline());
    bh->addWidget(dmgCardFrame_[OverviewLayout::WingFr] =
        makeDmgCard("Wing FR",   dmgWingFr));   bh->addWidget(tnrui::vline());
    bh->addWidget(dmgCardFrame_[OverviewLayout::WingRear] =
        makeDmgCard("Wing Rear", dmgWingRear)); bh->addWidget(tnrui::vline());
    bh->addWidget(dmgCardFrame_[OverviewLayout::Floor] =
        makeDmgCard("Floor",     dmgFloor));    bh->addWidget(tnrui::vline());
    bh->addWidget(dmgCardFrame_[OverviewLayout::Sidepod] =
        makeDmgCard("Sidepod",   dmgSidepod));  bh->addWidget(tnrui::vline());
    bh->addWidget(dmgCardFrame_[OverviewLayout::Diffuser] =
        makeDmgCard("Diffuser",  dmgDiffuser)); bh->addWidget(tnrui::vline());
    bh->addWidget(dmgCardFrame_[OverviewLayout::Gearbox] =
        makeDmgCard("Gearbox",   dmgGearbox));  bh->addWidget(tnrui::vline());
    bh->addWidget(dmgCardFrame_[OverviewLayout::Engine] =
        makeDmgCard("Engine",    dmgEngine));

    dv->addWidget(dmgRowA_);
    dv->addWidget(dmgHdiv_);
    dv->addWidget(dmgRowB_);

    vbox->addWidget(dmgFrame_);

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
    if (on) {
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
}

void OverviewPage::setCurrentTime(float t) {
    if (chart_) chart_->setCurrentTime(t);
    if (tyreCharts_) tyreCharts_->setCurrentTime(t);
}

void OverviewPage::setWindowSeconds(float secs) {
    if (chart_) chart_->setWindowSeconds(secs);
    if (tyreCharts_) tyreCharts_->setWindowSeconds(secs);
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
    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i) {
        if (dmgCardFrame_[i]) dmgCardFrame_[i]->setVisible(L.dmgCards[i]);
        if (i < OverviewLayout::WingFl) rowAVisible = rowAVisible || L.dmgCards[i];
        else                            rowBVisible = rowBVisible || L.dmgCards[i];
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
