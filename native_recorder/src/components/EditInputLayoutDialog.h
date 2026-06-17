#pragma once

#include <QDialog>
#include "InputLayout.h"

class MainWindow;
class QPushButton;

class EditInputLayoutDialog : public QDialog {
    Q_OBJECT

public:
    explicit EditInputLayoutDialog(MainWindow* mainWindow, QWidget* parent = nullptr);

private:
    void toggleGear(bool on);
    void toggleInputs(bool on);
    void toggleSteering(bool on);

    MainWindow*    mainWindow_;
    InputLayout    layout_;
    QPushButton*   gearBtn_ = nullptr;
    QPushButton*   inputsBtn_ = nullptr;
    QPushButton*   steeringBtn_ = nullptr;
};
