#pragma once

#include <QWidget>
#include <QPointer>
#include <QString>

#include "InputLayout.h"

class ChartView;
class SessionModel;
class QGridLayout;
class GraphTable;

// The Input page's three graphs — gear / throttle-brake / steering — rendered as
// panels of ONE ChartView (a single QRhi render target / repaint) rather
// than three separate widgets. Layout mirrors the old page: gear + throttle-brake
// side by side on top, steering full-width below; any section can be hidden.
class InputChartsWidget : public QWidget {
    Q_OBJECT
public:
    explicit InputChartsWidget(QWidget* parent = nullptr);

    void setModel(SessionModel* m);
    void setPlaybackMode(bool on);
    void setWindowSeconds(float seconds);
    // Sections: gear, signed combined, steering, overlaid combined, accelerator,
    // brake. All are panels of the same render target.
    void setSectionVisible(int section, bool on);
    // Swap a section between its chart and a raw-values table; reflows the layout.
    void setSectionViewMode(int section, bool table);
    void setPageLayout(InputPageLayout layout);
    void setPedalLayout(InputPedalLayout layout);
    void setPedalVisibility(bool accelerator, bool brake);

public slots:
    void setCurrentTime(float t);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void requestRefresh();
    void refresh();
    void rebuildLayout();
    void ensureTable(int section);   // build a section's raw-values table on demand
    float currentTime() const;

    QPointer<SessionModel> model_;
    bool      dirty_        = false;
    bool      playback_     = false;
    float     currentTime_  = 0.0f;
    float     windowS_      = 30.0f;    // toolbar default (tb_windowIdx_=1 = 30s)
    InputPageLayout pageLayout_ = InputPageLayout::Grid;
    InputPedalLayout pedalLayout_ = InputPedalLayout::Combined;
    bool showAccelerator_ = true;
    bool showBrake_ = true;

    enum Section { GEAR = 0, COMBINED = 1, STEERING = 2, COMBINED2 = 3,
                   ACCELERATOR = 4, BRAKE = 5, SECTIONS = 6 };
    ChartView* chart_ = nullptr;
    QGridLayout* outer_ = nullptr;   // grid holding chart_ + any table-mode tables
    int  xId_[SECTIONS]   = {};      // bottom (time) axis id per panel
    bool visible_[SECTIONS] = { true, true, true, false, false, false };
    bool tableMode_[SECTIONS] = {};
    GraphTable* table_[SECTIONS] = {};
    int primaryIds_[SECTIONS][2] = {};
    int referenceIds_[SECTIONS][2] = {};
    float lastAddedTime_[SECTIONS] = {};
    float previousTime_[SECTIONS] = {};
    QString dataModeKey_[SECTIONS];
};
