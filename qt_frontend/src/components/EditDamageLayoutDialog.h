#pragma once

#include <QDialog>
#include "DamageLayout.h"

class DamagePage;

// Electron's Damage layout editor: the status bar cards, the car diagram and
// the wear bar tiles, applied and saved as each toggle changes.
class EditDamageLayoutDialog : public QDialog {
    Q_OBJECT

public:
    explicit EditDamageLayoutDialog(DamagePage* page, QWidget* parent = nullptr);

private:
    DamagePage*  page_;
    DamageLayout layout_;
};
