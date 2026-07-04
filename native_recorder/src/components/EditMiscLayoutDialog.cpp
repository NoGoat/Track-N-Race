#include "EditMiscLayoutDialog.h"
#include "MiscPage.h"

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

EditMiscLayoutDialog::EditMiscLayoutDialog(MiscPage* page, QWidget* parent)
    : QDialog(parent), page_(page)
{
    setWindowTitle("Edit Misc Layout");
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);
    layout_ = page_->loadLayout();

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
    page_->applyAndSaveLayout(layout_);
}

void EditMiscLayoutDialog::toggleRideHeight(bool on) {
    layout_.showRideHeight = on;
    page_->applyAndSaveLayout(layout_);
}
