#include "EditPowerLayoutDialog.h"
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

EditPowerLayoutDialog::EditPowerLayoutDialog(MainWindow* mainWindow, QWidget* parent)
    : QDialog(parent), mainWindow_(mainWindow)
{
    setWindowTitle("Edit Power Layout");
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);
    layout_ = mainWindow_->loadPowerLayout();

    QVBoxLayout* main = new QVBoxLayout(this);
    main->setSizeConstraint(QLayout::SetFixedSize);

    // ── Stats ────────────────────────────────────────────────────
    QGroupBox* statsBox = new QGroupBox("Stats");
    QGridLayout* statsLay = new QGridLayout(statsBox);
    for (int i = 0; i < PowerLayout::CardCount; ++i) {
        QPushButton* b = new ToggleButton(PowerLayout::cardLabel(i));
        b->setCheckable(true);
        b->setChecked(layout_.cards[i]);
        connect(b, &QPushButton::toggled, this, [this, i](bool on) { toggleCard(i, on); });
        cardBtns_[i] = b;
        statsLay->addWidget(b, i / 4, i % 4);
    }
    main->addWidget(statsBox);

    QGroupBox* chartBox = new QGroupBox("Charts");
    QHBoxLayout* chartLay = new QHBoxLayout(chartBox);
    
    splitBtn_ = new ToggleButton("Power Split");
    splitBtn_->setCheckable(true);
    splitBtn_->setChecked(layout_.showSplit);
    connect(splitBtn_, &QPushButton::toggled, this, &EditPowerLayoutDialog::toggleSplit);
    chartLay->addWidget(splitBtn_);

    harvestBtn_ = new ToggleButton("Energy Harvest");
    harvestBtn_->setCheckable(true);
    harvestBtn_->setChecked(layout_.showHarvest);
    connect(harvestBtn_, &QPushButton::toggled, this, &EditPowerLayoutDialog::toggleHarvest);
    chartLay->addWidget(harvestBtn_);

    storeBtn_ = new ToggleButton("Energy Store");
    storeBtn_->setCheckable(true);
    storeBtn_->setChecked(layout_.showStore);
    connect(storeBtn_, &QPushButton::toggled, this, &EditPowerLayoutDialog::toggleStore);
    chartLay->addWidget(storeBtn_);

    fuelBtn_ = new ToggleButton("Fuel Usage");
    fuelBtn_->setCheckable(true);
    fuelBtn_->setChecked(layout_.showFuel);
    connect(fuelBtn_, &QPushButton::toggled, this, &EditPowerLayoutDialog::toggleFuel);
    chartLay->addWidget(fuelBtn_);

    main->addWidget(chartBox);

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(closeBtn);
    main->addLayout(bottom);
}

void EditPowerLayoutDialog::toggleCard(int idx, bool on) {
    layout_.cards[idx] = on;
    mainWindow_->applyAndSavePowerLayout(layout_);
}

void EditPowerLayoutDialog::toggleSplit(bool on) {
    layout_.showSplit = on;
    mainWindow_->applyAndSavePowerLayout(layout_);
}

void EditPowerLayoutDialog::toggleHarvest(bool on) {
    layout_.showHarvest = on;
    mainWindow_->applyAndSavePowerLayout(layout_);
}

void EditPowerLayoutDialog::toggleStore(bool on) {
    layout_.showStore = on;
    mainWindow_->applyAndSavePowerLayout(layout_);
}

void EditPowerLayoutDialog::toggleFuel(bool on) {
    layout_.showFuel = on;
    mainWindow_->applyAndSavePowerLayout(layout_);
}
