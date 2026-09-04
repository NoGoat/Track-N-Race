#pragma once

#include <QWidget>
#include <QPointer>
#include <QString>
#include <cstdint>

class ChartView;
class SessionModel;
class QGridLayout;
class GraphTable;

// The Power page's four graphs — power split / ERS harvest / ERS store / fuel —
// rendered as panels of ONE ChartView (a single QRhi render target /
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
    // Swap a section between its chart and a raw-values table; reflows the layout.
    void setSectionViewMode(int section, bool table);
    // 4 MJ → 8 MJ harvest Y-axis cap in 2026 (no-op for the other panels).
    void applyHarvestScale(uint16_t format);
    // Show/hide the MGU-H series and its table column from backend capability data.
    void setMguhVisible(bool visible);

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
    float     lastAddedTime_= -9999.0f;
    QString   dataModeKey_;

    enum Section { SPLIT = 0, HARVEST = 1, STORE = 2, FUEL = 3, SECTIONS = 4 };
    ChartView* chart_ = nullptr;
    QGridLayout* outer_ = nullptr;   // holds chart_ + any table-mode section tables
    int  xId_[SECTIONS]     = {};
    bool visible_[SECTIONS] = { true, true, true, true };
    bool tableMode_[SECTIONS] = { false, false, false, false };   // false = chart, true = table
    GraphTable* table_[SECTIONS] = { nullptr, nullptr, nullptr, nullptr };

    int splitIceId_  = -1;
    int splitMgukId_ = -1;
    int harvKId_     = -1;
    int harvHId_     = -1;
    int storeId_     = -1;
    int fuelId_      = -1;
    int harvYId_     = -1;   // harvest Y axis, retargeted by applyHarvestScale
    int splitIceRefId_ = -1;
    int splitMgukRefId_ = -1;
    int harvKRefId_ = -1;
    int harvHRefId_ = -1;
    int storeRefId_ = -1;
    int fuelRefId_ = -1;
    double harvestFixedMax_ = 4000.0;
    bool mguhVisible_ = true;
};
