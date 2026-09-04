#pragma once

#include <QWidget>
#include <QPointer>
#include <QString>

class ChartView;
class SessionModel;
class QGridLayout;
class GraphTable;

// The Misc page's two graphs — G-force / ride-height — rendered as panels of ONE
// ChartView (a single QRhi render target / repaint) rather than two
// separate widgets. Stacked full-width (G-force over ride height); either can be
// hidden. G-force reads motionBuf, ride height reads motionExBuf.
class MiscChartsWidget : public QWidget {
    Q_OBJECT
public:
    explicit MiscChartsWidget(QWidget* parent = nullptr);

    void setModel(SessionModel* m);
    void setPlaybackMode(bool on);
    void setWindowSeconds(float seconds);
    // Show/hide a section (gforce=0, ride height=1); reflows the layout.
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
    float     prevEndTime_  = -9999.0f;
    // Separate cursors: G-force and ride-height come from different buffers.
    float     lastMotionT_   = -1.0f;
    float     lastMotionExT_ = -1.0f;
    QString   dataModeKey_;

    enum Section { GFORCE = 0, RIDEHEIGHT = 1, SECTIONS = 2 };
    ChartView* chart_ = nullptr;
    QGridLayout* outer_ = nullptr;   // holds chart_ + any table-mode section tables
    int  xId_[SECTIONS]     = {};
    bool visible_[SECTIONS] = { true, true };
    bool tableMode_[SECTIONS] = { false, false };   // false = chart, true = raw table
    GraphTable* table_[SECTIONS] = { nullptr, nullptr };

    int latId_   = -1;
    int longId_  = -1;
    int frontId_ = -1;
    int rearId_  = -1;
    int latRefId_ = -1;
    int longRefId_ = -1;
    int frontRefId_ = -1;
    int rearRefId_ = -1;
};
