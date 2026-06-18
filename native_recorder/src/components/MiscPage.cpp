#include "../MainWindow.h"
#include "GForceChart.h"
#include "RideHeightChart.h"
#include "MiscLayout.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>
#include <QSettings>

QWidget* MainWindow::buildMiscPage() {
    QWidget* w = new QWidget;
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    auto makeHeader = [](const QString& titleText, const QList<QPair<QString, QColor>>& legend = {}) -> QWidget* {
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
        
        for (const auto& item : legend) {
            if (item.second.isValid() && item.second != Qt::transparent) {
                QWidget* colorBox = new QWidget;
                colorBox->setFixedSize(12, 12);
                colorBox->setStyleSheet(QString("background-color: %1; border-radius: 2px;").arg(item.second.name()));
                hl->addWidget(colorBox);
            }
            QLabel* legLabel = new QLabel(item.first);
            QFont lf; lf.setPointSize(8);
            legLabel->setFont(lf);
            legLabel->setForegroundRole(QPalette::PlaceholderText);
            
            hl->addWidget(legLabel);
            hl->addSpacing(8);
        }
        
        return header;
    };

    // ── G-Force Chart ──────────────────────────────────
    misc_gforceContainer_ = new QWidget;
    QVBoxLayout* gfLay = new QVBoxLayout(misc_gforceContainer_);
    gfLay->setContentsMargins(0, 0, 0, 0);
    gfLay->setSpacing(0);
    gfLay->addWidget(makeHeader("G-FORCE", {
        {"Lateral", QColor("#F0A500")},
        {"Longitudinal", QColor("#5794F2")}
    }));
    gforceChart_ = new GForceChart;
    gforceChart_->setModel(model_);
    gfLay->addWidget(gforceChart_, 1);

    misc_hdiv_ = new QFrame;
    misc_hdiv_->setFrameShape(QFrame::HLine);
    misc_hdiv_->setFrameShadow(QFrame::Sunken);

    // ── Ride Height Chart ──────────────────────────────────────
    misc_rideHeightContainer_ = new QWidget;
    QVBoxLayout* rhLay = new QVBoxLayout(misc_rideHeightContainer_);
    rhLay->setContentsMargins(0, 0, 0, 0);
    rhLay->setSpacing(0);
    rhLay->addWidget(makeHeader("RIDE HEIGHT", {
        {"Front", QColor("#73BF69")},
        {"Rear", QColor("#B877DB")}
    }));
    rideHeightChart_ = new RideHeightChart;
    rideHeightChart_->setModel(model_);
    rhLay->addWidget(rideHeightChart_, 1);

    // ── Assemble Page ─────────────────────────────────────────────
    vbox->addWidget(misc_gforceContainer_, 1);
    vbox->addWidget(misc_hdiv_);
    vbox->addWidget(misc_rideHeightContainer_, 1);

    applyMiscLayout(loadMiscLayout());

    return w;
}

MiscLayout MainWindow::loadMiscLayout()
{
    MiscLayout L;
    settings.beginGroup("miscLayout");
    L.showGForce = settings.value("showGForce", true).toBool();
    L.showRideHeight = settings.value("showRideHeight", true).toBool();
    settings.endGroup();
    return L;
}

void MainWindow::saveMiscLayout(const MiscLayout& L)
{
    settings.beginGroup("miscLayout");
    settings.setValue("showGForce", L.showGForce);
    settings.setValue("showRideHeight", L.showRideHeight);
    settings.endGroup();
}

void MainWindow::applyMiscLayout(const MiscLayout& L)
{
    if (misc_gforceContainer_) misc_gforceContainer_->setVisible(L.showGForce);
    if (misc_rideHeightContainer_) misc_rideHeightContainer_->setVisible(L.showRideHeight);

    if (misc_hdiv_) misc_hdiv_->setVisible(L.showGForce && L.showRideHeight);
}

void MainWindow::applyAndSaveMiscLayout(const MiscLayout& L)
{
    applyMiscLayout(L);
    saveMiscLayout(L);
}
