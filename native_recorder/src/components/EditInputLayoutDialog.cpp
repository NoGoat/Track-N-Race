#include "EditInputLayoutDialog.h"
#include "InputPage.h"

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
    QHBoxLayout* chartLay = new QHBoxLayout(chartBox);
    
    gearBtn_ = new ToggleButton("Gear Indicator");
    gearBtn_->setCheckable(true);
    gearBtn_->setChecked(layout_.showGear);
    connect(gearBtn_, &QPushButton::toggled, this, &EditInputLayoutDialog::toggleGear);
    chartLay->addWidget(gearBtn_);

    inputsBtn_ = new ToggleButton("Throttle / Brake Chart");
    inputsBtn_->setCheckable(true);
    inputsBtn_->setChecked(layout_.showInputs);
    connect(inputsBtn_, &QPushButton::toggled, this, &EditInputLayoutDialog::toggleInputs);
    chartLay->addWidget(inputsBtn_);

    steeringBtn_ = new ToggleButton("Steering Telemetry");
    steeringBtn_->setCheckable(true);
    steeringBtn_->setChecked(layout_.showSteering);
    connect(steeringBtn_, &QPushButton::toggled, this, &EditInputLayoutDialog::toggleSteering);
    chartLay->addWidget(steeringBtn_);

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

void EditInputLayoutDialog::toggleInputs(bool on) {
    layout_.showInputs = on;
    page_->applyAndSaveLayout(layout_);
}

void EditInputLayoutDialog::toggleSteering(bool on) {
    layout_.showSteering = on;
    page_->applyAndSaveLayout(layout_);
}
