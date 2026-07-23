#pragma once

#include <QDialog>

#include "OverviewLayout.h"

class OverviewPage;
class QPushButton;
class QWidget;

// Modal "Edit Layout" dialog for the Overview page. Immediate-apply: every
// toggle click updates the Overview page's live card visibility and persists it
// right away — there's no separate Save/Cancel.
class EditOverviewLayoutDialog : public QDialog {
    Q_OBJECT

public:
    explicit EditOverviewLayoutDialog(OverviewPage* page, QWidget* parent = nullptr);

private:
    void toggleChart(bool on);
    void toggleStat(int idx, bool on);
    void toggleDmg(int idx, bool on);
    void toggleTyreCard(int i, bool on);
    void toggleTyreChart(int i, bool on);

    OverviewPage*  page_;
    OverviewLayout layout_;
    QPushButton*   statBtns_[OverviewLayout::StatCardCount]    = {};
    QPushButton*   dmgBtns_[OverviewLayout::DmgCardCount]      = {};
    QPushButton*   chartBtn_                                       = nullptr;
    QWidget*       tyreCardsRow_                                   = nullptr;
    QWidget*       tyreChartsRow_                                  = nullptr;
    QPushButton*   tyreCardBtns_[OverviewLayout::TyreCornerCount]  = {};
    QPushButton*   tyreChartBtns_[OverviewLayout::TyreChartCount]  = {};
};
