#include "EditMiscLayoutDialog.h"
#include "../MainWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QStyleOptionButton>

namespace {
class ToggleButton : public QPushButton {
public:
    using QPushButton::QPushButton;
protected:
    void initStyleOption(QStyleOptionButton* option) const override {
        QPushButton::initStyleOption(option);
        if (isChecked()) {
            option->features |= QStyleOptionButton::DefaultButton;
            option->state &= ~QStyle::State_On;
            option->state |= QStyle::State_Raised;
        }
    }
};
}

EditMiscLayoutDialog::EditMiscLayoutDialog(MainWindow* mainWindow, QWidget* parent)
    : QDialog(parent), mainWindow_(mainWindow)
{
    setWindowTitle("Edit Misc Layout");
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);
    layout_ = mainWindow_->loadMiscLayout();

    QVBoxLayout* main = new QVBoxLayout(this);
    main->setSizeConstraint(QLayout::SetFixedSize);

    QGroupBox* chartBox = new QGroupBox("Charts");
    QHBoxLayout* chartLay = new QHBoxLayout(chartBox);
    
    gforceBtn_ = new ToggleButton("G-Force");
    gforceBtn_->setCheckable(true);
    gforceBtn_->setChecked(layout_.showGForce);
    connect(gforceBtn_, &QPushButton::toggled, this, &EditMiscLayoutDialog::toggleGForce);
    chartLay->addWidget(gforceBtn_);

    rideHeightBtn_ = new ToggleButton("Ride Height");
    rideHeightBtn_->setCheckable(true);
    rideHeightBtn_->setChecked(layout_.showRideHeight);
    connect(rideHeightBtn_, &QPushButton::toggled, this, &EditMiscLayoutDialog::toggleRideHeight);
    chartLay->addWidget(rideHeightBtn_);

    main->addWidget(chartBox);

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(closeBtn);
    main->addLayout(bottom);
}

void EditMiscLayoutDialog::toggleGForce(bool on) {
    layout_.showGForce = on;
    mainWindow_->applyAndSaveMiscLayout(layout_);
}

void EditMiscLayoutDialog::toggleRideHeight(bool on) {
    layout_.showRideHeight = on;
    mainWindow_->applyAndSaveMiscLayout(layout_);
}
