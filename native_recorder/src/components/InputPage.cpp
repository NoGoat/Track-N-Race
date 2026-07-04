#include "InputPage.h"
#include "PageUiHelpers.h"
#include "GearChart.h"
#include "InputsChart.h"
#include "SteeringChart.h"

#include <QVBoxLayout>
#include <QHBoxLayout>

InputPage::InputPage(SessionModel* model, QWidget* parent)
    : QWidget(parent)
{
    QVBoxLayout* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // ── Top Row: Gear and Inputs ──────────────────────────────────
    topRow_ = new QWidget;
    QHBoxLayout* topLay = new QHBoxLayout(topRow_);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(0);

    // Gear Chart
    gearContainer_ = new QWidget;
    QVBoxLayout* gearLay = new QVBoxLayout(gearContainer_);
    gearLay->setContentsMargins(0, 0, 0, 0);
    gearLay->setSpacing(0);
    gearLay->addWidget(tnrui::makeChartHeader("GEAR INDICATOR"));
    gearChart_ = new GearChart;
    gearChart_->setModel(model);
    gearLay->addWidget(gearChart_, 1);

    // Inputs Chart (Throttle/Brake)
    inputsContainer_ = new QWidget;
    QVBoxLayout* inputsLay = new QVBoxLayout(inputsContainer_);
    inputsLay->setContentsMargins(0, 0, 0, 0);
    inputsLay->setSpacing(0);
    inputsLay->addWidget(tnrui::makeChartHeader("THROTTLE / BRAKE CHART", {
        {"Throttle", QColor("#2FC584")},
        {"Brake", QColor("#FF4040")}
    }));
    inputsChart_ = new InputsChart;
    inputsChart_->setModel(model);
    inputsLay->addWidget(inputsChart_, 1);

    // Add to top row with a vertical divider
    topLay->addWidget(gearContainer_, 1);
    vdiv_ = tnrui::vline();
    topLay->addWidget(vdiv_);
    topLay->addWidget(inputsContainer_, 1);

    // ── Bottom Row: Steering ──────────────────────────────────────
    steeringContainer_ = new QWidget;
    QVBoxLayout* steeringLay = new QVBoxLayout(steeringContainer_);
    steeringLay->setContentsMargins(0, 0, 0, 0);
    steeringLay->setSpacing(0);
    steeringLay->addWidget(tnrui::makeChartHeader("STEERING TELEMETRY", {
        {"Steering", QColor("#BF5FFF")},
        {"(- Left / + Right)", QColor()}
    }));
    steeringChart_ = new SteeringChart;
    steeringChart_->setModel(model);
    steeringLay->addWidget(steeringChart_, 1);

    // ── Assemble Page ─────────────────────────────────────────────
    vbox->addWidget(topRow_, 1);
    hdiv_ = tnrui::hline();
    vbox->addWidget(hdiv_);
    vbox->addWidget(steeringContainer_, 1);

    applyLayout(loadLayout());
}

InputLayout InputPage::loadLayout()
{
    InputLayout L;
    settings_.beginGroup("inputLayout");
    L.showGear = settings_.value("showGear", true).toBool();
    L.showInputs = settings_.value("showInputs", true).toBool();
    L.showSteering = settings_.value("showSteering", true).toBool();
    settings_.endGroup();
    return L;
}

void InputPage::saveLayout(const InputLayout& L)
{
    settings_.beginGroup("inputLayout");
    settings_.setValue("showGear", L.showGear);
    settings_.setValue("showInputs", L.showInputs);
    settings_.setValue("showSteering", L.showSteering);
    settings_.endGroup();
}

void InputPage::applyLayout(const InputLayout& L)
{
    if (gearContainer_) gearContainer_->setVisible(L.showGear);
    if (inputsContainer_) inputsContainer_->setVisible(L.showInputs);
    if (steeringContainer_) steeringContainer_->setVisible(L.showSteering);

    bool topVisible = L.showGear || L.showInputs;
    if (topRow_) topRow_->setVisible(topVisible);
    if (vdiv_) vdiv_->setVisible(L.showGear && L.showInputs);
    if (hdiv_) hdiv_->setVisible(topVisible && L.showSteering);
}

void InputPage::applyAndSaveLayout(const InputLayout& L)
{
    applyLayout(L);
    saveLayout(L);
}

void InputPage::setPlaybackMode(bool on, float currentTime)
{
    if (gearChart_) gearChart_->setPlaybackMode(on);
    if (inputsChart_) inputsChart_->setPlaybackMode(on);
    if (steeringChart_) steeringChart_->setPlaybackMode(on);
    if (on) setCurrentTime(currentTime);
}

void InputPage::setCurrentTime(float t)
{
    if (gearChart_) gearChart_->setCurrentTime(t);
    if (inputsChart_) inputsChart_->setCurrentTime(t);
    if (steeringChart_) steeringChart_->setCurrentTime(t);
}

void InputPage::setWindowSeconds(float secs)
{
    if (gearChart_) gearChart_->setWindowSeconds(secs);
    if (inputsChart_) inputsChart_->setWindowSeconds(secs);
    if (steeringChart_) steeringChart_->setWindowSeconds(secs);
}
