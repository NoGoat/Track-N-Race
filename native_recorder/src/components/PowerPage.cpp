#include "PowerPage.h"
#include "PageUiHelpers.h"
#include "PowerChartsWidget.h"
#include "CardColors.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>

#include <cmath>

namespace {
    QWidget* makeStatCard(const QString& labelTxt, QLabel** valPtr, const QString& unitTxt, const QColor& color) {
        QWidget* w = new QWidget;
        QVBoxLayout* l = new QVBoxLayout(w);
        l->setContentsMargins(12, 8, 12, 8);
        l->setSpacing(2);

        QLabel* label = new QLabel(labelTxt);
        QFont f; f.setPointSize(8); f.setBold(true);
        label->setFont(f);
        label->setForegroundRole(QPalette::PlaceholderText);

        QWidget* valRow = new QWidget;
        QHBoxLayout* hl = new QHBoxLayout(valRow);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);

        *valPtr = new QLabel("—");
        QFont vf; vf.setPointSize(18); vf.setBold(true);
        (*valPtr)->setFont(vf);
        if (color.isValid()) {
            QPalette p = (*valPtr)->palette();
            p.setColor(QPalette::WindowText, color);
            (*valPtr)->setPalette(p);
        }

        QLabel* unit = new QLabel(unitTxt);
        QFont uf; uf.setPointSize(9);
        unit->setFont(uf);
        unit->setForegroundRole(QPalette::PlaceholderText);

        hl->addWidget(*valPtr);
        if (!unitTxt.isEmpty()) hl->addWidget(unit);
        hl->addStretch();

        l->addWidget(label);
        l->addWidget(valRow);
        l->addStretch();
        return w;
    }
}

PowerPage::PowerPage(SessionModel* model, QWidget* parent)
    : QWidget(parent)
{
    QVBoxLayout* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // ── Top Bar ───────────────────────────────────────────────────
    topBar_ = new QWidget;
    QHBoxLayout* topLay = new QHBoxLayout(topBar_);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(0);

    // Key-driven cards: { key, label, unit }. Colours are applied per-update from
    // the shared library spec (update), so none are set here.
    struct PCardDef { const char* key; const char* label; const char* unit; };
    static const PCardDef defs[] = {
        { "total",    "TOTAL POWER", "kW" }, { "ice",      "ICE",   "kW" },
        { "mguk",     "MGU-K",       "kW" }, { "split",    "SPLIT", ""   },
        { "ersStore", "ERS STORE",   "MJ" }, { "ersPct",   "ERS %", "%"  },
        { "fuel",     "FUEL",        "kg" },
    };
    for (int i = 0; i < (int)(sizeof(defs) / sizeof(defs[0])); ++i) {
        QLabel* val = nullptr;
        topLay->addWidget(cardFrames_[i] = makeStatCard(defs[i].label, &val, defs[i].unit, QColor()), 1);
        cardValue_[defs[i].key] = val;
        if (i < 6) {
            cardDivs_[i] = tnrui::vline();
            topLay->addWidget(cardDivs_[i]);
        }
    }

    vbox->addWidget(topBar_);
    hdiv_ = tnrui::hline();
    vbox->addWidget(hdiv_);

    // ── Charts ───────────────────────────────────────────────
    // Split / harvest / store / fuel are now panels of one ChartView (a single
    // QCustomPlot / OpenGL context / replot), laid out 2×2 — see PowerChartsWidget.
    charts_ = new PowerChartsWidget;
    charts_->setModel(model);
    vbox->addWidget(charts_, 1);

    applyLayout(loadLayout());
}

PowerLayout PowerPage::loadLayout()
{
    PowerLayout L;
    settings_.beginGroup("powerLayout");
    settings_.beginGroup("cards");
    for (int i = 0; i < PowerLayout::CardCount; ++i)
        L.cards[i] = settings_.value(PowerLayout::cardKey(i), true).toBool();
    settings_.endGroup();
    L.showSplit = settings_.value("showSplit", true).toBool();
    L.showHarvest = settings_.value("showHarvest", true).toBool();
    L.showStore = settings_.value("showStore", true).toBool();
    L.showFuel = settings_.value("showFuel", true).toBool();
    settings_.endGroup();
    return L;
}

void PowerPage::saveLayout(const PowerLayout& L)
{
    settings_.beginGroup("powerLayout");
    settings_.beginGroup("cards");
    for (int i = 0; i < PowerLayout::CardCount; ++i)
        settings_.setValue(PowerLayout::cardKey(i), L.cards[i]);
    settings_.endGroup();
    settings_.setValue("showSplit", L.showSplit);
    settings_.setValue("showHarvest", L.showHarvest);
    settings_.setValue("showStore", L.showStore);
    settings_.setValue("showFuel", L.showFuel);
    settings_.endGroup();
}

void PowerPage::applyLayout(const PowerLayout& L)
{
    bool anyCard = false;
    for (int i = 0; i < PowerLayout::CardCount; ++i) {
        if (cardFrames_[i]) cardFrames_[i]->setVisible(L.cards[i]);
        anyCard = anyCard || L.cards[i];
    }

    int lastVisibleIdx = -1;
    for (int i = 0; i < PowerLayout::CardCount; ++i) {
        if (i > 0 && cardDivs_[i - 1]) {
            cardDivs_[i - 1]->setVisible(false);
        }
        if (L.cards[i]) {
            if (lastVisibleIdx != -1 && i > 0) {
                if (cardDivs_[i - 1]) cardDivs_[i - 1]->setVisible(true);
            }
            lastVisibleIdx = i;
        }
    }

    if (topBar_) topBar_->setVisible(anyCard);

    // Chart sections are panels of the combined widget; it reflows internally.
    if (charts_) {
        charts_->setSectionVisible(0, L.showSplit);     // SPLIT
        charts_->setSectionVisible(1, L.showHarvest);   // HARVEST
        charts_->setSectionVisible(2, L.showStore);     // STORE
        charts_->setSectionVisible(3, L.showFuel);      // FUEL
    }

    const bool anyChart = L.showSplit || L.showHarvest || L.showStore || L.showFuel;
    if (hdiv_) hdiv_->setVisible(anyCard && anyChart);   // divider below the cards
}

void PowerPage::applyAndSaveLayout(const PowerLayout& L)
{
    applyLayout(L);
    saveLayout(L);
}

void PowerPage::update(const nlohmann::json& status) {
    if (!status.is_object()) return;

    float iceKw  = status.value("engine_power_ice_kw", 0.0f);
    float mgukKw = status.value("engine_power_mguk_kw", 0.0f);
    float totalKw = iceKw + mgukKw;

    float icePct = totalKw > 0 ? (iceKw / totalKw * 100.0f) : 0.0f;
    float ersPctS= totalKw > 0 ? (mgukKw / totalKw * 100.0f) : 0.0f;

    float ersPct = status.value("ers_pct", 0.0f);
    float ersMj  = (ersPct / 100.0f) * 4.0f;
    float fuelKg = status.value("fuel_kg", 0.0f);

    // Per-key resolver: value text + colour-spec key + the value the conditional
    // rules test against (NAN where the colour is unconditional).
    struct PCard { const char* key; QString value; const char* colorSpec; double self; };
    const PCard cards[] = {
        { "total",    QString::number(std::round(totalKw)),                              "power.total", totalKw },
        { "ice",      QString::number(std::round(iceKw)),                                "power.ice",   NAN },
        { "mguk",     QString::number(std::round(mgukKw)),                               "power.mguk",  NAN },
        { "split",    QString("%1:%2").arg(std::round(icePct)).arg(std::round(ersPctS)), "power.split", NAN },
        { "ersStore", QString::number(ersMj, 'f', 2),                                    "power.ers",   ersPct },
        { "ersPct",   QString::number(std::round(ersPct)),                               "power.ers",   ersPct },
        { "fuel",     QString::number(fuelKg, 'f', 1),                                   "power.fuel",  NAN },
    };
    for (const PCard& pc : cards) {
        QLabel* l = cardValue_.value(pc.key);
        if (!l) continue;
        l->setText(pc.value);
        const QColor c = tnr::cardColor(pc.colorSpec, pc.self);
        if (c.isValid()) {
            QPalette p = l->palette();
            p.setColor(QPalette::WindowText, c);
            l->setPalette(p);
        }
    }
}

void PowerPage::applyHarvestScale(uint16_t format)
{
    if (charts_) charts_->applyHarvestScale(format);
}

void PowerPage::setPlaybackMode(bool on, float currentTime)
{
    if (charts_) charts_->setPlaybackMode(on);
    if (on) setCurrentTime(currentTime);
}

void PowerPage::setCurrentTime(float t)
{
    if (charts_) charts_->setCurrentTime(t);
}

void PowerPage::setWindowSeconds(float secs)
{
    if (charts_) charts_->setWindowSeconds(secs);
}
