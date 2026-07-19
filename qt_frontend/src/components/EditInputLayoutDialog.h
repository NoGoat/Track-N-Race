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
    void toggleInputs(bool on);
    void toggleSteering(bool on);

    InputPage*     page_;
    InputLayout    layout_;
    QPushButton*   gearBtn_ = nullptr;
    QPushButton*   inputsBtn_ = nullptr;
    QPushButton*   steeringBtn_ = nullptr;
};
