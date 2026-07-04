#include "MiscPage.h"
#include "PageUiHelpers.h"
#include "GForceChart.h"
#include "RideHeightChart.h"

#include <QVBoxLayout>

MiscPage::MiscPage(SessionModel* model, QWidget* parent)
    : QWidget(parent)
{
    QVBoxLayout* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // ── G-Force Chart ──────────────────────────────────
    gforceContainer_ = new QWidget;
    QVBoxLayout* gfLay = new QVBoxLayout(gforceContainer_);
    gfLay->setContentsMargins(0, 0, 0, 0);
    gfLay->setSpacing(0);
    gfLay->addWidget(tnrui::makeChartHeader("G-FORCE", {
        {"Lateral", QColor("#F0A500")},
        {"Longitudinal", QColor("#5794F2")}
    }));
    gforceChart_ = new GForceChart;
    gforceChart_->setModel(model);
    gfLay->addWidget(gforceChart_, 1);

    hdiv_ = tnrui::hline();

    // ── Ride Height Chart ──────────────────────────────────────
    rideHeightContainer_ = new QWidget;
    QVBoxLayout* rhLay = new QVBoxLayout(rideHeightContainer_);
    rhLay->setContentsMargins(0, 0, 0, 0);
    rhLay->setSpacing(0);
    rhLay->addWidget(tnrui::makeChartHeader("RIDE HEIGHT", {
        {"Front", QColor("#73BF69")},
        {"Rear", QColor("#B877DB")}
    }));
    rideHeightChart_ = new RideHeightChart;
    rideHeightChart_->setModel(model);
    rhLay->addWidget(rideHeightChart_, 1);

    // ── Assemble Page ─────────────────────────────────────────────
    vbox->addWidget(gforceContainer_, 1);
    vbox->addWidget(hdiv_);
    vbox->addWidget(rideHeightContainer_, 1);

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
    if (gforceContainer_) gforceContainer_->setVisible(L.showGForce);
    if (rideHeightContainer_) rideHeightContainer_->setVisible(L.showRideHeight);

    if (hdiv_) hdiv_->setVisible(L.showGForce && L.showRideHeight);
}

void MiscPage::applyAndSaveLayout(const MiscLayout& L)
{
    applyLayout(L);
    saveLayout(L);
}

void MiscPage::setPlaybackMode(bool on, float currentTime)
{
    if (gforceChart_) gforceChart_->setPlaybackMode(on);
    if (rideHeightChart_) rideHeightChart_->setPlaybackMode(on);
    if (on) setCurrentTime(currentTime);
}

void MiscPage::setCurrentTime(float t)
{
    if (gforceChart_) gforceChart_->setCurrentTime(t);
    if (rideHeightChart_) rideHeightChart_->setCurrentTime(t);
}

void MiscPage::setWindowSeconds(float secs)
{
    if (gforceChart_) gforceChart_->setWindowSeconds(secs);
    if (rideHeightChart_) rideHeightChart_->setWindowSeconds(secs);
}
