#include "../MainWindow.h"
#include "PowerChart.h"
#include "../SessionModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>
#include <QTimer>

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

    QWidget* makeHeader(const QString& titleText) {
        QWidget* header = new QWidget;
        header->setFixedHeight(24);
        QHBoxLayout* hl = new QHBoxLayout(header);
        hl->setContentsMargins(8, 0, 8, 0);
        QLabel* label = new QLabel(titleText);
        QFont f; f.setPointSize(8); f.setBold(true);
        label->setFont(f);
        label->setForegroundRole(QPalette::PlaceholderText);
        hl->addWidget(label);
        hl->addStretch();
        return header;
    }
}

QWidget* MainWindow::buildPowerPage() {
    QWidget* w = new QWidget;
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // ── Top Bar ───────────────────────────────────────────────────
    pp_topBar_ = new QWidget;
    QHBoxLayout* topLay = new QHBoxLayout(pp_topBar_);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(0);

    const QColor cIce("#5794F2");
    const QColor cMguk("#FADE2A");
    const QColor cFuel("#F0A500");

    topLay->addWidget(pp_cardFrames_[0] = makeStatCard("TOTAL POWER", &pp_totalPowerVal, "kW", QColor()), 1);
    pp_cardDivs_[0] = new QFrame; pp_cardDivs_[0]->setFrameShape(QFrame::VLine); pp_cardDivs_[0]->setFrameShadow(QFrame::Sunken); topLay->addWidget(pp_cardDivs_[0]);
    topLay->addWidget(pp_cardFrames_[1] = makeStatCard("ICE", &pp_iceVal, "kW", cIce), 1);
    pp_cardDivs_[1] = new QFrame; pp_cardDivs_[1]->setFrameShape(QFrame::VLine); pp_cardDivs_[1]->setFrameShadow(QFrame::Sunken); topLay->addWidget(pp_cardDivs_[1]);
    topLay->addWidget(pp_cardFrames_[2] = makeStatCard("MGU-K", &pp_mgukVal, "kW", cMguk), 1);
    pp_cardDivs_[2] = new QFrame; pp_cardDivs_[2]->setFrameShape(QFrame::VLine); pp_cardDivs_[2]->setFrameShadow(QFrame::Sunken); topLay->addWidget(pp_cardDivs_[2]);
    topLay->addWidget(pp_cardFrames_[3] = makeStatCard("SPLIT", &pp_splitVal, "", QColor()), 1);
    pp_cardDivs_[3] = new QFrame; pp_cardDivs_[3]->setFrameShape(QFrame::VLine); pp_cardDivs_[3]->setFrameShadow(QFrame::Sunken); topLay->addWidget(pp_cardDivs_[3]);
    topLay->addWidget(pp_cardFrames_[4] = makeStatCard("ERS STORE", &pp_ersStoreVal, "MJ", QColor()), 1);
    pp_cardDivs_[4] = new QFrame; pp_cardDivs_[4]->setFrameShape(QFrame::VLine); pp_cardDivs_[4]->setFrameShadow(QFrame::Sunken); topLay->addWidget(pp_cardDivs_[4]);
    topLay->addWidget(pp_cardFrames_[5] = makeStatCard("ERS %", &pp_ersPctVal, "%", QColor()), 1);
    pp_cardDivs_[5] = new QFrame; pp_cardDivs_[5]->setFrameShape(QFrame::VLine); pp_cardDivs_[5]->setFrameShadow(QFrame::Sunken); topLay->addWidget(pp_cardDivs_[5]);
    topLay->addWidget(pp_cardFrames_[6] = makeStatCard("FUEL", &pp_fuelVal, "kg", cFuel), 1);

    vbox->addWidget(pp_topBar_);
    pp_hdiv_ = new QFrame; pp_hdiv_->setFrameShape(QFrame::HLine); pp_hdiv_->setFrameShadow(QFrame::Sunken); vbox->addWidget(pp_hdiv_);

    // ── Charts ───────────────────────────────────────────────
    QWidget* chartsWidget = new QWidget;
    QVBoxLayout* chartsLay = new QVBoxLayout(chartsWidget);
    chartsLay->setContentsMargins(0, 0, 0, 0);
    chartsLay->setSpacing(0);

    // Top Row: Split & Harvest
    pp_topChartsRow_ = new QWidget;
    QHBoxLayout* topChartsLay = new QHBoxLayout(pp_topChartsRow_);
    topChartsLay->setContentsMargins(0, 0, 0, 0);
    topChartsLay->setSpacing(0);

    pp_splitContainer_ = new QWidget;
    QVBoxLayout* splitLay = new QVBoxLayout(pp_splitContainer_);
    splitLay->setContentsMargins(0, 0, 0, 0);
    splitLay->setSpacing(0);
    splitLay->addWidget(makeHeader("POWER SPLIT"));
    pp_splitChart = new PowerChart(PowerChartType::Split);
    pp_splitChart->setModel(model_);
    splitLay->addWidget(pp_splitChart, 1);

    pp_harvContainer_ = new QWidget;
    QVBoxLayout* harvLay = new QVBoxLayout(pp_harvContainer_);
    harvLay->setContentsMargins(0, 0, 0, 0);
    harvLay->setSpacing(0);
    harvLay->addWidget(makeHeader("ERS HARVEST THIS LAP"));
    pp_harvestChart = new PowerChart(PowerChartType::Harvest);
    pp_harvestChart->setModel(model_);
    harvLay->addWidget(pp_harvestChart, 1);

    topChartsLay->addWidget(pp_splitContainer_, 1);
    QFrame* topVline = new QFrame; topVline->setFrameShape(QFrame::VLine); topVline->setFrameShadow(QFrame::Sunken);
    topChartsLay->addWidget(topVline, 0);
    topChartsLay->addWidget(pp_harvContainer_, 1);

    // Bottom Row: Store & Fuel
    pp_bottomChartsRow_ = new QWidget;
    QHBoxLayout* bottomChartsLay = new QHBoxLayout(pp_bottomChartsRow_);
    bottomChartsLay->setContentsMargins(0, 0, 0, 0);
    bottomChartsLay->setSpacing(0);

    pp_storeContainer_ = new QWidget;
    QVBoxLayout* storeLay = new QVBoxLayout(pp_storeContainer_);
    storeLay->setContentsMargins(0, 0, 0, 0);
    storeLay->setSpacing(0);
    storeLay->addWidget(makeHeader("ERS STORE HISTORY"));
    pp_storeChart = new PowerChart(PowerChartType::Store);
    pp_storeChart->setModel(model_);
    storeLay->addWidget(pp_storeChart, 1);

    pp_fuelContainer_ = new QWidget;
    QVBoxLayout* fuelLay = new QVBoxLayout(pp_fuelContainer_);
    fuelLay->setContentsMargins(0, 0, 0, 0);
    fuelLay->setSpacing(0);
    fuelLay->addWidget(makeHeader("FUEL HISTORY"));
    pp_fuelChart = new PowerChart(PowerChartType::Fuel);
    pp_fuelChart->setModel(model_);
    fuelLay->addWidget(pp_fuelChart, 1);

    bottomChartsLay->addWidget(pp_storeContainer_, 1);
    QFrame* bottomVline = new QFrame; bottomVline->setFrameShape(QFrame::VLine); bottomVline->setFrameShadow(QFrame::Sunken);
    bottomChartsLay->addWidget(bottomVline, 0);
    bottomChartsLay->addWidget(pp_fuelContainer_, 1);

    chartsLay->addWidget(pp_topChartsRow_, 1);
    pp_hline1_ = new QFrame; pp_hline1_->setFrameShape(QFrame::HLine); pp_hline1_->setFrameShadow(QFrame::Sunken);
    chartsLay->addWidget(pp_hline1_, 0);
    chartsLay->addWidget(pp_bottomChartsRow_, 1);

    // Store references for layout toggling
    pp_vline_ = topVline;
    pp_hline2_ = bottomVline;

    vbox->addWidget(chartsWidget, 1);

    applyPowerLayout(loadPowerLayout());

    return w;
}

PowerLayout MainWindow::loadPowerLayout()
{
    PowerLayout L;
    settings.beginGroup("powerLayout");
    settings.beginGroup("cards");
    for (int i = 0; i < PowerLayout::CardCount; ++i)
        L.cards[i] = settings.value(PowerLayout::cardKey(i), true).toBool();
    settings.endGroup();
    L.showSplit = settings.value("showSplit", true).toBool();
    L.showHarvest = settings.value("showHarvest", true).toBool();
    L.showStore = settings.value("showStore", true).toBool();
    L.showFuel = settings.value("showFuel", true).toBool();
    settings.endGroup();
    return L;
}

void MainWindow::savePowerLayout(const PowerLayout& L)
{
    settings.beginGroup("powerLayout");
    settings.beginGroup("cards");
    for (int i = 0; i < PowerLayout::CardCount; ++i)
        settings.setValue(PowerLayout::cardKey(i), L.cards[i]);
    settings.endGroup();
    settings.setValue("showSplit", L.showSplit);
    settings.setValue("showHarvest", L.showHarvest);
    settings.setValue("showStore", L.showStore);
    settings.setValue("showFuel", L.showFuel);
    settings.endGroup();
}

void MainWindow::applyPowerLayout(const PowerLayout& L)
{
    bool anyCard = false;
    for (int i = 0; i < PowerLayout::CardCount; ++i) {
        if (pp_cardFrames_[i]) pp_cardFrames_[i]->setVisible(L.cards[i]);
        anyCard = anyCard || L.cards[i];
    }

    int lastVisibleIdx = -1;
    for (int i = 0; i < PowerLayout::CardCount; ++i) {
        if (i > 0 && pp_cardDivs_[i - 1]) {
            pp_cardDivs_[i - 1]->setVisible(false);
        }
        if (L.cards[i]) {
            if (lastVisibleIdx != -1 && i > 0) {
                if (pp_cardDivs_[i - 1]) pp_cardDivs_[i - 1]->setVisible(true);
            }
            lastVisibleIdx = i;
        }
    }

    if (pp_topBar_) pp_topBar_->setVisible(anyCard);

    if (pp_splitContainer_) pp_splitContainer_->setVisible(L.showSplit);
    if (pp_harvContainer_) pp_harvContainer_->setVisible(L.showHarvest);
    if (pp_storeContainer_) pp_storeContainer_->setVisible(L.showStore);
    if (pp_fuelContainer_) pp_fuelContainer_->setVisible(L.showFuel);

    bool topVisible = L.showSplit || L.showHarvest;
    bool bottomVisible = L.showStore || L.showFuel;

    if (pp_topChartsRow_) pp_topChartsRow_->setVisible(topVisible);
    if (pp_bottomChartsRow_) pp_bottomChartsRow_->setVisible(bottomVisible);

    if (pp_vline_) pp_vline_->setVisible(L.showSplit && L.showHarvest); // Top VLine
    if (pp_hline2_) pp_hline2_->setVisible(L.showStore && L.showFuel);  // Bottom VLine
    if (pp_hline1_) pp_hline1_->setVisible(topVisible && bottomVisible); // Horizontal Line between rows
    if (pp_hdiv_) pp_hdiv_->setVisible(anyCard && (topVisible || bottomVisible)); // Line below cards
}

void MainWindow::applyAndSavePowerLayout(const PowerLayout& L)
{
    applyPowerLayout(L);
    savePowerLayout(L);
}

void MainWindow::updatePowerPage() {
    if (!lastPlayerStatusData.is_object()) return;

    float iceKw  = lastPlayerStatusData.value("engine_power_ice_kw", 0.0f);
    float mgukKw = lastPlayerStatusData.value("engine_power_mguk_kw", 0.0f);
    float totalKw = iceKw + mgukKw;
    
    float icePct = totalKw > 0 ? (iceKw / totalKw * 100.0f) : 0.0f;
    float ersPctS= totalKw > 0 ? (mgukKw / totalKw * 100.0f) : 0.0f;

    float ersPct = lastPlayerStatusData.value("ers_pct", 0.0f);
    float ersMj  = (ersPct / 100.0f) * 4.0f;
    float fuelKg = lastPlayerStatusData.value("fuel_kg", 0.0f);

    pp_totalPowerVal->setText(QString::number(std::round(totalKw)));
    
    QColor cTotal = totalKw > 800 ? QColor("#C4162A") : (totalKw > 600 ? QColor("#FADE2A") : QColor("#37872D"));
    QPalette p = pp_totalPowerVal->palette();
    p.setColor(QPalette::WindowText, cTotal);
    pp_totalPowerVal->setPalette(p);

    pp_iceVal->setText(QString::number(std::round(iceKw)));
    pp_mgukVal->setText(QString::number(std::round(mgukKw)));
    
    pp_splitVal->setText(QString("%1:%2").arg(std::round(icePct)).arg(std::round(ersPctS)));
    
    pp_ersStoreVal->setText(QString::number(ersMj, 'f', 2));
    pp_ersPctVal->setText(QString::number(std::round(ersPct)));
    
    QColor cErs = ersPct > 60 ? QColor("#5794F2") : (ersPct > 30 ? QColor("#FADE2A") : QColor("#C4162A"));
    QPalette pErs = pp_ersPctVal->palette();
    pErs.setColor(QPalette::WindowText, cErs);
    pp_ersPctVal->setPalette(pErs);
    pp_ersStoreVal->setPalette(pErs);

    pp_fuelVal->setText(QString::number(fuelKg, 'f', 1));
}
