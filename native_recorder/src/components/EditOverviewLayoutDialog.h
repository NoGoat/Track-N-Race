#pragma once

#include <QDialog>

#include "OverviewLayout.h"

class MainWindow;
class QPushButton;

// Modal "Edit Layout" dialog for the Overview page. Immediate-apply: every
// toggle click updates MainWindow's live card visibility and persists it
// right away — there's no separate Save/Cancel.
class EditOverviewLayoutDialog : public QDialog {
    Q_OBJECT

public:
    explicit EditOverviewLayoutDialog(MainWindow* mainWindow, QWidget* parent = nullptr);

private:
    void toggleChart(bool on);
    void toggleStat(int idx, bool on);
    void toggleDmg(int idx, bool on);
    void toggleTyreSection(bool on);

    MainWindow*    mainWindow_;
    OverviewLayout layout_;
    QPushButton*   statBtns_[OverviewLayout::StatCardCount] = {};
    QPushButton*   dmgBtns_[OverviewLayout::DmgCardCount]   = {};
    QPushButton*   chartBtn_       = nullptr;
    QPushButton*   tyreSectionBtn_ = nullptr;
};
