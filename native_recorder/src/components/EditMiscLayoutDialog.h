#pragma once

#include <QDialog>
#include "MiscLayout.h"

class MainWindow;
class QPushButton;

class EditMiscLayoutDialog : public QDialog {
    Q_OBJECT
public:
    explicit EditMiscLayoutDialog(MainWindow* mainWindow, QWidget* parent = nullptr);

private slots:
    void toggleGForce(bool on);
    void toggleRideHeight(bool on);

private:
    MainWindow* mainWindow_;
    MiscLayout layout_;

    QPushButton* gforceBtn_ = nullptr;
    QPushButton* rideHeightBtn_ = nullptr;
};
