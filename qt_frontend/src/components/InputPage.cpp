#include "InputPage.h"
#include "InputChartsWidget.h"

#include <QVBoxLayout>

InputPage::InputPage(SessionModel* model, QWidget* parent)
    : QWidget(parent)
{
    QVBoxLayout* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Gear / throttle-brake / steering are now panels of one ChartView (a single
    // QCustomPlot / OpenGL context / replot) — see InputChartsWidget.
    charts_ = new InputChartsWidget;
    charts_->setModel(model);
    vbox->addWidget(charts_, 1);

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
    if (!charts_) return;
    charts_->setSectionVisible(0, L.showGear);       // GEAR
    charts_->setSectionVisible(1, L.showInputs);     // INPUTS (throttle/brake)
    charts_->setSectionVisible(2, L.showSteering);   // STEERING
}

void InputPage::applyAndSaveLayout(const InputLayout& L)
{
    applyLayout(L);
    saveLayout(L);
}

void InputPage::setPlaybackMode(bool on, float currentTime)
{
    if (charts_) charts_->setPlaybackMode(on);
    if (on) setCurrentTime(currentTime);
}

void InputPage::setCurrentTime(float t)
{
    if (charts_) charts_->setCurrentTime(t);
}

void InputPage::setWindowSeconds(float secs)
{
    if (charts_) charts_->setWindowSeconds(secs);
}

void InputPage::setGraphSectionTable(int section, bool table)
{
    if (charts_) charts_->setSectionViewMode(section, table);
}
