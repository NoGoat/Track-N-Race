#pragma once

#include <QDialog>
#include "PowerLayout.h"

class PowerPage;
class QPushButton;

class EditPowerLayoutDialog : public QDialog {
    Q_OBJECT

public:
    explicit EditPowerLayoutDialog(PowerPage* page, QWidget* parent = nullptr);

private:
    void toggleCard(int idx, bool on);
    void toggleSplit(bool on);
    void toggleHarvest(bool on);
    void toggleStore(bool on);
    void toggleFuel(bool on);

    PowerPage*     page_;
    PowerLayout    layout_;
    QPushButton*   cardBtns_[PowerLayout::CardCount] = {};
    QPushButton*   splitBtn_ = nullptr;
    QPushButton*   harvestBtn_ = nullptr;
    QPushButton*   storeBtn_ = nullptr;
    QPushButton*   fuelBtn_ = nullptr;
};
