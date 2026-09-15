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
    auto* chartLay = new QVBoxLayout(chartBox);
    auto addToggle = [&](const QString& label, bool MiscLayout::* field) {
        auto* button = new ToggleButton(label);
        button->setCheckable(true);
        button->setChecked(layout_.*field);
        connect(button, &QPushButton::toggled, this, [this, field](bool on) {
            layout_.*field = on;
            page_->applyAndSaveLayout(layout_);
        });
        chartLay->addWidget(button);
    };
    if (page_->splitLayout(true)) {
        addToggle("Lateral G-Force", &MiscLayout::showGLateral);
        addToggle("Longitudinal G-Force", &MiscLayout::showGLongitudinal);
    } else addToggle("G-Force", &MiscLayout::showGForce);
    if (page_->splitLayout(false)) {
        addToggle("Front Ride Height", &MiscLayout::showRideFront);
        addToggle("Rear Ride Height", &MiscLayout::showRideRear);
    } else addToggle("Ride Height", &MiscLayout::showRideHeight);

    main->addWidget(chartBox);

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(closeBtn);
    main->addLayout(bottom);
}
