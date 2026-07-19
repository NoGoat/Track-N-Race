#include "MiscPage.h"
#include "MiscChartsWidget.h"

#include <QVBoxLayout>

MiscPage::MiscPage(SessionModel* model, QWidget* parent)
    : QWidget(parent)
{
    QVBoxLayout* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // G-force and ride-height are now panels of one ChartView (a single QCustomPlot
    // / OpenGL context / replot) — see MiscChartsWidget.
    charts_ = new MiscChartsWidget;
    charts_->setModel(model);
    vbox->addWidget(charts_, 1);

    applyLayout(loadLayout());
}

MiscLayout MiscPage::loadLayout()
{
    MiscLayout L;
    settings_.beginGroup("miscLayout");
    L.showGForce = settings_.value("showGForce", true).toBool();
    L.showRideHeight = settings_.value("showRideHeight", true).toBool();
    settings_.endGroup();
    return L;
}

void MiscPage::saveLayout(const MiscLayout& L)
{
    settings_.beginGroup("miscLayout");
    settings_.setValue("showGForce", L.showGForce);
    settings_.setValue("showRideHeight", L.showRideHeight);
    settings_.endGroup();
}

void MiscPage::applyLayout(const MiscLayout& L)
{
    if (!charts_) return;
    charts_->setSectionVisible(0, L.showGForce);       // GFORCE
    charts_->setSectionVisible(1, L.showRideHeight);   // RIDEHEIGHT
}

void MiscPage::applyAndSaveLayout(const MiscLayout& L)
{
    applyLayout(L);
    saveLayout(L);
}

void MiscPage::setPlaybackMode(bool on, float currentTime)
{
    if (charts_) charts_->setPlaybackMode(on);
    if (on) setCurrentTime(currentTime);
}

void MiscPage::setCurrentTime(float t)
{
    if (charts_) charts_->setCurrentTime(t);
}

void MiscPage::setWindowSeconds(float secs)
{
    if (charts_) charts_->setWindowSeconds(secs);
}

void MiscPage::setGraphSectionTable(int section, bool table)
{
    if (charts_) charts_->setSectionViewMode(section, table);
}
