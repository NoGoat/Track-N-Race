#pragma once

#include <QDialog>
#include "MiscLayout.h"

class MiscPage;

class EditMiscLayoutDialog : public QDialog {
    Q_OBJECT
public:
    explicit EditMiscLayoutDialog(MiscPage* page, QWidget* parent = nullptr);

private:
    MiscPage* page_;
    MiscLayout layout_;

};
