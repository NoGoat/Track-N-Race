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
    QWidget* topRow = new QWidget;
    QHBoxLayout* topLay = new QHBoxLayout(topRow);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(0);

    // Gear Chart
    QWidget* gearContainer = new QWidget;
    QVBoxLayout* gearLay = new QVBoxLayout(gearContainer);
    gearLay->setContentsMargins(0, 0, 0, 0);
    gearLay->setSpacing(0);
    gearLay->addWidget(makeHeader("GEAR INDICATOR"));
    gearChart_ = new GearChart;
    gearChart_->setModel(model_); // Pass the session model to the chart
    gearLay->addWidget(gearChart_, 1);

    // Inputs Chart (Throttle/Brake)
    QWidget* inputsContainer = new QWidget;
    QVBoxLayout* inputsLay = new QVBoxLayout(inputsContainer);
    inputsLay->setContentsMargins(0, 0, 0, 0);
    inputsLay->setSpacing(0);
    inputsLay->addWidget(makeHeader("THROTTLE / BRAKE CHART"));
    inputsChart_ = new InputsChart;
    inputsChart_->setModel(model_);
    inputsLay->addWidget(inputsChart_, 1);

    // Add to top row with a vertical divider
    topLay->addWidget(gearContainer, 1);
    QFrame* vdiv = new QFrame;
    vdiv->setFrameShape(QFrame::VLine);
    vdiv->setFrameShadow(QFrame::Sunken);
    topLay->addWidget(vdiv);
    topLay->addWidget(inputsContainer, 1);

    // ── Bottom Row: Steering ──────────────────────────────────────
    QWidget* steeringContainer = new QWidget;
    QVBoxLayout* steeringLay = new QVBoxLayout(steeringContainer);
    steeringLay->setContentsMargins(0, 0, 0, 0);
    steeringLay->setSpacing(0);
    steeringLay->addWidget(makeHeader("STEERING TELEMETRY"));
    steeringChart_ = new SteeringChart;
    steeringChart_->setModel(model_);
    steeringLay->addWidget(steeringChart_, 1);

    // ── Assemble Page ─────────────────────────────────────────────
    vbox->addWidget(topRow, 1);
    QFrame* hdiv = new QFrame;
    hdiv->setFrameShape(QFrame::HLine);
    hdiv->setFrameShadow(QFrame::Sunken);
    vbox->addWidget(hdiv);
    vbox->addWidget(steeringContainer, 1);

    // If the window-size segment changes, these charts won't automatically sync
    // unless we also wire up the windowGroup. In MainWindow::MainWindow, the
    // windowGroup changes chart->setWindowSeconds(). Here, since there are
    // multiple charts, we would normally manage them via a list or similar.
    // For now, they start with the default 30s. We can add signals later if needed.

    return w;
}
