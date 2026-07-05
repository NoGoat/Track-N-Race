#pragma once

#include <QWidget>
#include <QPointer>

class ChartView;
class SessionModel;
class QTimer;

// The Input page's three graphs — gear / throttle-brake / steering — rendered as
// panels of ONE ChartView (a single QCustomPlot / OpenGL context / replot) rather
// than three separate widgets. Layout mirrors the old page: gear + throttle-brake
// side by side on top, steering full-width below; any section can be hidden.
class InputChartsWidget : public QWidget {
    Q_OBJECT
public:
    explicit InputChartsWidget(QWidget* parent = nullptr);

    void setModel(SessionModel* m);
    void setPlaybackMode(bool on);
    void setWindowSeconds(float seconds);
    // Show/hide a section (gear=0, inputs=1, steering=2); reflows the layout.
    void setSectionVisible(int section, bool on);

public slots:
    void setCurrentTime(float t);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void requestRefresh();
    void refresh();
    void rebuildLayout();
    float currentTime() const;

    QPointer<SessionModel> model_;
    QTimer*   refreshTimer_ = nullptr;
    bool      dirty_        = false;
    bool      playback_     = false;
    float     currentTime_  = 0.0f;
    float     windowS_      = 30.0f;    // toolbar default (tb_windowIdx_=1 = 30s)
    float     prevEndTime_  = -9999.0f;
    float     lastAddedTime_= -9999.0f;

    // The three sections are panels of one ChartView (indices match panel ids).
    enum Section { GEAR = 0, INPUTS = 1, STEERING = 2, SECTIONS = 3 };
    ChartView* chart_ = nullptr;
    int  xId_[SECTIONS]   = {};      // bottom (time) axis id per panel
    bool visible_[SECTIONS] = { true, true, true };

    // Series ids.
    int gearId_ = -1;
    int thId_   = -1;
    int brId_   = -1;
    int stId_   = -1;
};
