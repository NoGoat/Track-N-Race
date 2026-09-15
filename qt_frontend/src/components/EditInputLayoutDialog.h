#pragma once

#include <QDialog>
#include "InputLayout.h"

class InputPage;
class QPushButton;

class EditInputLayoutDialog : public QDialog {
    Q_OBJECT

public:
    explicit EditInputLayoutDialog(InputPage* page, QWidget* parent = nullptr);

private:
    void toggleGear(bool on);
    void toggleAccelerator(bool on);
    void toggleBrake(bool on);
    void toggleCombined(bool on);
    void toggleSteering(bool on);

    InputPage*     page_;
    InputLayout    layout_;
    QPushButton*   gearBtn_ = nullptr;
    QPushButton*   acceleratorBtn_ = nullptr;
    QPushButton*   brakeBtn_ = nullptr;
    QPushButton*   steeringBtn_ = nullptr;
};
