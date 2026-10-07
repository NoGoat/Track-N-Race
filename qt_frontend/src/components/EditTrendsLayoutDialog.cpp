#include "EditTrendsLayoutDialog.h"
#include "TrendsPage.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QVBoxLayout>

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

EditTrendsLayoutDialog::EditTrendsLayoutDialog(TrendsPage* page, QWidget* parent)
    : QDialog(parent), page_(page)
{
    setWindowTitle("Edit Trends Layout");
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);
    layout_ = page_->loadLayout();

    QVBoxLayout* main = new QVBoxLayout(this);
    main->setSizeConstraint(QLayout::SetFixedSize);

    auto toggle = [this](const QString& label, bool* flag) {
        QPushButton* button = new ToggleButton(label);
        button->setCheckable(true);
        button->setChecked(*flag);
        connect(button, &QPushButton::toggled, this, [this, flag](bool on) {
            *flag = on;
            page_->applyAndSaveLayout(layout_);
        });
        return button;
    };

    QGroupBox* statsBox = new QGroupBox("Stats Bar");
    QHBoxLayout* statsLay = new QHBoxLayout(statsBox);
    for (int i = 0; i < TrendsLayout::StatCount; ++i)
        statsLay->addWidget(toggle(TrendsLayout::statLabel(i), &layout_.statsCards[i]));
    main->addWidget(statsBox);

    // The chart layout in use decides which charts there are to show.
    QGroupBox* chartBox = new QGroupBox("Charts");
    QGridLayout* chartLay = new QGridLayout(chartBox);
    const QString tyreLabel = page_->tyreLifeMode() ? QStringLiteral("Tyre Life") : QStringLiteral("Tyre Wear");
    const TrendsChartLayout chartLayout = page_->chartLayout();
    if (chartLayout == TrendsChartLayout::Separate) {
        QPushButton* lapTimes = toggle("Lap Times", &layout_.charts[TrendsLayout::LapTimes]);
        QPushButton* ersUsage = toggle("ERS Usage", &layout_.charts[TrendsLayout::ErsUsage]);
        QPushButton* recharge = toggle("Recharge", &layout_.charts[TrendsLayout::Recharge]);
        QPushButton* tyres = toggle(tyreLabel, &layout_.charts[TrendsLayout::TyreWear]);
        for (QPushButton* button : {lapTimes, ersUsage, recharge, tyres}) button->setMinimumHeight(48);
        chartLay->addWidget(lapTimes, 0, 0, 1, 2);
        chartLay->addWidget(ersUsage, 1, 0);
        chartLay->addWidget(recharge, 1, 1);
        chartLay->addWidget(tyres, 2, 0, 1, 2);
    } else {
        QPushButton* single = chartLayout == TrendsChartLayout::Bars
            ? toggle(tyreLabel + QStringLiteral(" · ERS · Lap Times"), &layout_.charts[TrendsLayout::Bars])
            : toggle(QStringLiteral("Lap Times · ERS · ") + tyreLabel, &layout_.charts[TrendsLayout::Combined]);
        single->setMinimumHeight(96);
        chartLay->addWidget(single, 0, 0);
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
