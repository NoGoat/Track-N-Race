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
    QWidget* topBar = new QWidget;
    QHBoxLayout* topLay = new QHBoxLayout(topBar);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(0);

    const QColor cIce("#5794F2");
    const QColor cMguk("#FADE2A");
    const QColor cFuel("#F0A500");

    topLay->addWidget(makeStatCard("TOTAL POWER", &pp_totalPowerVal, "kW", QColor()), 1);
    QFrame* d1 = new QFrame; d1->setFrameShape(QFrame::VLine); d1->setFrameShadow(QFrame::Sunken); topLay->addWidget(d1);
    topLay->addWidget(makeStatCard("ICE", &pp_iceVal, "kW", cIce), 1);
    QFrame* d2 = new QFrame; d2->setFrameShape(QFrame::VLine); d2->setFrameShadow(QFrame::Sunken); topLay->addWidget(d2);
    topLay->addWidget(makeStatCard("MGU-K", &pp_mgukVal, "kW", cMguk), 1);
    QFrame* d3 = new QFrame; d3->setFrameShape(QFrame::VLine); d3->setFrameShadow(QFrame::Sunken); topLay->addWidget(d3);
    topLay->addWidget(makeStatCard("SPLIT", &pp_splitVal, "", QColor()), 1);
    QFrame* d4 = new QFrame; d4->setFrameShape(QFrame::VLine); d4->setFrameShadow(QFrame::Sunken); topLay->addWidget(d4);
    topLay->addWidget(makeStatCard("ERS STORE", &pp_ersStoreVal, "MJ", QColor()), 1);
    QFrame* d5 = new QFrame; d5->setFrameShape(QFrame::VLine); d5->setFrameShadow(QFrame::Sunken); topLay->addWidget(d5);
    topLay->addWidget(makeStatCard("ERS %", &pp_ersPctVal, "%", QColor()), 1);
    QFrame* d6 = new QFrame; d6->setFrameShape(QFrame::VLine); d6->setFrameShadow(QFrame::Sunken); topLay->addWidget(d6);
    topLay->addWidget(makeStatCard("FUEL", &pp_fuelVal, "kg", cFuel), 1);

    vbox->addWidget(topBar);
    QFrame* hdiv = new QFrame; hdiv->setFrameShape(QFrame::HLine); hdiv->setFrameShadow(QFrame::Sunken); vbox->addWidget(hdiv);

    // ── Charts Grid ───────────────────────────────────────────────
    QWidget* gridWidget = new QWidget;
    QGridLayout* grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);

    // Power Split
    pp_splitContainer_ = new QWidget;
    QVBoxLayout* splitLay = new QVBoxLayout(pp_splitContainer_);
    splitLay->setContentsMargins(0, 0, 0, 0);
    splitLay->setSpacing(0);
    splitLay->addWidget(makeHeader("POWER SPLIT"));
    pp_splitChart = new PowerChart(PowerChartType::Split);
    pp_splitChart->setModel(model_);
    splitLay->addWidget(pp_splitChart, 1);

    // ERS Harvest
    pp_harvContainer_ = new QWidget;
    QVBoxLayout* harvLay = new QVBoxLayout(pp_harvContainer_);
    harvLay->setContentsMargins(0, 0, 0, 0);
    harvLay->setSpacing(0);
    harvLay->addWidget(makeHeader("ERS HARVEST THIS LAP"));
    pp_harvestChart = new PowerChart(PowerChartType::Harvest);
    pp_harvestChart->setModel(model_);
    harvLay->addWidget(pp_harvestChart, 1);

    // ERS Store
    pp_storeContainer_ = new QWidget;
    QVBoxLayout* storeLay = new QVBoxLayout(pp_storeContainer_);
    storeLay->setContentsMargins(0, 0, 0, 0);
    storeLay->setSpacing(0);
    storeLay->addWidget(makeHeader("ERS STORE HISTORY"));
    pp_storeChart = new PowerChart(PowerChartType::Store);
    pp_storeChart->setModel(model_);
    storeLay->addWidget(pp_storeChart, 1);

    // Fuel History
    pp_fuelContainer_ = new QWidget;
    QVBoxLayout* fuelLay = new QVBoxLayout(pp_fuelContainer_);
    fuelLay->setContentsMargins(0, 0, 0, 0);
    fuelLay->setSpacing(0);
    fuelLay->addWidget(makeHeader("FUEL HISTORY"));
    pp_fuelChart = new PowerChart(PowerChartType::Fuel);
    pp_fuelChart->setModel(model_);
    fuelLay->addWidget(pp_fuelChart, 1);

    grid->addWidget(pp_splitContainer_, 0, 0);
    grid->addWidget(pp_harvContainer_,  0, 2);
    grid->addWidget(pp_storeContainer_, 2, 0);
    grid->addWidget(pp_fuelContainer_,  2, 2);

    pp_vline_ = new QFrame; pp_vline_->setFrameShape(QFrame::VLine); pp_vline_->setFrameShadow(QFrame::Sunken);
    grid->addWidget(pp_vline_, 0, 1, 3, 1);

    pp_hline1_ = new QFrame; pp_hline1_->setFrameShape(QFrame::HLine); pp_hline1_->setFrameShadow(QFrame::Sunken);
    grid->addWidget(pp_hline1_, 1, 0);

    pp_hline2_ = new QFrame; pp_hline2_->setFrameShape(QFrame::HLine); pp_hline2_->setFrameShadow(QFrame::Sunken);
    grid->addWidget(pp_hline2_, 1, 2);

    vbox->addWidget(gridWidget, 1);

    applyPowerLayout(loadPowerLayout());

    return w;
}

PowerLayout MainWindow::loadPowerLayout()
{
    PowerLayout L;
    settings.beginGroup("powerLayout");
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
    settings.setValue("showSplit", L.showSplit);
    settings.setValue("showHarvest", L.showHarvest);
    settings.setValue("showStore", L.showStore);
    settings.setValue("showFuel", L.showFuel);
    settings.endGroup();
}

void MainWindow::applyPowerLayout(const PowerLayout& L)
{
    if (pp_splitContainer_) pp_splitContainer_->setVisible(L.showSplit);
    if (pp_harvContainer_) pp_harvContainer_->setVisible(L.showHarvest);
    if (pp_storeContainer_) pp_storeContainer_->setVisible(L.showStore);
    if (pp_fuelContainer_) pp_fuelContainer_->setVisible(L.showFuel);

    if (pp_vline_) pp_vline_->setVisible((L.showSplit || L.showStore) && (L.showHarvest || L.showFuel));
    if (pp_hline1_) pp_hline1_->setVisible(L.showSplit && L.showStore);
    if (pp_hline2_) pp_hline2_->setVisible(L.showHarvest && L.showFuel);
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
