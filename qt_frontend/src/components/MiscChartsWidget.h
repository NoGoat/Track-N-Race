#pragma once

#include <QWidget>
#include <QPointer>
#include <QString>

class ChartView;
class SessionModel;
class QGridLayout;
class GraphTable;

// Combined or split Misc graphs share one ChartView and incremental data paths.
// G-force reads motionBuf, ride height reads motionExBuf.
class MiscChartsWidget : public QWidget {
    Q_OBJECT
public:
    explicit MiscChartsWidget(QWidget* parent = nullptr);

    void setModel(SessionModel* m);
    void setPlaybackMode(bool on);
    void setWindowSeconds(float seconds);
    // Sections: combined G/ride=0/1, lateral/longitudinal/front/rear=2..5.
    void setSectionVisible(int section, bool on);
    // Swap a section between its chart and a raw-values table; reflows the layout.
    void setSectionViewMode(int section, bool table);

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
    float     windowS_      = 30.0f;
    enum Section { GFORCE = 0, RIDEHEIGHT = 1, LATERAL = 2, LONGITUDINAL = 3,
                   FRONT = 4, REAR = 5, SECTIONS = 6 };
    ChartView* chart_ = nullptr;
    QGridLayout* outer_ = nullptr;   // holds chart_ + any table-mode section tables
    int  xId_[SECTIONS]     = {};
    bool visible_[SECTIONS] = { true, true, false, false, false, false };
    bool tableMode_[SECTIONS] = {};
    GraphTable* table_[SECTIONS] = {};
    int primaryIds_[SECTIONS][2] = {};
    int referenceIds_[SECTIONS][2] = {};
    float lastAddedTime_[SECTIONS] = {};
    float previousTime_[SECTIONS] = {};
    QString dataModeKey_[SECTIONS];
};
