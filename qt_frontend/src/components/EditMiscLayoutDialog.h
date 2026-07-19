#pragma once

#include <QDialog>
#include "MiscLayout.h"

class MiscPage;
class QPushButton;

class EditMiscLayoutDialog : public QDialog {
    Q_OBJECT
public:
    explicit EditMiscLayoutDialog(MiscPage* page, QWidget* parent = nullptr);

private slots:
    void toggleGForce(bool on);
    void toggleRideHeight(bool on);

private:
    MiscPage* page_;
    MiscLayout layout_;

    QPushButton* gforceBtn_ = nullptr;
    QPushButton* rideHeightBtn_ = nullptr;
};
