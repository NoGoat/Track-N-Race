#include "EditDamageLayoutDialog.h"
#include "DamagePage.h"

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

EditDamageLayoutDialog::EditDamageLayoutDialog(DamagePage* page, QWidget* parent)
    : QDialog(parent), page_(page)
{
    setWindowTitle("Edit Damage Layout");
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);
    layout_ = page_->loadLayout();

    QVBoxLayout* main = new QVBoxLayout(this);
    main->setSizeConstraint(QLayout::SetFixedSize);

    auto toggle = [this](const QString& label, bool on, auto apply) {
        QPushButton* button = new ToggleButton(label);
        button->setCheckable(true);
        button->setChecked(on);
        connect(button, &QPushButton::toggled, this, [this, apply](bool checked) {
            apply(checked);
            page_->applyAndSaveLayout(layout_);
        });
        return button;
    };

    QGroupBox* statusBox = new QGroupBox("Status Bar");
    QHBoxLayout* statusLay = new QHBoxLayout(statusBox);
    for (int i = 0; i < DamageLayout::StatusCount; ++i)
        statusLay->addWidget(toggle(DamageLayout::statusLabel(i), layout_.statusCards[i],
                                    [this, i](bool on) { layout_.statusCards[i] = on; }));
    main->addWidget(statusBox);

    QGroupBox* diagramBox = new QGroupBox("Car Diagram");
    QHBoxLayout* diagramLay = new QHBoxLayout(diagramBox);
    QPushButton* diagram = toggle("Car Damage Diagram", layout_.showDiagram,
                                  [this](bool on) { layout_.showDiagram = on; });
    diagram->setMinimumHeight(72);
    diagramLay->addWidget(diagram);
    main->addWidget(diagramBox);

    QGroupBox* wearBox = new QGroupBox("Wear Bar");
    QHBoxLayout* wearLay = new QHBoxLayout(wearBox);
    for (int i = 0; i < DamageLayout::WearCount; ++i)
        wearLay->addWidget(toggle(DamageLayout::wearName(i), layout_.wearTiles[i],
                                  [this, i](bool on) { layout_.wearTiles[i] = on; }));
    main->addWidget(wearBox);

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(closeBtn);
    main->addLayout(bottom);
}
