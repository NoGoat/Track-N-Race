#include "EditInputLayoutDialog.h"
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

EditInputLayoutDialog::EditInputLayoutDialog(MainWindow* mainWindow, QWidget* parent)
    : QDialog(parent), mainWindow_(mainWindow)
{
    setWindowTitle("Edit Input Layout");
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);
    layout_ = mainWindow_->loadInputLayout();

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
    mainWindow_->applyAndSaveInputLayout(layout_);
}

void EditInputLayoutDialog::toggleInputs(bool on) {
    layout_.showInputs = on;
    mainWindow_->applyAndSaveInputLayout(layout_);
}

void EditInputLayoutDialog::toggleSteering(bool on) {
    layout_.showSteering = on;
    mainWindow_->applyAndSaveInputLayout(layout_);
}
