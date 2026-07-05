#pragma once

#include <QWidget>
#include <QPointer>
#include <cstdint>

class ChartView;
class SessionModel;
class QTimer;

// The Power page's four graphs — power split / ERS harvest / ERS store / fuel —
// rendered as panels of ONE ChartView (a single QCustomPlot / OpenGL context /
// replot) rather than four separate widgets. Laid out 2×2 (split + harvest over
// store + fuel); any section can be hidden. All four read stsBuf.
class PowerChartsWidget : public QWidget {
    Q_OBJECT
public:
    explicit PowerChartsWidget(QWidget* parent = nullptr);

    void setModel(SessionModel* m);
    void setPlaybackMode(bool on);
    void setWindowSeconds(float seconds);
    // Show/hide a section (split=0, harvest=1, store=2, fuel=3); reflows the layout.
    void setSectionVisible(int section, bool on);
    // 4 MJ → 8 MJ harvest Y-axis cap in 2026 (no-op for the other panels).
    void applyHarvestScale(uint16_t format);

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
    float     windowS_      = 30.0f;
    float     prevEndTime_  = -9999.0f;
    float     lastAddedTime_= -9999.0f;

    enum Section { SPLIT = 0, HARVEST = 1, STORE = 2, FUEL = 3, SECTIONS = 4 };
    ChartView* chart_ = nullptr;
    int  xId_[SECTIONS]     = {};
    bool visible_[SECTIONS] = { true, true, true, true };

    int splitIceId_  = -1;
    int splitMgukId_ = -1;
    int harvKId_     = -1;
    int harvHId_     = -1;
    int storeId_     = -1;
    int fuelId_      = -1;
    int harvYId_     = -1;   // harvest Y axis, retargeted by applyHarvestScale
};
