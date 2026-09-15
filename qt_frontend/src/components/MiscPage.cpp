#include "MiscPage.h"
#include "MiscChartsWidget.h"

#include <QVBoxLayout>

MiscPage::MiscPage(SessionModel* model, QWidget* parent)
    : QWidget(parent)
{
    QVBoxLayout* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // G-force and ride-height are now panels of one ChartView (a single QRhi target
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
    L.showGLateral = settings_.value("showGLateral", true).toBool();
    L.showGLongitudinal = settings_.value("showGLongitudinal", true).toBool();
    L.showRideFront = settings_.value("showRideFront", true).toBool();
    L.showRideRear = settings_.value("showRideRear", true).toBool();
    settings_.endGroup();
    return L;
}

void MiscPage::saveLayout(const MiscLayout& L)
{
    settings_.beginGroup("miscLayout");
    settings_.setValue("showGForce", L.showGForce);
    settings_.setValue("showRideHeight", L.showRideHeight);
    settings_.setValue("showGLateral", L.showGLateral);
    settings_.setValue("showGLongitudinal", L.showGLongitudinal);
    settings_.setValue("showRideFront", L.showRideFront);
    settings_.setValue("showRideRear", L.showRideRear);
    settings_.endGroup();
}

void MiscPage::applyLayout(const MiscLayout& L)
{
    if (!charts_) return;
    const bool g = splitLayout(true), ride = splitLayout(false);
    charts_->setSectionVisible(0, !g && L.showGForce);
    charts_->setSectionVisible(1, !ride && L.showRideHeight);
    charts_->setSectionVisible(2, g && L.showGLateral);
    charts_->setSectionVisible(3, g && L.showGLongitudinal);
    charts_->setSectionVisible(4, ride && L.showRideFront);
    charts_->setSectionVisible(5, ride && L.showRideRear);
}

void MiscPage::applyAndSaveLayout(const MiscLayout& L)
{
    applyLayout(L);
    saveLayout(L);
    emit layoutChanged();
}

bool MiscPage::splitLayout(bool gForce) const {
    return settings_.value(gForce ? "pageLayouts/miscGForce" : "pageLayouts/miscRideHeight",
                           "combined").toString() == "split";
}

void MiscPage::setSplitLayout(bool gForce, bool split) {
    settings_.setValue(gForce ? "pageLayouts/miscGForce" : "pageLayouts/miscRideHeight",
                        split ? "split" : "combined");
    applyLayout(loadLayout());
    emit layoutChanged();
}

QVector<tnr::GraphSection> MiscPage::chartSections() {
    const MiscLayout l = loadLayout();
    QVector<tnr::GraphSection> result;
    if (splitLayout(true)) {
        if (l.showGLateral) result.append(tnr::GraphSection::MiscGLateral);
        if (l.showGLongitudinal) result.append(tnr::GraphSection::MiscGLongitudinal);
    } else if (l.showGForce) result.append(tnr::GraphSection::MiscGForce);
    if (splitLayout(false)) {
        if (l.showRideFront) result.append(tnr::GraphSection::MiscRideFront);
        if (l.showRideRear) result.append(tnr::GraphSection::MiscRideRear);
    } else if (l.showRideHeight) result.append(tnr::GraphSection::MiscRideHeight);
    return result;
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
