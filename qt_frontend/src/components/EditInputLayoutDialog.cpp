#include "EditInputLayoutDialog.h"
#include "InputPage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
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

EditInputLayoutDialog::EditInputLayoutDialog(InputPage* page, QWidget* parent)
    : QDialog(parent), page_(page)
{
    setWindowTitle("Edit Input Layout");
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);
    layout_ = page_->loadLayout();

    QVBoxLayout* main = new QVBoxLayout(this);
    main->setSizeConstraint(QLayout::SetFixedSize);

    QGroupBox* chartBox = new QGroupBox("Charts");
    QGridLayout* chartLay = new QGridLayout(chartBox);
    
    gearBtn_ = new ToggleButton("Gear Indicator");
    gearBtn_->setCheckable(true);
    gearBtn_->setChecked(layout_.showGear);
    connect(gearBtn_, &QPushButton::toggled, this, &EditInputLayoutDialog::toggleGear);
    acceleratorBtn_ = new ToggleButton;
    acceleratorBtn_->setCheckable(true);

    const InputPedalLayout pedals = page_->pedalLayout();
    const InputPageLayout arrangement = page_->pageLayout();
    if (pedals == InputPedalLayout::Split) {
        acceleratorBtn_->setText("Accelerator");
        acceleratorBtn_->setChecked(layout_.showAccelerator);
        connect(acceleratorBtn_, &QPushButton::toggled, this,
                &EditInputLayoutDialog::toggleAccelerator);
        brakeBtn_ = new ToggleButton("Brake");
        brakeBtn_->setCheckable(true);
        brakeBtn_->setChecked(layout_.showBrake);
        connect(brakeBtn_, &QPushButton::toggled, this,
                &EditInputLayoutDialog::toggleBrake);
    } else {
        acceleratorBtn_->setText(pedals == InputPedalLayout::Combined2
            ? "Accelerator / Brake (Combined 2)" : "Accelerator / Brake");
        acceleratorBtn_->setChecked(layout_.showAccelerator || layout_.showBrake);
        connect(acceleratorBtn_, &QPushButton::toggled, this,
                &EditInputLayoutDialog::toggleCombined);
    }

    steeringBtn_ = new ToggleButton("Steering Telemetry");
    steeringBtn_->setCheckable(true);
    steeringBtn_->setChecked(layout_.showSteering);
    connect(steeringBtn_, &QPushButton::toggled, this, &EditInputLayoutDialog::toggleSteering);
    if (arrangement == InputPageLayout::Vertical) {
        int row = 0;
        chartLay->addWidget(gearBtn_, row++, 0);
        chartLay->addWidget(acceleratorBtn_, row++, 0);
        if (brakeBtn_) chartLay->addWidget(brakeBtn_, row++, 0);
        chartLay->addWidget(steeringBtn_, row, 0);
    } else if (pedals == InputPedalLayout::Split) {
        chartLay->addWidget(acceleratorBtn_, 0, 0);
        chartLay->addWidget(brakeBtn_, 0, 1);
        chartLay->addWidget(gearBtn_, 1, 0);
        chartLay->addWidget(steeringBtn_, 1, 1);
    } else {
        chartLay->addWidget(gearBtn_, 0, 0);
        chartLay->addWidget(acceleratorBtn_, 0, 1);
        chartLay->addWidget(steeringBtn_, 1, 0, 1, 2);
    }

    main->addWidget(chartBox);

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(closeBtn);
    main->addLayout(bottom);
}

void EditInputLayoutDialog::toggleGear(bool on) {
    layout_.showGear = on;
    page_->applyAndSaveLayout(layout_);
}

void EditInputLayoutDialog::toggleAccelerator(bool on) {
    layout_.showAccelerator = on;
    page_->applyAndSaveLayout(layout_);
}

void EditInputLayoutDialog::toggleBrake(bool on) {
    layout_.showBrake = on;
    page_->applyAndSaveLayout(layout_);
}

void EditInputLayoutDialog::toggleCombined(bool on) {
    layout_.showAccelerator = on;
    layout_.showBrake = on;
    page_->applyAndSaveLayout(layout_);
}

void EditInputLayoutDialog::toggleSteering(bool on) {
    layout_.showSteering = on;
    page_->applyAndSaveLayout(layout_);
}
