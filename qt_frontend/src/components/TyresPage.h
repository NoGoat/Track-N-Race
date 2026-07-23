#pragma once

#include <QWidget>

#include <tnrp/rows.h>
#include <tnrp/control_rows.h>

class QTableWidget;
class QStackedWidget;
class QLabel;
class TyreCardsWidget;
class TyreChartsWidget;
class SessionModel;

// Tyres tab — dry/wet tyre-set tables plus a vertical strip of per-corner
// tyre cards, with a toggle to swap the whole page for the tyre trend graphs
// (the same TyreChartsWidget the Overview page uses). Self-contained: MainWindow
// feeds it the latest cached rows via updateTyreCards()/updateTyreSets().
class TyresPage : public QWidget {
    Q_OBJECT

public:
    explicit TyresPage(SessionModel* model, QWidget* parent = nullptr);

    // Refresh the per-corner cards from the latest telemetry + damage rows
    // (nullptr = not yet seen).
    void updateTyreCards(const TelemetryRow* telemetry, const DamageRow* damage);

    // Rebuild the dry/wet set tables from the latest tyre_sets row.
    void updateTyreSets(const tnrp::TyreSetsRow* tyreSets);

    // Playback + chart-window pass-through for the tyre graphs (mirrors OverviewPage).
    void setPlaybackMode(bool on, float currentTime = 0.0f);
    void setCurrentTime(float t);
    void setWindowSeconds(float secs);

    // Per-graph Chart/Table toggle (section: surface=0, inner=1, brake=2, wear=3).
    void setGraphSectionTable(int section, bool table);

    // Per-corner tyre-card Card/Table toggle (corner: FL=0, FR=1, RL=2, RR=3),
    // driven from the Settings "Graphs" tab like every other graph section.
    void setCardTable(int corner, bool table);

private:
    void setGraphsShown(bool on);   // swap allocation ⇄ graphs views

    TyreCardsWidget*  tyreCards_    = nullptr;
    QTableWidget*     drySetsTable_ = nullptr;
    QTableWidget*     wetSetsTable_ = nullptr;
    TyreChartsWidget* tyreCharts_   = nullptr;
    QStackedWidget*   stack_        = nullptr;
    QLabel*           graphsFitted_ = nullptr;   // fitted-tyre summary in the graphs header
    bool              graphsShown_  = false;
};
