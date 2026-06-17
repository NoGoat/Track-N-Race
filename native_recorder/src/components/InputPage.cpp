#include "../MainWindow.h"
#include "GearChart.h"
#include "InputsChart.h"
#include "SteeringChart.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>

QWidget* MainWindow::buildInputPage() {
    QWidget* w = new QWidget;
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Helper to create a styled header for each chart section
    auto makeHeader = [](const QString& titleText) -> QWidget* {
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
    };

    // ── Top Row: Gear and Inputs ──────────────────────────────────
    ip_topRow_ = new QWidget;
    QHBoxLayout* topLay = new QHBoxLayout(ip_topRow_);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(0);

    // Gear Chart
    ip_gearContainer_ = new QWidget;
    QVBoxLayout* gearLay = new QVBoxLayout(ip_gearContainer_);
    gearLay->setContentsMargins(0, 0, 0, 0);
    gearLay->setSpacing(0);
    gearLay->addWidget(makeHeader("GEAR INDICATOR"));
    gearChart_ = new GearChart;
    gearChart_->setModel(model_); // Pass the session model to the chart
    gearLay->addWidget(gearChart_, 1);

    // Inputs Chart (Throttle/Brake)
    ip_inputsContainer_ = new QWidget;
    QVBoxLayout* inputsLay = new QVBoxLayout(ip_inputsContainer_);
    inputsLay->setContentsMargins(0, 0, 0, 0);
    inputsLay->setSpacing(0);
    inputsLay->addWidget(makeHeader("THROTTLE / BRAKE CHART"));
    inputsChart_ = new InputsChart;
    inputsChart_->setModel(model_);
    inputsLay->addWidget(inputsChart_, 1);

    // Add to top row with a vertical divider
    topLay->addWidget(ip_gearContainer_, 1);
    ip_vdiv_ = new QFrame;
    ip_vdiv_->setFrameShape(QFrame::VLine);
    ip_vdiv_->setFrameShadow(QFrame::Sunken);
    topLay->addWidget(ip_vdiv_);
    topLay->addWidget(ip_inputsContainer_, 1);

    // ── Bottom Row: Steering ──────────────────────────────────────
    ip_steeringContainer_ = new QWidget;
    QVBoxLayout* steeringLay = new QVBoxLayout(ip_steeringContainer_);
    steeringLay->setContentsMargins(0, 0, 0, 0);
    steeringLay->setSpacing(0);
    steeringLay->addWidget(makeHeader("STEERING TELEMETRY"));
    steeringChart_ = new SteeringChart;
    steeringChart_->setModel(model_);
    steeringLay->addWidget(steeringChart_, 1);

    // ── Assemble Page ─────────────────────────────────────────────
    vbox->addWidget(ip_topRow_, 1);
    ip_hdiv_ = new QFrame;
    ip_hdiv_->setFrameShape(QFrame::HLine);
    ip_hdiv_->setFrameShadow(QFrame::Sunken);
    vbox->addWidget(ip_hdiv_);
    vbox->addWidget(ip_steeringContainer_, 1);

    applyInputLayout(loadInputLayout());

    return w;
}

InputLayout MainWindow::loadInputLayout()
{
    InputLayout L;
    settings.beginGroup("inputLayout");
    L.showGear = settings.value("showGear", true).toBool();
    L.showInputs = settings.value("showInputs", true).toBool();
    L.showSteering = settings.value("showSteering", true).toBool();
    settings.endGroup();
    return L;
}

void MainWindow::saveInputLayout(const InputLayout& L)
{
    settings.beginGroup("inputLayout");
    settings.setValue("showGear", L.showGear);
    settings.setValue("showInputs", L.showInputs);
    settings.setValue("showSteering", L.showSteering);
    settings.endGroup();
}

void MainWindow::applyInputLayout(const InputLayout& L)
{
    if (ip_gearContainer_) ip_gearContainer_->setVisible(L.showGear);
    if (ip_inputsContainer_) ip_inputsContainer_->setVisible(L.showInputs);
    if (ip_steeringContainer_) ip_steeringContainer_->setVisible(L.showSteering);

    bool topVisible = L.showGear || L.showInputs;
    if (ip_topRow_) ip_topRow_->setVisible(topVisible);
    if (ip_vdiv_) ip_vdiv_->setVisible(L.showGear && L.showInputs);
    if (ip_hdiv_) ip_hdiv_->setVisible(topVisible && L.showSteering);
}

void MainWindow::applyAndSaveInputLayout(const InputLayout& L)
{
    applyInputLayout(L);
    saveInputLayout(L);
}
