#pragma once

#include <QWidget>
#include <QPointer>
#include <QString>

class ChartView;
class SessionModel;
class QGridLayout;
class GraphTable;

class TyreChartsWidget : public QWidget {
    Q_OBJECT
public:
    // grid = true lays the 4 charts out 2×2 (for the fullscreen Tyres view);
    // false (default) keeps them in a 1×4 row (the Overview strip).
    explicit TyreChartsWidget(bool grid = false, QWidget* parent = nullptr);

    void setModel(SessionModel* m);
    void setPlaybackMode(bool on);
    void setWindowSeconds(float seconds);
    void setChartSectionVisible(int i, bool on);
    // Swap a section between its chart and a raw-values table; reflows the layout.
    void setSectionViewMode(int section, bool table);
    // Tyre wear graph mode: true = remaining life (100 - wear), false = accumulated
    // wear. Mirrors the Electron tyreWearMode toggle. Retitles the 4th chart.
    void setTyreLifeMode(bool life);

public slots:
    void setCurrentTime(float t);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void requestRefresh();
    void refresh();
    void rebuildLayout();   // (re)arrange visible panels + dividers
    void ensureTable(int section);   // build a section's raw-values table on demand

    QPointer<SessionModel> model_;
    bool      grid_         = false;    // 2×2 layout vs 1×4 row
    bool      dirty_        = false;
    bool      playback_     = false;
    bool      lifeMode_     = true;     // default to remaining-life, matching Electron
    float     currentTime_  = 0.0f;
    float     windowS_      = 30.0f;    // default view window (matches toolbar default tb_windowIdx_=1 = 30s)

    float currentTime() const;

    float prevEndTime_  = -9999.0f;
    float lastAddedTime_= -9999.0f;
    float lastAddedDamageTime_ = -9999.0f;
    QString dataModeKey_;

    // The four sections (surface / inner / brake / wear) are panels of ONE ChartView
    // — a single QRhi render target / repaint instead of four separate widgets.
    enum Section { SURF = 0, INNER = 1, BRAKE = 2, WEAR = 3, SECTIONS = 4 };
    ChartView* chart_ = nullptr;
    QGridLayout* outer_ = nullptr;   // holds chart_ + any table-mode section tables
    int  xId_[SECTIONS]             = {};   // bottom (time) axis id per panel
    int  yId_[SECTIONS]             = {};
    int  seriesIds_[SECTIONS][4]    = {};   // [section][FL/FR/RL/RR]
    int  referenceIds_[SECTIONS][4] = {};
    bool visible_[SECTIONS]         = { true, true, true, true };
    bool tableMode_[SECTIONS]       = { false, false, false, false };   // false = chart, true = table
    GraphTable* table_[SECTIONS]    = { nullptr, nullptr, nullptr, nullptr };
};
