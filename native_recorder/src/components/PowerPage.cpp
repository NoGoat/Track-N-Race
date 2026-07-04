#include "PowerPage.h"
#include "PageUiHelpers.h"
#include "PowerChart.h"
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
    QWidget* chartsWidget = new QWidget;
    QVBoxLayout* chartsLay = new QVBoxLayout(chartsWidget);
    chartsLay->setContentsMargins(0, 0, 0, 0);
    chartsLay->setSpacing(0);

    // Top Row: Split & Harvest
    topChartsRow_ = new QWidget;
    QHBoxLayout* topChartsLay = new QHBoxLayout(topChartsRow_);
    topChartsLay->setContentsMargins(0, 0, 0, 0);
    topChartsLay->setSpacing(0);

    splitContainer_ = new QWidget;
    QVBoxLayout* splitLay = new QVBoxLayout(splitContainer_);
    splitLay->setContentsMargins(0, 0, 0, 0);
    splitLay->setSpacing(0);
    splitLay->addWidget(tnrui::makeChartHeader("POWER SPLIT"));
    splitChart_ = new PowerChart(PowerChartType::Split);
    splitChart_->setModel(model);
    splitLay->addWidget(splitChart_, 1);

    harvContainer_ = new QWidget;
    QVBoxLayout* harvLay = new QVBoxLayout(harvContainer_);
    harvLay->setContentsMargins(0, 0, 0, 0);
    harvLay->setSpacing(0);
    harvLay->addWidget(tnrui::makeChartHeader("ERS HARVEST THIS LAP"));
    harvestChart_ = new PowerChart(PowerChartType::Harvest);
    harvestChart_->setModel(model);
    harvLay->addWidget(harvestChart_, 1);

    topChartsLay->addWidget(splitContainer_, 1);
    QFrame* topVline = tnrui::vline();
    topChartsLay->addWidget(topVline, 0);
    topChartsLay->addWidget(harvContainer_, 1);

    // Bottom Row: Store & Fuel
    bottomChartsRow_ = new QWidget;
    QHBoxLayout* bottomChartsLay = new QHBoxLayout(bottomChartsRow_);
    bottomChartsLay->setContentsMargins(0, 0, 0, 0);
    bottomChartsLay->setSpacing(0);

    storeContainer_ = new QWidget;
    QVBoxLayout* storeLay = new QVBoxLayout(storeContainer_);
    storeLay->setContentsMargins(0, 0, 0, 0);
    storeLay->setSpacing(0);
    storeLay->addWidget(tnrui::makeChartHeader("ERS STORE HISTORY"));
    storeChart_ = new PowerChart(PowerChartType::Store);
    storeChart_->setModel(model);
    storeLay->addWidget(storeChart_, 1);

    fuelContainer_ = new QWidget;
    QVBoxLayout* fuelLay = new QVBoxLayout(fuelContainer_);
    fuelLay->setContentsMargins(0, 0, 0, 0);
    fuelLay->setSpacing(0);
    fuelLay->addWidget(tnrui::makeChartHeader("FUEL HISTORY"));
    fuelChart_ = new PowerChart(PowerChartType::Fuel);
    fuelChart_->setModel(model);
    fuelLay->addWidget(fuelChart_, 1);

    bottomChartsLay->addWidget(storeContainer_, 1);
    QFrame* bottomVline = tnrui::vline();
    bottomChartsLay->addWidget(bottomVline, 0);
    bottomChartsLay->addWidget(fuelContainer_, 1);

    chartsLay->addWidget(topChartsRow_, 1);
    hline1_ = tnrui::hline();
    chartsLay->addWidget(hline1_, 0);
    chartsLay->addWidget(bottomChartsRow_, 1);

    // Store references for layout toggling
    vline_ = topVline;
    hline2_ = bottomVline;

    vbox->addWidget(chartsWidget, 1);

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

    if (splitContainer_) splitContainer_->setVisible(L.showSplit);
    if (harvContainer_) harvContainer_->setVisible(L.showHarvest);
    if (storeContainer_) storeContainer_->setVisible(L.showStore);
    if (fuelContainer_) fuelContainer_->setVisible(L.showFuel);

    bool topVisible = L.showSplit || L.showHarvest;
    bool bottomVisible = L.showStore || L.showFuel;

    if (topChartsRow_) topChartsRow_->setVisible(topVisible);
    if (bottomChartsRow_) bottomChartsRow_->setVisible(bottomVisible);

    if (vline_) vline_->setVisible(L.showSplit && L.showHarvest); // Top VLine
    if (hline2_) hline2_->setVisible(L.showStore && L.showFuel);  // Bottom VLine
    if (hline1_) hline1_->setVisible(topVisible && bottomVisible); // Horizontal Line between rows
    if (hdiv_) hdiv_->setVisible(anyCard && (topVisible || bottomVisible)); // Line below cards
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
    if (harvestChart_) harvestChart_->applyHarvestScale(format);
}

void PowerPage::setPlaybackMode(bool on, float currentTime)
{
    if (splitChart_) splitChart_->setPlaybackMode(on);
    if (harvestChart_) harvestChart_->setPlaybackMode(on);
    if (storeChart_) storeChart_->setPlaybackMode(on);
    if (fuelChart_) fuelChart_->setPlaybackMode(on);
    if (on) setCurrentTime(currentTime);
}

void PowerPage::setCurrentTime(float t)
{
    if (splitChart_) splitChart_->setCurrentTime(t);
    if (harvestChart_) harvestChart_->setCurrentTime(t);
    if (storeChart_) storeChart_->setCurrentTime(t);
    if (fuelChart_) fuelChart_->setCurrentTime(t);
}

void PowerPage::setWindowSeconds(float secs)
{
    if (splitChart_) splitChart_->setWindowSeconds(secs);
    if (harvestChart_) harvestChart_->setWindowSeconds(secs);
    if (storeChart_) storeChart_->setWindowSeconds(secs);
    if (fuelChart_) fuelChart_->setWindowSeconds(secs);
}
